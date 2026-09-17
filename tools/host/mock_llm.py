#!/usr/bin/env python3
"""Scripted OpenAI-compatible mock LLM for the device demo.

It behaves like a tool-calling model without needing an API key:
  * a scheduling request -> cron_add(wake_agent=true)  (agent self-schedules)
  * a patrol request     -> run_shell sweep (free/net_status/df/uptime),
                            then a report built from the REAL tool outputs
  * anything else        -> plain text echo

Stateless: every decision is derived from the message history of the request.
"""
import json
import os
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PATROL_CMDS = ["free", "ps", "df", "uptime"]

SCHEDULE_RE = re.compile(r"(每|每隔|定时|周期).*(分钟|min|秒|小时)")
PATROL_RE = re.compile(r"巡检|patrol|健康|guardian|设备状态")
MEM_RE = re.compile(r"内存|存储|空间|还剩")
TASK_RE = re.compile(r"任务|进程|运行着|跑着")
TIME_RE = re.compile(r"几点|时间")
CANCEL_RE = re.compile(r"取消|停止|不用了|别巡检")
SHOW_RE = re.compile(r"屏幕|显示")

# Pull the device's in-RAM syslog out to the host. The device redirects
# syslog to RAMLOG so it cannot fight PPP for the console, which means every
# error the agent logs is invisible from outside -- including the reason an
# HTTPS request failed. dmesg is the only way to read it, and run_shell is
# the only way to run it.
LOG_RE = re.compile(r"日志|dmesg|log")

# Hostname the diagnostics resolve. Set from ~/.llm_env by the caller if it
# wants a different one; the point is to check the name the agent actually
# connects to, not some well-known one that might resolve differently.
DIAG_HOST = os.environ.get("DIAG_HOST", "api.deepseek.com")

# What each spoken intent needs the device to actually go and do. The commands
# run one per turn and the reply is composed from the real output, so an
# intent that cannot be served says so instead of inventing a plausible
# number -- the whole point of the exercise is that the device really ran it.
# (defined after the composers, below)


def tool_call(name, args):
    return {
        "role": "assistant",
        "content": None,
        "tool_calls": [{
            "id": "call_mock_1",
            "type": "function",
            "function": {
                "name": name,
                "arguments": json.dumps(args, ensure_ascii=False),
            },
        }],
    }, "tool_calls"


def text_reply(text):
    return {"role": "assistant", "content": text}, "stop"


def _tool_text(m):
    """Tool result JSON -> the raw output text."""
    c = m.get("content")
    if not isinstance(c, str):
        return ""
    try:
        obj = json.loads(c)
        if isinstance(obj, dict):
            return str(obj.get("output") or obj.get("error") or "")
    except Exception:
        pass
    return c


def _mem_row(tool_msgs):
    """(total, used, free) in bytes from the `free` table, or None."""
    for m in tool_msgs:
        t = _tool_text(m)
        if "total used free" not in " ".join(t.split()):
            continue
        for row in t.splitlines():
            parts = row.split()
            if len(parts) >= 3 and parts[0].isdigit():
                return int(parts[0]), int(parts[1]), int(parts[2])
    return None


def _uptime_secs(tool_msgs):
    for m in tool_msgs:
        hit = re.search(r"Uptime:\s*([0-9.]+)", _tool_text(m))
        if hit:
            return float(hit.group(1))
    return None


def _data_disk(tool_msgs):
    """(used, total) as printed for /data, or None."""
    for m in tool_msgs:
        t = _tool_text(m)
        if "Filesystem" not in t:
            continue
        for row in t.splitlines():
            if "/data" in row:
                parts = row.split()
                if len(parts) >= 4:
                    return parts[2], parts[1]
    return None


def _task_lines(tool_msgs):
    for m in tool_msgs:
        t = _tool_text(m)
        if re.search(r"\bpppd\b|\bnsh\b", t):
            return [r for r in t.splitlines() if r.strip()]
    return []


