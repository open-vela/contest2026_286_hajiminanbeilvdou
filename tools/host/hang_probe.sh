#!/bin/bash
# Tell a genuinely hung device apart from a link that is merely blocked.
#
# Both look identical from the PC: the audio stops, ping fails, and the raw
# serial capture stops growing. The difference is what happens when the
# backpressure is removed. Killing the host-side pppd empties the PTY, so a
# device that is alive but stuck behind a full transmit path starts sending
# again immediately; a device that has really hung stays silent.
#
#   ./hang_probe.sh [watch_seconds]

WATCH=${1:-40}
RAW="$HOME/bridge.raw"
LOG="$HOME/voice_test.log"

rate() {   # bytes/s off the wire over a 5 s window
  local a b
  a=$(stat -c %s "$RAW" 2>/dev/null || echo 0)
  sleep 5
  b=$(stat -c %s "$RAW" 2>/dev/null || echo 0)
  echo $(( (b - a) / 5 ))
}

echo "=== bringing the pipeline up ==="
bash "$HOME/voice_ready.sh" > /dev/null 2>&1
echo "bridge said: $(grep -c CAPTURING "$LOG") capturing heartbeat(s)"

echo
echo "=== watching for ${WATCH}s ==="
for i in $(seq $((WATCH / 10))); do
  sleep 10
  hb=$(grep '\[alive\]' "$LOG" | tail -1)
  ping -c 1 -W 2 192.168.223.2 > /dev/null 2>&1 && p="ping OK " || p="ping DEAD"
  printf '  t=%2ds  %s  %s\n' "$((i * 10))" "$p" "${hb:-no heartbeat}"
done

echo
echo "=== raw serial while pppd is alive: $(rate) B/s ==="

echo
echo "=== killing host-side pppd to remove backpressure ==="
sudo -n pkill -f "ppp[d] .*ttyHS"
sleep 2
echo "=== raw serial with pppd gone: $(rate) B/s ==="
echo
echo "  nonzero above  => the device is alive; the stall was host-side"
echo "  still zero     => the device itself has stopped transmitting"

echo
echo "=== bridge process state ==="
bp=$(pgrep -f "bridg[e].py" | head -1)
echo "pid=$bp wchan=$(cat /proc/$bp/wchan 2>/dev/null)"
