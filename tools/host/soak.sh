#!/bin/bash
# Watch the microphone uplink for a while and report how it held up.
#
# The failure this is looking for does not announce itself: the stream simply
# stops, the device stops answering ping, and nothing anywhere reports an
# error. So sample often and keep the last value, rather than checking once at
# the end and seeing nothing wrong.
#
#   ./soak.sh [minutes]

MINUTES=${1:-4}
LOG="$HOME/voice_test.log"

echo "=== single pppd? ==="
pgrep -af "ppp[d] " | grep -v pgrep
echo "count: $(pgrep -cf 'ppp[d] .*ttyHS')"

echo
echo "  time   delivered   ppp0_rx   link"
prev=0
prevp=0
stalls=0
for i in $(seq $((MINUTES * 4))); do
  sleep 15
  cur=$(grep -o '[0-9]* bytes total' "$LOG" | tail -1 | grep -o '^[0-9]*')
  ppp=$(grep ppp0 /proc/net/dev | awk '{print $2}')
  [ -z "$cur" ] && cur=0
  [ -z "$ppp" ] && ppp=0

  if ping -c 1 -W 2 192.168.223.2 > /dev/null 2>&1; then
    link="OK  "
  else
    link="DEAD"
  fi

  d=$(( (cur - prev) / 15 ))
  if [ "$d" -eq 0 ]; then
    stalls=$((stalls + 1))
    rate="  STALL "
  else
    rate=$(printf '%6d' "$d")
  fi

  printf '  %4ds  %s B/s  %7d   %s\n' "$((i * 15))" "$rate" \
    "$(( (ppp - prevp) / 15 ))" "$link"

  prev=$cur
  prevp=$ppp
done

echo
echo "stalled samples: $stalls  (0 across a soak means the uplink held)"