def compose_report(tool_msgs):
    lines = []

    mem = _mem_row(tool_msgs)
    if mem:
        total, used, free = mem
        lines.append("- 内存: 空闲 %.2f MB / 共 %.2f MB (已用 %.2f MB)"
                     % (free / 1048576.0, total / 1048576.0,
                        used / 1048576.0))

    secs = _uptime_secs(tool_msgs)
    if secs is not None:
        lines.append("- 运行时长: %d 分 %d 秒" % (secs // 60, secs % 60))

    disk = _data_disk(tool_msgs)
    if disk:
        lines.append("- 存储 /data: 已用 %s / 共 %s" % disk)

    tasks = _task_lines(tool_msgs)
    if tasks:
        up = "pppd 在运行" if "pppd" in "\n".join(tasks) else "未发现 pppd"
        lines.append("- 系统: %d 个任务, %s" % (max(0, len(tasks) - 1), up))

    body = "\n".join(lines) if lines else "(未采集到遥测)"
    return ("【设备巡检】\n" + body +
            "\n巡检结论: 各项指标正常，无需处理。")


def compose_mem(tool_msgs):
    lines = []

    mem = _mem_row(tool_msgs)
    if mem:
        total, used, free = mem
        lines.append("内存: 空闲 %.2f MB / 共 %.2f MB (已用 %.2f MB)"
                     % (free / 1048576.0, total / 1048576.0,
                        used / 1048576.0))

    disk = _data_disk(tool_msgs)
    if disk:
        lines.append("存储 /data: 已用 %s / 共 %s" % disk)

    return "【设备资源】\n" + ("\n".join(lines) if lines else "(未采集到数据)")


def compose_tasks(tool_msgs):
    tasks = _task_lines(tool_msgs)
    if not tasks:
        return "【任务列表】\n(未采集到数据)"
    return ("【任务列表】共 %d 项\n" % max(0, len(tasks) - 1)
            + "\n".join(tasks[:12]))


def compose_time(tool_msgs):
    for m in tool_msgs:
        t = _tool_text(m).strip()
        if t:
            return "现在时间是 " + t.splitlines()[0].strip()
    return "读取系统时间失败。"


# Which shell commands a spoken intent needs, in order. First match wins, so
# the more specific intents come first.
SWEEPS = (
    (PATROL_RE, PATROL_CMDS, compose_report),
    (MEM_RE, ["free", "df"], compose_mem),
    (TASK_RE, ["ps"], compose_tasks),
    (TIME_RE, ["date"], compose_time),
)


def build_reply(req):
    msgs = req.get("messages", [])
    tool_names = [t.get("function", {}).get("name")
                  for t in req.get("tools", []) if isinstance(t, dict)]

    # Scope the decision to the CURRENT turn: everything after the last
    # user message. History from earlier turns must not leak in — the same
    # chat_id is reused by the cron wake-up, so old tool calls stay in the
    # session history.
    last_user_idx = -1
    for i, m in enumerate(msgs):
        if m.get("role") == "user":
            last_user_idx = i
    user = ""
    if last_user_idx >= 0:
        c = msgs[last_user_idx].get("content")
        user = c if isinstance(c, str) else ""

    turn = msgs[last_user_idx + 1:] if last_user_idx >= 0 else msgs
    tool_msgs = [m for m in turn if m.get("role") == "tool"]
    done = len(tool_msgs)

    called = []
    for m in turn:
        if m.get("role") == "assistant":
            for tc in (m.get("tool_calls") or []):
                called.append(tc.get("function", {}).get("name"))

    # Scheduling first: the request also mentions 巡检, so it would otherwise
    # be swallowed by the patrol branch below.
    if "cron_add" in called:
        return text_reply("好的，已设置每 2 分钟自动巡检，巡检结果会主动推送给你。")

    if SCHEDULE_RE.search(user) and done == 0 and "cron_add" in tool_names:
        return tool_call("cron_add", {
            "name": "device_patrol",
            "schedule_type": "every",
            "interval_s": 120,
            "message": "请巡检当前设备健康状态，并按 device-guardian 技能的格式输出简报。",
            "wake_agent": True,
        })

    # Cancelling: look at what is scheduled, then remove it a job at a time.
    # The listing carries the ids; there is no wildcard remove.
    removed = []
    for m in turn:
        if m.get("role") != "assistant":
            continue
        for tc in (m.get("tool_calls") or []):
            fn = tc.get("function", {})
            if fn.get("name") == "cron_remove":
                try:
                    removed.append(json.loads(fn.get("arguments") or "{}")
                                   .get("job_id"))
                except Exception:                    # noqa: BLE001
                    pass

    if "cron_remove" in called:
        return text_reply("好的，已经取消设备上的定时任务。")

    if CANCEL_RE.search(user) and not called and "cron_list" in tool_names:
        return tool_call("cron_list", {})

    if "cron_list" in called:
        listing = " ".join(_tool_text(m) for m in tool_msgs)
        ids = [i for i in re.findall(r"\[([A-Za-z0-9_-]+)\]", listing)
               if i not in removed]
        if not ids:
            return text_reply("设备上当前没有定时任务。")
        if "cron_remove" in tool_names:
            return tool_call("cron_remove", {"job_id": ids[0]})
        return text_reply("有 %d 个定时任务在运行。" % len(ids))

    # Device diagnostics: name resolution first, then the in-RAM syslog.
    # Between them they say whether a failing HTTPS call is failing to find
    # the host or failing after it got there.
    if LOG_RE.search(user) and "run_shell" in tool_names:
        # A raw address first, then a name. Together they separate the two
        # ways "cannot reach the internet" happens here: no route off the
        # link (raw IP fails too) versus route but no working resolution
        # (raw IP succeeds, name fails). The device reaches the host at
        # 192.168.223.1 either way, so that proves nothing on its own.
        if done == 0:
            return tool_call("run_shell", {"command": "ping -c 2 114.114.114.114"})
        if done == 1:
            return tool_call("run_shell", {"command": "ping -c 2 " + DIAG_HOST})
        parts = []
        for m in tool_msgs:
            t = _tool_text(m).strip()
            if t:
                parts.append(t[-1800:])
        return text_reply("\n----\n".join(parts) if parts
                          else "(no diagnostic output)")


    # "Put X on the screen". The reply itself is what the watch displays, so
    # answer with the text that was asked for rather than a description of
    # having shown it.
    if SHOW_RE.search(user):
        text = re.split(r"[：:]", user, maxsplit=1)[-1].strip()
        return text_reply(text or user)

    # Everything else that needs real device data: run the sweep for this
    # intent, one command per turn, then answer from what came back.
    for rx, cmds, composer in SWEEPS:
        if rx.search(user) and "run_shell" in tool_names:
            if done < len(cmds):
                return tool_call("run_shell", {"command": cmds[done]})
            return text_reply(composer(tool_msgs))

    return text_reply("[mock-llm] 收到: " + user[:120])


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length)

        # Time service, mirroring the real-API proxy. The device's clock comes
        # up reading 2036 and its own fallback is an HTTPS HEAD to a public
        # host, which does not work over this link -- so it asks its backend
        # instead, and the backend answers with an RFC-1123 date.
        if self.path.startswith("/time"):
            from email.utils import formatdate
            stamp = formatdate(usegmt=True).encode()
            print("[mock-llm] <- time request, answering %s" % stamp.decode(),
                  flush=True)
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(stamp)))
            self.end_headers()
            self.wfile.write(stamp)
            return

        try:
            req = json.loads(raw)
        except Exception:
            req = {}

        try:
            message, finish = build_reply(req)
        except Exception as exc:
            message, finish = {"role": "assistant",
                               "content": "mock error: %s" % exc}, "stop"

        last_user = ""
        for m in reversed(req.get("messages", [])):
            if m.get("role") == "user" and isinstance(m.get("content"), str):
                last_user = m["content"]
                break
        print("[mock-llm] <- " + repr(last_user[:160]), flush=True)
        if os.environ.get("MOCK_DEBUG"):
            for m in req.get("messages", []):
                if m.get("role") == "tool":
                    print("    [tool] %r" % (str(m.get("content"))[:150],),
                          flush=True)
        if os.environ.get("MOCK_DEBUG"):
            try:
                with open(os.path.expanduser("~/mock_raw.jsonl"), "a",
                          encoding="utf-8") as fh:
                    fh.write(raw.decode("utf-8", "replace")[:8000] + "\n")
            except Exception:
                pass
        if os.environ.get("MOCK_DEBUG"):
            try:
                with open(os.path.expanduser("~/mock_requests.jsonl"), "a",
                          encoding="utf-8") as fh:
                    fh.write(json.dumps([
                        {"role": m.get("role"),
                         "calls": [tc.get("function", {}).get("name")
                                   for tc in (m.get("tool_calls") or [])],
                         "content": (m.get("content") or "")[:120]}
                        for m in req.get("messages", [])
                    ], ensure_ascii=False) + "\n")
            except Exception:
                pass
        if message.get("tool_calls"):
            tc = message["tool_calls"][0]["function"]
            print("    -> tool_call %s %s" % (tc["name"], tc["arguments"][:140]),
                  flush=True)
        else:
            print("    -> text " + (message.get("content") or "")[:140].replace("\n", " "),
                  flush=True)

        body = json.dumps({
            "id": "chatcmpl-mock",
            "object": "chat.completion",
            "created": 0,
            "model": "mock-model",
            "choices": [{"index": 0, "message": message, "finish_reason": finish}],
            "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2},
        }, ensure_ascii=False).encode("utf-8")

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass

    def log_error(self, fmt, *args):
        # Keep server/protocol errors visible: without this, malformed or
        # oversized requests fail silently and look like "the device never
        # called us".
        print("[mock-llm] ERROR " + (fmt % args), flush=True)

    def handle_one_request(self):
        try:
            super().handle_one_request()
        except Exception as exc:
            print("[mock-llm] EXC %s: %s" % (type(exc).__name__, exc), flush=True)
            raise


class LoggingHTTPServer(ThreadingHTTPServer):
    """Threaded on purpose.
    The device's agent makes several LLM round trips per turn (one per tool
    call), and a single-threaded server answers them one at a time. Under any
    real load the queue grows, and while it waits the agent is parked in a
    blocking socket read that holds up the device's whole network stack --
    the audio uplink and ping both stop, and the device looks hung when it is
    only waiting for us. Answering concurrently keeps that read short.
    """

    daemon_threads = True
    """Log every accepted TCP connection: a request that never completes is
    otherwise invisible, and looks identical to a request never sent."""

    def get_request(self):
        sock, addr = super().get_request()
        print("[mock-llm] TCP connect from %s:%d" % addr, flush=True)
        return sock, addr


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print("[mock-llm] listening on 0.0.0.0:%d" % port, flush=True)
    LoggingHTTPServer(("0.0.0.0", port), Handler).serve_forever()
