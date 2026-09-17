#!/usr/bin/env python3
"""Drive mock_llm.build_reply() through the tool loop without a device.

Each spoken command should end in a reply built from real tool output, and
should pick the right tools on the way. Checking that here costs seconds;
checking it on the board costs a build, a flash and a link that only holds
for a few minutes.

    python3 mock_test.py
"""
import json
import sys

sys.path.insert(0, "/home/Oliweitz")
import mock_llm  # noqa: E402

TOOLS = [{"function": {"name": n}} for n in
         ("run_shell", "cron_add", "cron_list", "cron_remove")]

FREE = """\
             total       used       free    largest
          524288     123456     390000     200000"""

PS = """\
  PID  PRI  POLICY  TYPE   NPARMS   STACK   USED  CPU  COMMAND
    0    0  FIFO    TASK        0     2048   1100  0.0  nsh
    1    1  FIFO    TASK        0     2048   2200  1.0  pppd
    2    2  FIFO    TASK        0     4096   3300  0.5  agent"""

DF = """\
Filesystem      Size    Used   Avail  Use%
/data            2.0M    0.3M    1.7M   15%"""

UPTIME = "Uptime: 123.45 seconds"
DATE = "2026-09-16 17:52:03"

SHELL = {
    "free": FREE, "ps": PS, "df": DF, "uptime": UPTIME, "date": DATE,
}

LIST_ONE = 'Scheduled jobs (1):\n  1. [abc123] "device_patrol" every 120s, ' \
           'enabled, next=99, last=0, ch=websocket:ws_5\n'
LIST_NONE = "No cron jobs scheduled."


def drive(user, listing=LIST_ONE, verbose=False):
    msgs = [{"role": "user", "content": user}]
    ran = []
    for _ in range(10):
        msg, finish = mock_llm.build_reply(
            {"messages": msgs, "tools": TOOLS})
        msgs.append(msg)
        if finish == "stop":
            return ran, msg.get("content")
        fn = msg["tool_calls"][0]["function"]
        name = fn["name"]
        args = json.loads(fn["arguments"])
        ran.append((name, args.get("command") or args.get("job_id") or ""))
        if name == "run_shell":
            out = SHELL.get(args.get("command"), "")
        elif name == "cron_list":
            out = listing
        else:
            out = "ok"
        msgs.append({"role": "tool",
                     "content": json.dumps({"output": out})})
        if verbose:
            print("    -> %s %s" % (name, args))
    return ran, "(tool loop did not terminate)"


CASES = [
    "立即巡检设备，采集内存、任务数和存储数据",
    "看一下设备当前的内存和存储使用情况",
    "看看设备上正在运行哪些任务",
    "现在几点了",
    "在屏幕上显示一句话：你好，我是随身AI管家",
    "取消所有定时任务",
    "随便说点什么",
]

failed = 0
for case in CASES:
    ran, reply = drive(case)
    print("· %s" % case)
    print("   tools: %s" % (", ".join("%s(%s)" % t for t in ran) or "(none)"))
    print("   reply: %s" % reply.replace("\n", "\n          "))
    if "未采集到" in reply or "读取系统时间失败" in reply:
        failed += 1
        print("   !! no real data made it into the reply")
    assert "run_shell" not in [t[0] for t in ran] or ran, "sweep ran nothing"
    print()

print("%d/%d commands produced a reply without real device data"
      % (failed, len(CASES)))
sys.exit(1 if failed else 0)
