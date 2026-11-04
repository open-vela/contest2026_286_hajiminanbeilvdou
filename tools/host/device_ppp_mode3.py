#!/usr/bin/env python3
"""Reset the board; the firmware rcS autostarts ai_agent + pppd.

Since 2026-10-31 the boot script /etc/init.d/rcS starts the headless agent
and runs pppd in the foreground on /dev/console (the CH340 console), so NSH
waits in the script and never competes with PPP for the UART.

This host script therefore only pulses RTS through the bridge.  Typing
commands into the console here would now land inside the PPP byte stream.

    python3 ~/device_ppp_mode3.py
"""
import os
import signal
import time

PIDFILE = os.path.expanduser("~/bridge.pid")


def hard_reset():
    with open(PIDFILE) as f:
        pid = int(f.read().strip())
    os.kill(pid, signal.SIGUSR1)


hard_reset()
time.sleep(3)
print("=== board reset; rcS autostarts ai_agent + pppd ===")
