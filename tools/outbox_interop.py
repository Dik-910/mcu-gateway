#!/usr/bin/env python3
"""
outbox_interop.py —— QoS1 与断线补发的端到端验证

要证明的不是「能连上 broker」，而是三件在真实现场才会暴露的事：

  A. 正常链路：消息按序到达，载荷是合法 JSON
  B. broker 掉线：网关继续采集，数据落盘；broker 回来后**先补旧数据**
     （判断依据是补发消息的 ts 明显落后于当前时刻 —— 新采的数据 ts 不会回头）
  C. 网关自己重启：没发出去的数据还在盘上，新进程起来后接着发
     （判断依据是新进程的 ts 从 0 开始数，出现大 ts 只可能来自恢复的旧数据）

脚本自己起停 mosquitto，不依赖外部已启动的 broker。

用法：
    python3 tools/outbox_interop.py [--port 18831]
"""
import argparse
import atexit
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time

for _cand in (os.environ.get("MG_PYLIB"), "/home/dikl/work/.pylibs"):
    if _cand and os.path.isdir(_cand):
        sys.path.insert(0, _cand)
        break

import paho.mqtt.client as mqtt  # noqa: E402

TOPIC_UP = "gw/uplink"
OUTBOX = ".outbox_interop.bin"
MOSQ_CONF = ".outbox_interop.mosq.conf"
GW = "./gw-mqtt"

RECV = []
PASS, FAIL = [], []


def check(cond, what, detail=""):
    if cond:
        PASS.append(what)
        print(f"  [PASS] {what}" + (f"  ({detail})" if detail else ""))
    else:
        FAIL.append(what)
        print(f"  [FAIL] {what}" + (f"  ({detail})" if detail else ""))

def make_client(cid):
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id=cid)
    except AttributeError:
        return mqtt.Client(client_id=cid)


def on_message(cl, userdata, msg):
    try:
        obj = json.loads(msg.payload.decode("utf-8", "replace"))
    except Exception:
        return
    if isinstance(obj, dict) and isinstance(obj.get("ts"), int):
        RECV.append(obj)


SUBACK = []


def on_subscribe(cl, userdata, mid, granted_qos):
    SUBACK.append(mid)


def wait_port(port, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), 0.3):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def find_mosquitto():
    for cand in (shutil.which("mosquitto"), "/usr/sbin/mosquitto", "/usr/bin/mosquitto"):
        if cand and os.path.exists(cand):
            return cand
    return None


