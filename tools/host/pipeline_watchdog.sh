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
DEV_IP=192.168.223.2
LOG="$HOME/voice_test.log"
HEALTH_URL="http://$DEV_IP:28789/api/health"
ACK_URL="http://$DEV_IP:28789/api/health/ack"
WD_LOG="$HOME/watchdog.log"

say() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "$WD_LOG"; }

last=""
bad=0

say "watching $HEALTH_URL + $LOG every ${INTERVAL}s (recover after ${STRIKES} failed checks)"

while true; do
  sleep "$INTERVAL"

  # The bridge prints the running total with each heartbeat, so a value that
  # does not move means no audio has arrived. Missing heartbeat lines
  # entirely reads as the same thing.
  #
  # The health body is the second opinion: a device that still answers but
  # says it is critical is given time to recover itself, and one that has
  # already set reboot_pending/handling must not be reset from this side on
  # top of the reset it is about to do.
  cur=$(grep -o '[0-9]* bytes total' "$LOG" 2>/dev/null | tail -1 | grep -o '^[0-9]*')
  body=$(timeout 4 curl -sS "$HEALTH_URL" 2>/dev/null || true)

  [ -n "$body" ] && say "health $body"

  status=$(printf '%s' "$body" | sed -n 's/.*"status":"\([^"]*\)".*/\1/p')
  reboot_pending=$(printf '%s' "$body" \
    | sed -n 's/.*"reboot_pending":\(true\|false\).*/\1/p')
  handling=$(printf '%s' "$body" \
    | sed -n 's/.*"handling":\(true\|false\).*/\1/p')

  if [ -z "$body" ]; then
    bad=$((bad + 1))
    say "health probe failed ($bad/$STRIKES)"
  elif [ "$reboot_pending" = "true" ] || [ "$handling" = "true" ] \
       || [ "$status" = "starting" ]; then
    # The device is recovering (or still in its boot grace window). A host
    # reset here would interrupt exactly the work we asked it to do.
    [ "$bad" -ne 0 ] && say "device recovering; holding ($bad/$STRIKES)"
    bad=0
  elif [ -z "$cur" ]; then
    bad=$((bad + 1))
    say "no heartbeat yet ($bad/$STRIKES)"
  elif [ "$cur" = "$last" ]; then
    bad=$((bad + 1))
    say "stalled at $cur bytes (status=${status:-?}, $bad/$STRIKES)"
  else
    [ "$bad" -ne 0 ] && say "recovered on its own at $cur bytes"
    bad=0
  fi
  last="$cur"

  if [ "$bad" -ge "$STRIKES" ]; then
    say "device has stopped streaming; bringing the pipeline back up"
    bad=0
    last=""

    # The bridge is the single entry point for serial, PPP and audio, and
    # voice_ready.sh does not start it. If it died, the full bring-up below
    # cannot succeed, so recover it first.
    if ! pgrep -f "[b]ridge.py" >/dev/null || [ ! -e "$HOME/ttyHS" ]; then
      say "serial bridge is down; restarting it"
      if [ -x "$HOME/bridge_restart.sh" ]; then
        bash "$HOME/bridge_restart.sh" >> "$WD_LOG" 2>&1 || \
          say "bridge restart failed"
      else
        say "bridge_restart.sh missing; cannot recover the bridge automatically"
      fi
    fi

    start=$(wc -l < "$WD_LOG")
    bash "$HOME/voice_ready.sh" >> "$WD_LOG" 2>&1
    # Count only this attempt's failures: the log accumulates across runs, so
    # a running total climbs forever and hides whether recovery worked.
    this_run=$(tail -n "+$((start + 1))" "$WD_LOG" | grep -cE 'FAIL')
    if tail -n "+$((start + 1))" "$WD_LOG" | grep -q 'READY'; then
      say "recovery OK"
      # Tell the device the host has it under control again: this clears
      # safe_mode and the auto-reset budget, so the next fault can still be
      # treated as fresh.
      if timeout 4 curl -sS -X POST "$ACK_URL" > /dev/null 2>&1; then
        say "health ack sent"
      else
        say "health ack failed (device will stay in safe mode)"
      fi
    else
      say "recovery FAILED ($this_run failed checks this attempt)"
    fi
  fi
done
