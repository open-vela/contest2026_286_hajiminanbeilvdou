#!/bin/bash
# Print the most recent crash from the raw serial capture, with the registers,
# the faulting task's backtrace and the addresses that were being touched.
#
#   ./show_crash.sh [lines_around]

AROUND=${1:-70}
RAW="$HOME/bridge.raw"
TXT=/tmp/raw_text.txt

strings -n 5 "$RAW" > "$TXT" 2>/dev/null

n=$(grep -n 'dump_assert_info' "$TXT" | tail -1 | cut -d: -f1)

if [ -z "$n" ]; then
  echo "no crash in $RAW"
  exit 0
fi

echo "=== crash at text line $n of $(wc -l < "$TXT") ==="
sed -n "$((n)),$((n + AROUND))p" "$TXT"
