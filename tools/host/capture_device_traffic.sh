#!/bin/bash
# See what the device actually puts on the wire when it tries to reach the
# LLM.
#
# It never opens a connection to port 443, so the failure is upstream of
# that: name resolution, or a missing route. Capturing the ppp0 interface
# during an attempt distinguishes them -- a DNS query going out and nothing
# coming back is a resolution problem, while no packets at all means the
# device is not even trying to leave the link.
#
#   ./capture_device_traffic.sh

echo "=== capture tools ==="
for t in tcpdump conntrack; do
  if command -v "$t" > /dev/null 2>&1; then echo "  $t: yes"; else echo "  $t: no"; fi
done

echo
echo "=== ppp0 counters before ==="
awk '/ppp0/ {print "  rx="$2" tx="$10}' /proc/net/dev

echo
echo "=== pointing the device at the real API and sending a command ==="
bash "$HOME/set_llm.sh" real > /dev/null 2>&1
( cd "$HOME" && timeout 70 python3 -u ws_chat.py "请巡检设备" > /dev/null 2>&1 ) &

if command -v tcpdump > /dev/null 2>&1; then
  echo "=== capturing 60s on ppp0 ==="
  sudo -n timeout 60 tcpdump -i ppp0 -nn -q 2>/dev/null \
    | awk '{print $3, $5, $6, $7, $8}' | sort | uniq -c | sort -rn | head -15
else
  echo "no tcpdump; falling back to counters"
  sleep 60
fi

echo
echo "=== ppp0 counters after ==="
awk '/ppp0/ {print "  rx="$2" tx="$10}' /proc/net/dev