def start_broker(path, port):
    """
    自己写一份最小配置再启动。

    直接 `mosquitto -p PORT` 在容器/沙箱里会失败：它默认降权到 mosquitto
    用户，而受限环境禁止 setgroups（"Error setting groups whilst dropping
    privileges"）。配置里显式 `user root` 就跳过降权这一步。
    broker 只监听 127.0.0.1，且开的是匿名访问，仅用于本地验证。
    """
    with open(MOSQ_CONF, "w", encoding="utf-8") as f:
        f.write(f"listener {port} 127.0.0.1\n")
        f.write("allow_anonymous true\n")
        f.write("user root\n")
        f.write("persistence false\n")

    p = subprocess.Popen([path, "-c", MOSQ_CONF],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not wait_port(port, 8):
        p.kill()
        raise RuntimeError(f"broker did not listen on {port}")
    return p


def stop_broker(p):
    if p is None:
        return
    p.send_signal(signal.SIGTERM)
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill()


def start_gw(port, seconds, logpath):
    env = os.environ.copy()
    env["MG_MQTT_ADDR"] = f"tcp://127.0.0.1:{port}"
    env["MG_MQTT_CID"] = "gw-outbox-test"
    env["MG_OUTBOX_FILE"] = OUTBOX
    log = open(logpath, "ab")
    return subprocess.Popen([GW, "--seconds", str(seconds)], env=env,
                            stdout=log, stderr=subprocess.STDOUT)


def stop_gw(p, hard=False):
    if p is None:
        return
    if hard:
        p.send_signal(signal.SIGKILL)
    try:
        p.wait(timeout=8)
    except subprocess.TimeoutExpired:
        p.kill()


def new_subscriber(port, old=None):
    """
    新建一个订阅端。

    为什么每轮都换一个新的：broker 重启后订阅关系全丢（persistence false），
    老客户端要等自己的重连 + 重订阅，和网关的重连退避是两条独立的赛跑，
    谁先到不确定 —— 抢输了就会漏掉补发的头几条，测试变成看运气。

    为什么等 SUBACK 而不是睡固定时间：subscribe 是异步的，on_message 之前
    必须确认 broker 已经把这个会话的主题登记进路由表，否则补发的头几条
    会被 broker 当作「无人订阅」直接丢掉 —— 那不是网关的问题，但会让
    测试结果看起来像丢数据。
    """
    if old is not None:
        try:
            old.loop_stop()
            old.disconnect()
        except Exception:
            pass

    SUBACK.clear()
    c = make_client(f"outbox-sub-{int(time.time() * 1000) % 1000000}")
    c.on_message = on_message
    c.on_subscribe = on_subscribe
    c.connect("127.0.0.1", port, 15)
    c.loop_start()
    c.subscribe(TOPIC_UP, 1)

    deadline = time.time() + 3
    while not SUBACK and time.time() < deadline:
        time.sleep(0.02)

    return c


def cleanup(procs):
    for p in procs:
        try:
            if p and p.poll() is None:
                p.kill()
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=18831)
    ap.add_argument("--log", default=".outbox_interop.gw.log")
    args = ap.parse_args()

    print("=== mcu-gateway outbox interop ===")

    mosq = find_mosquitto()
    if mosq is None:
        print("  SKIP: mosquitto not found (apt install mosquitto)")
        return 0
    if not os.path.exists(GW):
        print(f"  SKIP: {GW} not built (make gw-mqtt)")
        return 0

    for f in (OUTBOX, args.log, MOSQ_CONF):
        if os.path.exists(f):
            os.unlink(f)

    live = []
    atexit.register(lambda: cleanup(live))

    broker = start_broker(mosq, args.port)
    live.append(broker)

    sub = new_subscriber(args.port)

    # ---------- A. 正常链路 ----------
    print("\nPhase A: uplink with broker up")
    gw = start_gw(args.port, 24, args.log)
    live.append(gw)
    time.sleep(4)
    base = len(RECV)
    check(base > 0, "messages arrive while the broker is up", f"{base} msgs")
    if base > 0:
        ts = [m["ts"] for m in RECV[:base]]
        check(all(b >= a for a, b in zip(ts, ts[1:])), "timestamps are non-decreasing")
        check(all("ch" in m and "type" in m for m in RECV[:base]),
              "payload keeps the gateway fields")
    last_ts_before = RECV[-1]["ts"] if base else 0

    # ---------- B. broker 掉线 -> 积压 -> 补发 ----------
    print("\nPhase B: broker down, gateway keeps sampling")
    stop_broker(broker)
    live.remove(broker)
    time.sleep(4.0)                      # 这段时间的数据只能进 outbox
    check(os.path.exists(OUTBOX), "outbox file exists on disk")
    check(os.path.exists(OUTBOX) and os.path.getsize(OUTBOX) > 0,
          "outbox file is non-empty",
          f"{os.path.getsize(OUTBOX)} bytes" if os.path.exists(OUTBOX) else "missing")

    # broker 回来之后先把订阅端就位，再等网关自己按退避重连
    broker = start_broker(mosq, args.port)
    live.append(broker)
    sub = new_subscriber(args.port, sub)

    mark = len(RECV)
    time.sleep(8)

    recovered = RECV[mark:]
    check(len(recovered) >= 5, "backlog is delivered after reconnect",
          f"{len(recovered)} msgs")
    if recovered:
        first = recovered[0]["ts"]
        # 补发的数据 ts 应紧接断线前最后一条；若这里是新采的数据，
        # ts 会比断线前大 4 秒以上
        check(first - last_ts_before < 1500,
              "the first message after reconnect is the backlog, not fresh data",
              f"gap={first - last_ts_before} ms")
        backfill = [m for m in recovered if m["ts"] <= last_ts_before + 3500]
        check(len(backfill) >= 5, "backlog covers the outage window",
              f"{len(backfill)} msgs within 3.5s of the outage")

        # 断线窗口里如果有洞，说明补发不连续 —— 比「第一条晚到了」更值得暴露
        ts = sorted(m["ts"] for m in recovered if m["ts"] <= first + 4000)
        holes = [(b - a, a, b) for a, b in zip(ts, ts[1:]) if b - a > 500]
        check(len(holes) == 0, "no hole larger than 500ms in the backlog",
              f"{len(holes)} hole(s), worst {max(holes)[0]}ms" if holes else "")

    # ---------- C. 网关重启 -> 盘上数据还在 ----------
    print("\nPhase C: gateway restart while broker is down")
    stop_broker(broker)
    live.remove(broker)
    stop_gw(gw, hard=True)
    live.remove(gw)
    time.sleep(3.0)                      # 这段时间没有进程在跑

    broker = start_broker(mosq, args.port)
    live.append(broker)
    sub = new_subscriber(args.port, sub)
    mark = len(RECV)

    gw = start_gw(args.port, 10, args.log)
    live.append(gw)
    time.sleep(6)

    after = RECV[mark:]
    check(len(after) > 0, "messages flow again after the restart", f"{len(after)} msgs")
    if after:
        # 新进程的 tick 计时从 0 开始，6 秒内不可能出现 5000 以上的 ts；
        # 出现就说明这些记录是从盘上恢复的旧数据
        recovered_old = [m for m in after if m["ts"] > 5000]
        check(len(recovered_old) > 0,
              "records from before the restart are replayed from the outbox file",
              f"{len(recovered_old)} msgs, max ts={max(m['ts'] for m in after)}")

    stop_gw(gw)
    live.remove(gw)
    stop_broker(broker)
    live.remove(broker)
    try:
        sub.loop_stop()
    except Exception:
        pass

    print(f"\nResult: {len(PASS)} passed, {len(FAIL)} failed")
    if FAIL:
        print(f"gateway log: {args.log}")
    return 0 if not FAIL else 1


if __name__ == "__main__":
    sys.exit(main())
