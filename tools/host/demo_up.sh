#!/bin/bash
# Bring up the full device<->PC link for the Huangshan Pi demo.
#
#   Device side (typed once, before this script):
#       ai_agent                       # set_llm <url> <model> <key>, then 'quit'
#       pppd &
#       ai_agent < /dev/null &         # headless: CLI thread exits on EOF
#       sleep 3600                     # NSH stops reading the console
#
#   Then run this script on the PC (WSL).
#
# Ceiling: mock LLM on 8080, pppd server on /dev/ttyUSB0 (192.168.223.1:...:2)
set -e

# pppd talks to the PTY bridge, NOT the CH340 directly: opening the physical
# port asserts RTS, which is wired to the board's reset pin.
DEV=${DEV_PORT:-$HOME/ttyHS}
BAUD=1000000
MOCK=${MOCK:-1}

if [ ! -e "$DEV" ]; then
  echo "ERROR: $DEV missing. Start the bridge first:  nohup python3 ~/bridge.py &"
  exit 1
fi

# --- NAT for the device's outbound traffic -------------------------------
sudo sysctl -w net.ipv4.ip_forward=1 > /dev/null
sudo iptables -t nat -C POSTROUTING -s 192.168.223.0/24 -o eth0 -j MASQUERADE 2>/dev/null || \
  sudo iptables -t nat -A POSTROUTING -s 192.168.223.0/24 -o eth0 -j MASQUERADE
echo "[demo] NAT ready (192.168.223.0/24 -> eth0)"

# --- optional mock LLM so no API key is needed ---------------------------
if [ "$MOCK" = "1" ]; then
  pkill -f mock_llm.py 2>/dev/null || true
  # -u: without it the redirect makes stdout block-buffered, so the mock
  # shows nothing until it writes 4 KB -- which is forever at one line per
  # request, and makes "the mock was never called" and "the mock was called
  # and chose badly" look identical.
  nohup python3 -u "$HOME/mock_llm.py" 8080 > "$HOME/mock_llm.log" 2>&1 &
  echo "[demo] mock LLM on 0.0.0.0:8080 (log: ~/mock_llm.log)"
fi

# --- PPP server -----------------------------------------------------------
# noauth requires root; sudoers grants NOPASSWD for /usr/sbin/pppd.
# sudo: pppd runs as root, so this has to match privilege or the previous
# instance survives and both then read the same tty. See voice_ready.sh.
sudo -n pkill -f "pppd $DEV" 2>/dev/null || true

# Then wait for it to actually be gone. pppd takes a lock on the tty while it
# sets up and the next one refuses to start while that lock is held -- "Device
# ttyHS is locked by pid N" -- so starting the moment after signalling turns
# every restart into a race, which is exactly what a recovery script needs to
# not lose. Escalate if it will not leave.
for _ in $(seq 20); do
  pgrep -f "pppd $DEV" > /dev/null || break
  sleep 0.5
done
if pgrep -f "pppd $DEV" > /dev/null; then
  echo "[demo] previous pppd still alive after 10s; killing it"
  sudo -n pkill -9 -f "pppd $DEV" 2>/dev/null || true
  sleep 1
fi
echo "[demo] starting pppd server on $DEV ..."
exec sudo -n pppd "$DEV" "$BAUD" 192.168.223.1:192.168.223.2 noauth nodetach debug
