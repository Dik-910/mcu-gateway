#!/usr/bin/env python3
"""
run_broker.py —— 本地开发用的最小 MQTT broker

为什么仓库里带这个：本机没有 mosquitto（不想为联调改系统环境），
而用 amqtt（独立的 Python broker 实现）比「自己写一个 broker 再测自己的客户端」
靠谱得多 —— 后者是自己跟自己对测，格式错了两边一起错。

生产/CI 环境应该用 mosquitto：
    sudo apt install mosquitto
    mosquitto -p 1883

依赖：pip install amqtt
"""
import argparse
import asyncio
import sys

try:
    from amqtt.broker import Broker
except ImportError:
    print("amqtt not installed:  pip install amqtt", file=sys.stderr)
    sys.exit(1)


async def main(host, port):
    cfg = {
        "listeners": {"default": {"type": "tcp", "bind": f"{host}:{port}"}},
        "sys_interval": 0,
        "topic-check": {"enabled": False},
    }
    broker = Broker(cfg)
    await broker.start()
    print(f"broker listening on {host}:{port}", flush=True)
    await asyncio.Event().wait()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1883)
    a = ap.parse_args()
    try:
        asyncio.run(main(a.host, a.port))
    except KeyboardInterrupt:
        pass
