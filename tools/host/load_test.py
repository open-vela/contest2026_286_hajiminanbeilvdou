#!/usr/bin/env python3
"""Hammer the device with agent turns and watch for it to fall over.

The one crash we caught happened during a burst of turns while audio was
streaming, so this reproduces load rather than a specific input: each turn
drives several LLM round trips and shell invocations on the device, which is
where the mutex traffic is.

Reports the first failure and stops, because a dead device answers nothing
afterwards and the point is the moment it dies.

    python3 load_test.py [seconds] [commands_per_minute]
"""
import asyncio
import json
import subprocess
import sys
import time

import websockets

URL = "ws://192.168.223.2:28789"
SECS = int(sys.argv[1]) if len(sys.argv) > 1 else 300
PER_MIN = int(sys.argv[2]) if len(sys.argv) > 2 else 20

# "drain"  - keep reading between commands, like the real bridge (default)
# "silent" - go quiet between commands: a peer that stops reading, which is
#            the case that used to stall the whole device
MODE = sys.argv[3] if len(sys.argv) > 3 else "drain"

# Spread across the intents so the device alternates between shell sweeps,
# cron bookkeeping and plain replies rather than repeating one path.
COMMANDS = [
    "立即巡检设备，采集内存、任务数和存储数据",
    "看一下设备当前的内存和存储使用情况",
    "看看设备上正在运行哪些任务",
    "现在几点了",
    "在屏幕上显示一句话：压力测试中",
]


def alive() -> bool:
    r = subprocess.run(["ping", "-c", "1", "-W", "2", "192.168.223.2"],
                       capture_output=True)
    return r.returncode == 0


async def main() -> int:
    t0 = time.time()
    sent = 0
    fails = 0

    async with websockets.connect(URL, max_size=None) as ws:
        print("[load] connected; %.0fs at %d cmds/min" % (SECS, PER_MIN))
        while time.time() - t0 < SECS:
            cmd = COMMANDS[sent % len(COMMANDS)]
            try:
                await ws.send(json.dumps(
                    {"type": "message", "content": cmd, "chat_id": "load"}))

                # Keep reading for the whole gap to the next command rather
                # than sending and then going quiet. A client that stops
                # reading lets the device's send buffer fill, and when that
                # send blocks it does so inside the network stack, which
                # stalls *every* socket on the device -- including the audio
                # stream to a completely different client. Pacing by reading,
                # the way the real bridge does, is what makes this a test of
                # the server rather than of the client.
                interval = 60.0 / PER_MIN
                if MODE == "silent":
                    # Deliberately stop reading: the device's send buffer to
                    # this client fills, and without a bound on send() that
                    # parks the network stack for everyone.
                    await asyncio.sleep(interval)
                else:
                    deadline = time.time() + interval
                    while time.time() < deadline:
                        try:
                            await asyncio.wait_for(
                                ws.recv(),
                                timeout=min(1.0, deadline - time.time()))
                        except asyncio.TimeoutError:
                            continue
                sent += 1
            except Exception as exc:              # noqa: BLE001
                print("[load] send/recv failed after %d cmds: %r" % (sent, exc))
                fails += 1
                break

            if sent % 10 == 0:
                ok = alive()
                print("[load] %d cmds, t=%.0fs, device %s"
                      % (sent, time.time() - t0, "OK" if ok else "DEAD"))
                if not ok:
                    fails += 1
                    break

            await asyncio.sleep(60.0 / PER_MIN)

    ok = alive()
    print("[load] done: %d commands, device %s, %d failure(s)"
          % (sent, "OK" if ok else "DEAD", fails))
    return 1 if fails else 0


sys.exit(asyncio.run(main()))
