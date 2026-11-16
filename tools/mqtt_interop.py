#!/usr/bin/env python3
"""
mqtt_interop.py —— mcu-gateway 的 MQTT 联调测试

测的不是「能连上」，而是两个方向都对得上：

  1. 上行：网关采集的寄存器值能否以合法 JSON 发到 broker
  2. 下行：云端发一条写指令，网关转成 Modbus 写请求，
     从机改了寄存器之后，下一次轮询读到的新值必须出现在上行里

第 2 条才是关键。只测上行的话，一个只会发不会收的客户端也能通过。

用法：
    python3 tools/mqtt_interop.py [--host 127.0.0.1] [--port 1883]
"""
import argparse
import json
import os
import sys
import time

# 本地开发时依赖可能装在独立目录；CI 用系统包，走默认 import
for _cand in (os.environ.get("MG_PYLIB"), "/home/dikl/work/.pylibs"):
    if _cand and os.path.isdir(_cand):
        sys.path.insert(0, _cand)
        break

import paho.mqtt.client as mqtt  # noqa: E402

TOPIC_UP = "gw/uplink"
TOPIC_DOWN = "gw/downlink"

RECV = []
BAD = []

PASS, FAIL = [], []


def check(cond, what, detail=""):
    if cond:
        PASS.append(what)
        print(f"  [PASS] {what}" + (f"  ({detail})" if detail else ""))
    else:
        FAIL.append(what)
        print(f"  [FAIL] {what}" + (f"  ({detail})" if detail else ""))


def on_message(cl, userdata, msg):
    raw = msg.payload.decode("utf-8", "replace")
    try:
        obj = json.loads(raw)
        if not isinstance(obj, dict):
            raise ValueError("not an object")
        RECV.append(obj)
    except Exception as e:
        BAD.append((str(e), raw[:120]))


def make_client(cid):
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id=cid)
    except AttributeError:
        return mqtt.Client(client_id=cid)


def collect(seconds):
    """收集一段时间内的消息，返回这一批"""
    start = len(RECV)
    time.sleep(seconds)
    return RECV[start:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--timeout", type=float, default=25.0)
    args = ap.parse_args()

    print("=== mcu-gateway MQTT interop ===")

    bus = make_client("interop-sub")
    bus.on_message = on_message

    print(f"\nConnect to {args.host}:{args.port}")
    try:
        bus.connect(args.host, args.port, 15)
    except Exception as e:
        print(f"  [FAIL] cannot connect: {e}")
        print("\n  broker not running. start it first:")
        print("    mosquitto -p 1883")
        return 2

    bus.loop_start()
    rc = bus.subscribe(TOPIC_UP, 1)
    check(rc[0] == 0, f"subscribe {TOPIC_UP}")

    # ---------- 阶段 A：上行基线 ----------
    print("\nPhase A: uplink baseline")
    base = collect(2.5)

    modbus = [m for m in base if m.get("type") == "modbus"]
    check(len(modbus) > 0, "uplink carries modbus samples", f"{len(modbus)} msgs")
    check(len(BAD) == 0, "every payload is valid json object",
          f"{len(BAD)} bad" if BAD else "")

    if modbus:
        m = modbus[0]
        for field in ("gw", "ch", "type", "slave", "func", "sid", "ts", "value"):
            check(field in m, f"field '{field}' present")
        check(isinstance(m.get("sid"), int) and m["sid"] > 0,
              "sid assigned (session matched)", f"sid={m.get('sid')}")

    # 找到 slave=1 reg=2 的基线值（模拟从机初值是 reg*100 = 200）
    target = [m for m in modbus
              if m.get("slave") == 1 and m.get("reg") == 2]
    check(len(target) > 0, "sample for slave1 reg2 found")
    base_val = target[0]["value"] if target else None
    check(base_val == 200, "baseline value is 200 (reg*100)", f"got {base_val}")

    # ---------- 阶段 B：下行写入 ----------
    print("\nPhase B: downlink write")
    NEW_VAL = 777
    cmd = {"slave": 1, "reg": 2, "value": NEW_VAL}
    bus.publish(TOPIC_DOWN, json.dumps(cmd), qos=1)
    check(True, f"published {TOPIC_DOWN}: {cmd}")

    after = collect(3.0)

    acks = [m for m in after if m.get("type") == "write_ack"]
    check(len(acks) > 0, "gateway acknowledged the write", f"{len(acks)} ack")
    check(any(a.get("value") == NEW_VAL for a in acks),
          f"ack carries new value {NEW_VAL}")

    # 闭环：下一次轮询读回来的值必须变了
    readback = [m for m in after
                if m.get("type") == "modbus"
                and m.get("slave") == 1 and m.get("reg") == 2]
    check(len(readback) > 0, "gateway polled the written register again")
    check(any(m.get("value") == NEW_VAL for m in readback),
          f"readback reflects the write ({NEW_VAL})",
          f"values seen: {sorted({m.get('value') for m in readback})}")

    # ---------- 阶段 C：异常指令 ----------
    print("\nPhase C: malformed command must not crash")
    bus.publish(TOPIC_DOWN, b"not-a-json", qos=1)
    bus.publish(TOPIC_DOWN, json.dumps({"slave": 1}), qos=1)
    alive = collect(1.5)
    check(len([m for m in alive if m.get("type") == "modbus"]) > 0,
          "gateway keeps polling after bad commands")

    bus.loop_stop()
    bus.disconnect()

    print(f"\nResult: {len(PASS)} passed, {len(FAIL)} failed")
    if FAIL:
        for f in FAIL:
            print(f"  failed: {f}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
