#!/bin/bash
# One-shot throughput report for the microphone uplink:
#
#   delivered  - audio payload the voice bridge actually received (B/s)
#   ppp0       - bytes the PPP interface carried (payload + all framing)
#   raw        - bytes that came off /dev/ttyUSB0 (requires BRIDGE_RAW=1)
#
# MU-law is one byte per sample, so at 8 kHz a lossless stream delivers
# 8000 B/s. Anything less means the reader was lapped by the DMA ring.
#
#   ./measure.sh [seconds]
SECS=${1:-15}

b0=$(grep -o '[0-9]* bytes total' "$HOME/voice_test.log" | tail -1 | grep -o '^[0-9]*')
p0=$(grep ppp0 /proc/net/dev | awk '{print $2}')
r0=$(stat -c %s "$HOME/bridge.raw" 2>/dev/null || echo 0)

echo "sampling for ${SECS}s ..."
sleep "$SECS"

b1=$(grep -o '[0-9]* bytes total' "$HOME/voice_test.log" | tail -1 | grep -o '^[0-9]*')
p1=$(grep ppp0 /proc/net/dev | awk '{print $2}')
r1=$(stat -c %s "$HOME/bridge.raw" 2>/dev/null || echo 0)

echo "delivered : $(( (b1-b0)/SECS )) B/s   (target 8000 = lossless at 8 kHz)"
echo "ppp0      : $(( (p1-p0)/SECS )) B/s"
echo "raw serial: $(( (r1-r0)/SECS )) B/s"

echo
echo "--- TCP ---"
ss -tin state established "( sport = :28789 or dport = :28789 )" 2>/dev/null \
  | tail -1 | tr ' ' '\n' \
  | grep -E "^(mss|advmss|rcvmss|data_segs_in|bytes_received|rcv_ooopack|retrans|bytes_retrans):"

echo
echo "--- last heartbeats ---"
grep '\[alive\]' "$HOME/voice_test.log" | tail -3
