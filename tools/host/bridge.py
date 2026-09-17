#!/usr/bin/env python3
"""PTY bridge: /dev/ttyUSB0  <->  ~/ttyHS (virtual tty)

Why: opening the CH340 from Linux asserts RTS, which is wired to the board's
reset pin -> every pppd start would reboot the device.  This bridge opens the
physical port exactly once, then exposes a PTY that carries no modem lines,
so pppd (and our console scripts) can attach/detach freely.

Survives transient serial errors (device resets can make the port throw
EIO); reopens the physical port when that happens.

Control: send SIGUSR1 to pulse RTS (hard-reset the board).
    kill -USR1 $(cat ~/bridge.pid)
"""
import errno
import os
import select
import signal
import sys
import time

import serial

PHYS = os.environ.get("PHYS_PORT", "/dev/ttyUSB0")
BAUD = 1000000
LINK = os.path.expanduser("~/ttyHS")
PIDFILE = os.path.expanduser("~/bridge.pid")
RAWFILE = os.path.expanduser("~/bridge.raw")

do_reset = False
running = True


def on_usr1(_signum, _frame):
    global do_reset
    do_reset = True


def on_term(_signum, _frame):
    global running
    running = False


signal.signal(signal.SIGUSR1, on_usr1)
signal.signal(signal.SIGTERM, on_term)


def open_phys():
    s = serial.Serial(PHYS, BAUD, timeout=0)
    try:
        s.rts = False
        s.dtr = False
    except OSError:
        pass
    return s


def pty_drain(fd, buf):
    """Push as much of 'buf' into the PTY as the tty will take right now.

    os.write() on a non-blocking PTY stops at the tty's input queue and
    returns the count it took; the bytes it did not take are gone unless they
    are kept. The queue holds a few KB while the serial side hands over as
    much as 64 KB at once, so under a continuous stream this fires constantly
    -- and the loss is silent here: PPP sees a corrupted frame and drops it,
    which looks like a flaky link rather than a short write.

    Draining rather than waiting matters just as much. The relay is one
    thread, so time spent waiting for room here is time not spent reading the
    serial port; the device's transmit path then backs up behind whatever it
    is sending, and the link looks dead even though nothing has failed. So
    write only what fits and keep the rest for the next pass.
    """
    if not buf:
        return True

    try:
        n = os.write(fd, buf)
        del buf[:n]
    except BlockingIOError:
        pass
    except OSError as exc:
        if exc.errno != errno.EIO:
            print(f"[bridge] pty write failed: {exc}", flush=True)
        buf.clear()
        return False

    return True


ser = open_phys()
time.sleep(0.2)

master_fd, slave_fd = os.openpty()
# The relay drains each side in a loop, so a read that finds nothing must
# return rather than block -- a blocking read on an empty master wedges the
# whole loop and the link goes silent with no error anywhere.
os.set_blocking(master_fd, False)
slave_name = os.ttyname(slave_fd)
os.chmod(slave_name, 0o666)
if os.path.islink(LINK) or os.path.exists(LINK):
    os.remove(LINK)
os.symlink(slave_name, LINK)

with open(PIDFILE, "w") as f:
    f.write(str(os.getpid()))

# Debug capture of everything the board sends. Off unless asked for: it sits
# in the relay loop, and once the link carries a continuous stream the write
# and flush are no longer free. BRIDGE_RAW=1 turns it on.
raw_fp = None
if os.environ.get("BRIDGE_RAW", "0") == "1":
    try:
        raw_fp = open(RAWFILE, "ab")
    except OSError:
        raw_fp = None

print(f"[bridge] {PHYS} <-> {LINK} ({slave_name}); pid={os.getpid()}; "
      f"raw capture {'on' if raw_fp else 'off'}", flush=True)

to_pty = bytearray()

try:
    while running:
        if do_reset:
            do_reset = False
            print("[bridge] resetting board (RTS pulse)", flush=True)
            try:
                ser.rts = True
                time.sleep(0.1)
                ser.rts = False
            except OSError as exc:
                print(f"[bridge] reset failed: {exc}", flush=True)

        try:
            rlist, _, _ = select.select([master_fd, ser.fileno()], [], [], 0.2)
        except (OSError, ValueError) as exc:
            print(f"[bridge] select failed ({exc}); reopening port", flush=True)
            try:
                ser.close()
            except Exception:
                pass
            time.sleep(1.0)
            ser = open_phys()
            continue

        # Drain each side in a loop rather than moving one 4 KB block per
        # select(). One block per wakeup puts a ceiling on throughput that a
        # continuous stream runs straight into; the loop costs nothing when
        # there is only a trickle to move.
        if master_fd in rlist:
            for _ in range(16):
                try:
                    data = os.read(master_fd, 65536)
                except BlockingIOError:
                    break
                except OSError:
                    data = b""
                if not data:
                    break
                try:
                    ser.write(data)
                except Exception as exc:
                    print(f"[bridge] serial write failed: {exc}", flush=True)
                    break

        if ser.fileno() in rlist:
            for _ in range(16):
                try:
                    data = ser.read(65536)
                except Exception as exc:
                    print(f"[bridge] serial read failed ({exc}); reopening",
                          flush=True)
                    try:
                        ser.close()
                    except Exception:
                        pass
                    time.sleep(1.0)
                    ser = open_phys()
                    break
                if not data:
                    break
                if raw_fp is not None:
                    try:
                        raw_fp.write(data)
                        raw_fp.flush()
                    except OSError:
                        pass
                to_pty.extend(data)

        # Give the PTY a chance to take the queued bytes on every pass, not
        # only when the serial port has something new. Under a continuous
        # stream the queue is rarely empty at the top of the loop.
        pty_drain(master_fd, to_pty)
except KeyboardInterrupt:
    pass
finally:
    try:
        ser.close()
    except Exception:
        pass
    os.close(master_fd)
    os.close(slave_fd)
    print("[bridge] closed", flush=True)
    sys.exit(0)
