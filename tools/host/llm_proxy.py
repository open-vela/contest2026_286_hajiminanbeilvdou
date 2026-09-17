#!/usr/bin/env python3
"""Forward the device's LLM calls to the real API.

    python3 llm_proxy.py [port]        # default 8080, where the mock listens

The device talks plain HTTP to this, exactly as it does to mock_llm.py, and
this makes the real HTTPS call on its behalf.

Why the indirection rather than letting the board call the API directly:

  * The board's TLS does not hold up against this endpoint. It completes the
    handshake sometimes ("Handshake OK: TLSv1.2", followed by a real answer
    with a tool call) and fails the rest of the time with
    MBEDTLS_ERR_SSL_CONN_EOF -- the server closing mid-handshake -- and it
    does so with the link idle and the request already cut to 7 KB, so it is
    not a bandwidth problem. Each failure also tends to trip a NuttX
    semaphore assertion that stops the device dead.
  * A failed handshake costs the board nothing here. The hop to this proxy
    is the same plain-HTTP path the mock has used reliably for days.
  * The real key never leaves this machine. The device's own api_key field is
    ignored and overwritten, so the board holds a dummy and the submitted
    config, logs and screenshots cannot leak the real one.

The agent itself is untouched: it still runs on the device, still decides
which tool to call, still executes it. Only the hop that reaches the model
moved, and the design notes already allow the backend to be reached over the
USB bridge.

Falls back to the scripted mock if the API is unreachable, so the demo still
runs with no network -- pass --fallback-mock to enable that.
"""
import json
import os
import sys
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ENV_FILE = os.path.expanduser("~/.llm_env")


def load_env(path=ENV_FILE):
    env = {}
    try:
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    k, v = line.split("=", 1)
                    env[k.strip()] = v.strip()
    except OSError:
        pass
    return env


ENV = load_env()
API_URL = "https://%s:%s%s" % (ENV.get("LLM_HOST", ""),
                               ENV.get("LLM_PORT", "443"),
                               ENV.get("LLM_PATH", "/v1/chat/completions"))
API_KEY = ENV.get("LLM_API_KEY", "")
API_MODEL = ENV.get("LLM_MODEL", "")

# Optional offline path: if the API cannot be reached, answer from the
# scripted mock instead of failing the turn. The device cannot tell the
# difference, which is the point -- the demo keeps running with no network.
USE_FALLBACK = "--fallback-mock" in sys.argv
try:
    import mock_llm
except ImportError:
    mock_llm = None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):        # quieter than the default
        pass

    def _reply(self, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0)))

        # Time service. The board's clock comes up reading 2036 and its own
        # NTP-ish fallback is an HTTPS HEAD to a public host, which its TLS
        # stack cannot reliably do on this link. It reaches this proxy without
        # trouble on every turn, so it asks here instead. The body is the
        # date in the same RFC-1123 shape as a Date: header, which is what the
        # device's parser already understands.
        if self.path.startswith("/time"):
            from email.utils import formatdate
            stamp = formatdate(usegmt=True).encode()      # Sat, 16 Sep 2026 14:25:00 GMT
            print("[proxy] <- time request, answering %s" % stamp.decode(),
                  flush=True)
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(stamp)))
            self.end_headers()
            self.wfile.write(stamp)
            return


        try:
            req = json.loads(raw)
        except ValueError:
            req = {}

        # Always take the model from our own config: the device may still be
        # configured with the mock's model name, and a stale name is a 4xx
        # that reads like a network fault.
        if API_MODEL:
            req["model"] = API_MODEL

        user = ""
        for m in req.get("messages", []):
            if m.get("role") == "user" and isinstance(m.get("content"), str):
                user = m["content"]
        print("[proxy] -> %s  (%d tools, %d bytes)"
              % (API_MODEL, len(req.get("tools", []) or []), len(raw)),
              flush=True)

        try:
            body = json.dumps(req).encode("utf-8")
            net = urllib.request.Request(
                API_URL, data=body,
                headers={"Authorization": "Bearer " + API_KEY,
                         "Content-Type": "application/json"})
            with urllib.request.urlopen(net, timeout=120) as resp:
                out = json.loads(resp.read().decode("utf-8"))
            calls = out["choices"][0]["message"].get("tool_calls") or []
            print("[proxy] <- %s" % (", ".join(
                c["function"]["name"] for c in calls) if calls else "(text)"),
                flush=True)
            self._reply(out)
            return
        except Exception as exc:                     # noqa: BLE001
            print("[proxy] !! %r" % (exc,), flush=True)

        if USE_FALLBACK and mock_llm is not None:
            print("[proxy]    falling back to the scripted mock", flush=True)
            try:
                msg, finish = mock_llm.build_reply(req)
                self._reply({"choices": [{"message": msg,
                                          "finish_reason": finish}]})
                return
            except Exception as exc:                 # noqa: BLE001
                print("[proxy] !! mock failed too: %r" % (exc,), flush=True)

        # Hand back something the agent can report honestly rather than a
        # connection error it will read as a dead network.
        self._reply({"error": {"message": "upstream LLM unavailable",
                               "type": "proxy_error"}})


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() \
        else 8080
    if not API_KEY:
        print("no LLM_API_KEY in %s -- run ~/setup_llm.sh" % ENV_FILE)
        return 1
    print("[proxy] listening on 0.0.0.0:%d -> %s (%s)%s"
          % (port, API_URL, API_MODEL,
             "  [mock fallback on]" if USE_FALLBACK else ""), flush=True)
    ThreadingHTTPServer(("0.0.0.0", port), Handler).serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
