#!/bin/bash
# Watch the voice bridge's delivered byte rate and the PPP link's own counters
# side by side. The delivered byte rate equals the device's capture rate
# exactly while nothing is being dropped, so this is the loss detector:
# MU-law is one byte per sample, so a clean 8 kHz stream shows ~8000 B/s.
#
#   ./rate_watch.sh [intervals] [seconds_each]
LOG="$HOME/voice_test.log"
N=${1:-6}
STEP=${2:-10}

last_bytes=0
last_ppp=0
echo "interval  delivered B/s   ppp0_rx B/s   cum_bytes"
for i in $(seq "$N"); do
  sleep "$STEP"
  bytes=$(grep -o '[0-9]* bytes total' "$LOG" | tail -1 | grep -o '^[0-9]*')
  ppp=$(grep ppp0 /proc/net/dev | awk '{print $2}')
  [ -z "$bytes" ] && bytes=0
  [ -z "$ppp" ] && ppp=0
  if [ "$bytes" = "$last_bytes" ]; then
    echo "  $i        STALLED        $(( (ppp-last_ppp)/STEP ))          $bytes"
  else
    echo "  $i        $(( (bytes-last_bytes)/STEP ))            $(( (ppp-last_ppp)/STEP ))          $bytes"
  fi
  last_bytes=$bytes
  last_ppp=$ppp
done
