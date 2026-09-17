#!/bin/bash
# Bring the PPP link up but never start the microphone stream, then watch.
#
# The point is to separate "this firmware configuration cannot hold a PPP link"
# from "the audio stream is what kills it". Nothing else differs from
# voice_ready.sh except that no audio flows.
#
#   ./idle_control.sh [minutes]

MINUTES=${1:-3}

sudo -n pkill -f "pppd .*ttyHS"
pkill -f 'voice_bridg[e]'
sleep 1

python3 -u "$HOME/device_ppp_mode3.py" > "$HOME/idle_device.log" 2>&1
setsid nohup bash "$HOME/demo_up.sh" > "$HOME/idle_demo.log" 2>&1 &

for _ in $(seq 40); do
  ip -4 addr show ppp0 2>/dev/null | grep -q 192.168.223.1 && break
  sleep 1
done
ip -4 addr show ppp0 2>/dev/null | grep -q 192.168.223.1 \
  || { echo "ppp0 did not come up"; exit 1; }

echo "PPP up, no audio. Watching for ${MINUTES} min."
for i in $(seq $((MINUTES * 4 * 4))); do
  sleep 15
  if ping -c 1 -W 2 192.168.223.2 > /dev/null 2>&1; then
    echo "  t=$((i * 15))s  ping OK"
  else
    echo "  t=$((i * 15))s  ping DEAD"
    break
  fi
done

echo "--- host pppd verdict ---"
tail -3 "$HOME/demo_up.log"
