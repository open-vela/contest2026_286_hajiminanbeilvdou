#!/bin/bash
# Does the device's traffic ever leave the host for the internet?
#
# The device sits behind this host's NAT, so the MASQUERADE rule's packet
# counter counts exactly the traffic it sends beyond the link. That is a
# cleaner answer than the ppp0 byte counters, which the audio stream
# (several KB/s) swamps completely.
#
#   ./nat_counter_test.sh

MASK='192.168.223.0/24'

count() {
  sudo -n iptables -t nat -L POSTROUTING -v -n 2>/dev/null \
    | awk -v m="$MASK" '$0 ~ m {print $1; exit}'
}

echo "=== recover the device first ==="
sudo -n pkill -f 'ppp[d] .*ttyHS'
pkill -f 'voice_bridg[e]'
sleep 1
BRIDGE_RAW=1 bash "$HOME/bridge_restart.sh" > /dev/null 2>&1
bash "$HOME/voice_ready.sh" > /dev/null 2>&1
echo "  pipeline: $(grep -c '\[alive\]' "$HOME/voice_test.log") heartbeat(s)"

echo
echo "=== point at the real API ==="
bash "$HOME/set_llm.sh" real > /dev/null 2>&1

before=$(count)
echo "  NAT packets before: ${before:-<rule missing>}"

echo
echo "=== send a command and wait 90s ==="
( cd "$HOME" && timeout 90 python3 -u ws_chat.py "请巡检设备" > /dev/null 2>&1 ) &
sleep 90

after=$(count)
echo "  NAT packets after:  ${after:-<rule missing>}"

echo
if [ -n "$before" ] && [ -n "$after" ]; then
  echo "  delta: $((after - before)) packets"
  if [ "$after" -gt "$before" ]; then
    echo "=> the device IS sending traffic out to the internet"
  else
    echo "=> the device sent NOTHING beyond the link: it never got off the ppp0 route"
  fi
fi

echo
echo "=== device state afterwards ==="
ping -c 1 -W 3 192.168.223.2 > /dev/null 2>&1 && echo "  ping OK" || echo "  ping FAIL (wedged)"
