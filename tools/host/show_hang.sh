#!/bin/bash
# Print the task dump forced by the mic-stream stall watchdog, focusing on
# what each task was actually doing when the audio stopped making progress.
#
#   ./show_hang.sh

TXT=/tmp/hang_text.txt
strings -n 5 "$HOME/bridge.raw" > "$TXT" 2>/dev/null

n=$(grep -n 'STALL WATCHDOG' "$TXT" | tail -1 | cut -d: -f1)
if [ -z "$n" ]; then
  echo "no stall watchdog event in the capture"
  exit 0
fi

echo "=== watchdog event at line $n ==="
sed -n "${n},$((n + 3))p" "$TXT"

echo
echo "=== task states ==="
sed -n "${n},$((n + 200))p" "$TXT" | grep '^dump_task:' | sed 's/.*COMMAND/COMMAND/'

echo
echo "=== stacks of the interesting tasks ==="
# ai_agent loop (19), pppd (22), mic stream (31) -- the three that decide
# whether audio flows.
for tid in 19 22 31; do
  echo "--- task $tid ---"
  sed -n "${n},$((n + 400))p" "$TXT" \
    | grep -A14 "^sched_dumpstack: \[$tid\]" | head -15
done
