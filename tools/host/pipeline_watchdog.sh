#!/bin/bash
# Keep the demo pipeline alive without anyone watching it.
#
# The device can stop answering for reasons that are not ours to fix -- a
# scheduler fault we caught once, a link that goes quiet -- and when it does,
# everything downstream looks fine: the bridge is running, the log has no
# errors, and the screen just stops changing. The only symptom is that the
# byte counter in the log stops moving.
#
# So that counter is what this watches. Recovery is the same full bring-up
# voice_ready.sh does, because a device that has stopped streaming usually
# needs the reset rather than a reconnect.
#
#   ./pipeline_watchdog.sh [interval_seconds] [strikes_before_recovery]

INTERVAL=${1:-20}
STRIKES=${2:-3}
LOG="$HOME/voice_test.log"
WD_LOG="$HOME/watchdog.log"

say() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "$WD_LOG"; }

last=""
bad=0

say "watching $LOG every ${INTERVAL}s (recover after ${STRIKES} silent checks)"

while true; do
  sleep "$INTERVAL"

  # The bridge prints the running total with each heartbeat, so a value that
  # does not move means no audio has arrived. Missing heartbeat lines
  # entirely reads as the same thing.
  cur=$(grep -o '[0-9]* bytes total' "$LOG" 2>/dev/null | tail -1 | grep -o '^[0-9]*')

  if [ -z "$cur" ]; then
    bad=$((bad + 1))
    say "no heartbeat yet ($bad/$STRIKES)"
  elif [ "$cur" = "$last" ]; then
    bad=$((bad + 1))
    say "stalled at $cur bytes ($bad/$STRIKES)"
  else
    [ "$bad" -ne 0 ] && say "recovered on its own at $cur bytes"
    bad=0
  fi
  last="$cur"

  if [ "$bad" -ge "$STRIKES" ]; then
    say "device has stopped streaming; bringing the pipeline back up"
    bad=0
    last=""
    start=$(wc -l < "$WD_LOG")
    bash "$HOME/voice_ready.sh" >> "$WD_LOG" 2>&1
    # Count only this attempt's failures: the log accumulates across runs, so
    # a running total climbs forever and hides whether recovery worked.
    this_run=$(tail -n "+$((start + 1))" "$WD_LOG" | grep -cE 'FAIL')
    if tail -n "+$((start + 1))" "$WD_LOG" | grep -q 'READY'; then
      say "recovery OK"
    else
      say "recovery FAILED ($this_run failed checks this attempt)"
    fi
  fi
done
