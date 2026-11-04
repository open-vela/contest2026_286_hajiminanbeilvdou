#!/bin/bash
# Inject a health fault on the device, for HIL verification only.
#
#   ./health_fault.sh health                 # print the current snapshot
#   ./health_fault.sh agent_stall 30         # 30 s stall, then self-recover
#   ./health_fault.sh agent_stall 0          # permanent stall -> soft reset
#   ./health_fault.sh mic_read_fail 3        # make the next 3 reads fail
#   ./health_fault.sh ws_send_fail 20        # fail the next 20 sends
#   ./health_fault.sh heap_hog 7000          # hold 7000 KB (until fault_off);
#                                            #   a sudden drop also trips heap_trend
#   ./health_fault.sh net_down 60            # pretend the link is down
#   ./health_fault.sh fault_off              # clear everything
#
# The device must be built with CONFIG_AI_AGENT_HEALTH_TEST.
# A busy device may answer the POST late (heap_hog memsets ~7 MB first); the
# fault is applied even if curl gives up, so confirm through the snapshot.

set -u

DEV_IP=${DEV_IP:-192.168.223.2}
PORT=${PORT:-28789}
BASE="http://$DEV_IP:$PORT/api/health"

usage() {
  sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
}

case "${1:-}" in
  health)
    curl -sS "$BASE" | python3 -m json.tool
    exit 0
    ;;
  agent_stall)
    body="{\"fault\":\"agent_stall\",\"seconds\":${2:-30}}"
    ;;
  mic_read_fail)
    body="{\"fault\":\"mic_read_fail\",\"count\":${2:-3}}"
    ;;
  ws_send_fail)
    body="{\"fault\":\"ws_send_fail\",\"count\":${2:-20}}"
    ;;
  heap_hog)
    body="{\"fault\":\"heap_hog\",\"kb\":${2:-7000}}"
    ;;
  net_down)
    body="{\"fault\":\"net_down\",\"seconds\":${2:-60}}"
    ;;
  fault_off)
    body='{"fault":"fault_off"}'
    ;;
  *)
    usage
    exit 2
    ;;
esac

echo "POST $BASE/fault $body"
if curl -sS --max-time 30 -X POST "$BASE/fault" \
     -H 'Content-Type: application/json' \
     -d "$body"; then
  :
else
  echo
  echo "(no response within 30 s; check GET /api/health before retrying)"
fi
echo
