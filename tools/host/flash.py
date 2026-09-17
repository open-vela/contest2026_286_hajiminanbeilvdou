#!/usr/bin/env python3
"""Flash nuttx.bin to the board, coordinating with the PTY bridge.

The bridge holds /dev/ttyUSB0 exclusively, so it must be stopped for the
flash and restarted afterwards.

    python3 ~/flash.py [path/to/nuttx.bin]
"""
import os
import signal
import subprocess
import sys
import time

import serial

IMG = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/nuttx.bin")
SFTOOL = os.path.expanduser("~/sftool/sftool")
PHYS = "/dev/ttyUSB0"
BAUD = 1000000


def stop_bridge():
    r = subprocess.run(["pgrep", "-f", "[b]ridge.py"], capture_output=True, text=True)
    for pid in r.stdout.split():
        os.kill(int(pid), signal.SIGTERM)
    if r.stdout.strip():
        time.sleep(1)


def start_bridge():
    subprocess.Popen(
        ["python3", os.path.expanduser("~/bridge.py")],
        stdout=open(os.path.expanduser("~/bridge.log"), "a"),
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    time.sleep(1.5)


stop_bridge()

ser = serial.Serial(PHYS, BAUD, timeout=0.5)
ser.rts = True
time.sleep(0.1)
ser.rts = False
ser.close()
time.sleep(0.3)

cmd = [SFTOOL, "-c", "SF32LB52", "-p", PHYS, "-b", str(BAUD),
       "--before", "no_reset", "--after", "soft_reset",
       "write_flash", f"{IMG}@0x12010000"]
result = subprocess.run(cmd, capture_output=True, text=True)
print("flash exit:", result.returncode)

start_bridge()
print("bridge restarted")
sys.exit(result.returncode)
