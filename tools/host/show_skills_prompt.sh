#!/bin/bash
# Show the skills list exactly as the model receives it.
#
# The prompt reaches us in the mock's raw request dump with newlines escaped
# as \n, so unescape before reading it -- otherwise it is one unreadable line
# and it is impossible to tell an empty description from a missing skill.
#
#   ./show_skills_prompt.sh

RAW="$HOME/mock_raw.jsonl"

if [ ! -r "$RAW" ]; then
  echo "no raw dump; start the mock with ~/mock_restart_dbg.sh first"
  exit 1
fi

# Last request only, then unescape the JSON string escapes.
tail -1 "$RAW" | sed 's/\\n/\n/g' | sed 's/\\"/"/g' | \
  awk '/^## Skills/{hit=1} hit && /^- \*\*/{print} hit && /^$/{if(seen)exit; seen=1}' | \
  head -20
