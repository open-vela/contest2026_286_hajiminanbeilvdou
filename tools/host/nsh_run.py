#!/usr/bin/env python3
"""Reset the board, wait for NSH, run one command, print what comes back.

Used to exercise the device with no PPP in the way, so that console output is
visible and a hang can be told apart from a link failure.

The agent is started first, headless, because the mic bring-up commands
(mic_test/mic_rate/mic_dump) are registered by ai_agent rather than built
into NSH -- without it they are simply "command not found".

    python3 ~/nsh_run.py "mic_test 20 pll 0 30 9 1" [seconds_to_watch]
    python3 ~/nsh_run.py --no-agent "free"
"""
import os
import signal
import sys
import time

import serial

PORT = os.environ.get('DEV_PORT', os.path.expanduser('~/ttyHS'))
BAUD = 1000000
PIDFILE = os.path.expanduser('~/bridge.pid')
ARGV = [a for a in sys.argv[1:] if a != '--no-agent']
WITH_AGENT = '--no-agent' not in sys.argv
CMD = ARGV[0] if ARGV else 'help'
WATCH = float(ARGV[1]) if len(ARGV) > 1 else 30.0


def hard_reset():
    with open(PIDFILE) as f:
        pid = int(f.read().strip())
    os.kill(pid, signal.SIGUSR1)


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

ok, buf = wait_for(ser, 'nsh>', 25)
sys.stdout.write(buf.decode('utf-8', errors='replace'))
if not ok:
    print('!! no NSH prompt', file=sys.stderr)
    sys.exit(1)

if WITH_AGENT:
    # The mic commands are dispatched by the agent's own CLI thread, which
    # reads stdin -- running the agent with stdin redirected to /dev/null
    # makes that thread exit on EOF and the commands unreachable. So it has
    # to be started interactively and driven at its "vela>" prompt.
    # Foreground, not '&': with a background job NSH returns to its own prompt
    # and both shells then read the console, so the command lands on whichever
    # wins the race (in practice, NSH's "command not found"). In the
    # foreground NSH blocks in waitpid and stops reading, leaving the console
    # to the agent.
    print('=== starting interactive agent, waiting for vela> ===', flush=True)
    ser.write(b'ai_agent\r\n')
    ok, buf = wait_for(ser, 'vela>', 40)
    sys.stdout.write(buf.decode('utf-8', errors='replace'))
    if not ok:
        print('!! no vela> prompt', file=sys.stderr)
        sys.exit(1)
    time.sleep(1)

print(f'=== NSH up; running: {CMD} ===', flush=True)
ser.write(CMD.encode() + b'\r\n')

# Stream whatever the console says until the watch window closes. A device
# that has hung stops emitting entirely, which is the thing we are looking
# for, so keep reading right up to the end rather than stopping at the first
# quiet moment.
end = time.time() + WATCH
silent_since = None
while time.time() < end:
    data = ser.read(4096)
    if data:
        silent_since = None
        sys.stdout.write(data.decode('utf-8', errors='replace'))
        sys.stdout.flush()
    elif silent_since is None:
        silent_since = time.time()

print(f'\n=== done (silent for the last '
      f'{time.time() - (silent_since or end):.1f}s) ===', flush=True)
ser.close()
