#!/usr/bin/env python3
"""Headless agent + PPP, race-free, no interactive step.

    ai_agent < /dev/null &      # headless agent (no CLI, no exit-crash)
    pppd                        # FOREGROUND: NSH blocks in waitpid,
                                # pppd owns the console exclusively

LLM config is done later over REST (PUT /api/config).
"""
import os
import signal
import sys
import time

import serial

PORT = os.environ.get('DEV_PORT', os.path.expanduser('~/ttyHS'))
BAUD = 1000000
PIDFILE = os.path.expanduser('~/bridge.pid')


def hard_reset():
    with open(PIDFILE) as f:
        pid = int(f.read().strip())
    os.kill(pid, signal.SIGUSR1)


def drain(ser, seconds, echo=True):
    buf = bytearray()
    end = time.time() + seconds
    while time.time() < end:
        data = ser.read(4096)
        if data:
            buf.extend(data)
    if echo and buf:
        sys.stdout.write(buf.decode('utf-8', errors='replace'))
        sys.stdout.flush()
    return bytes(buf)


def wait_for(ser, needle, timeout):
    buf = bytearray()
    end = time.time() + timeout
    while time.time() < end:
        data = ser.read(4096)
        if data:
            buf.extend(data)
            if needle.encode() in buf:
                return True, bytes(buf)
    return False, bytes(buf)


hard_reset()
time.sleep(1.0)

ser = serial.Serial(PORT, BAUD, timeout=0.2)
try:
    ser.dtr = False
    ser.rts = False
except OSError:
    pass

print('=== booting ===', flush=True)
ok, buf = wait_for(ser, 'nsh>', 25)
sys.stdout.write(buf.decode('utf-8', errors='replace'))
if not ok:
    print('!! no NSH prompt', file=sys.stderr)
    ser.close()
    sys.exit(1)
print('=== NSH up ===', flush=True)
drain(ser, 1)

print('=== headless ai_agent ===', flush=True)
# NOTE: syslog now goes to the in-RAM log (CONFIG_RAMLOG_SYSLOG), so the agent's
# "All network services started!" line no longer reaches the console. Wait a
# fixed boot window instead; read the agent log later via run_shell dmesg or
# GET /api/logs.
ser.write(b'ai_agent < /dev/null &\r\n')
drain(ser, 10)

print('=== pppd FOREGROUND (NSH blocks) ===', flush=True)
ser.write(b'pppd\r\n')
drain(ser, 6)
ser.close()
print('=== device configured ===', flush=True)
