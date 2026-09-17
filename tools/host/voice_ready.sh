#!/bin/bash
# Bring the voice pipeline up and verify it is actually streaming before
# returning. Run this, wait for READY, then talk at the board.
#
#   ./voice_ready.sh          # bring up + verify, leave the bridge running
#   ./voice_ready.sh --stop   # stop the bridge
#
# The bridge is the receiver: it must be running before anyone speaks, and it
# has to be re-verified each time because the PPP link does not survive the
# board being reset or the WSL session idling out.

set -u

DEV_IP=192.168.223.2
PC_IP=192.168.223.1
PORT=28789
BRIDGE_LOG="$HOME/voice_test.log"
BRIDGE_PID_FILE="$HOME/voice_bridge.pid"

if [ "${1:-}" = "--stop" ]; then
  pkill -f voice_bridge.py 2>/dev/null && echo "voice bridge stopped" \
    || echo "voice bridge was not running"
  exit 0
fi

step() { printf '\n=== %s ===\n' "$1"; }
ok()   { printf '  OK   %s\n' "$1"; }
bad()  { printf '  FAIL %s\n' "$1"; }

# --- 1. board up, headless agent, pppd on the device ------------------------
step "1/5 device: reset -> headless agent -> pppd"
pkill -f voice_bridge.py 2>/dev/null
# sudo: pppd runs as root (noauth needs it), so an unprivileged pkill leaves
# it alive. The leftovers then pile up, and because Linux reuses pty numbers
# a stale pppd can end up on the *new* bridge's tty, splitting the PPP byte
# stream with the current one -- which shows up as a link that works and then
# mysteriously stops, not as anything obviously wrong with the setup.
sudo -n pkill -f "pppd $HOME/ttyHS" 2>/dev/null
sleep 1
python3 -u "$HOME/device_ppp_mode3.py" 2>&1 | tail -21 | tail -1
ok "device configured"

# --- 2. PC side: NAT + mock LLM + pppd server -------------------------------
step "2/5 PC: NAT + mock LLM + pppd server"
setsid nohup bash "$HOME/demo_up.sh" > "$HOME/demo_up.log" 2>&1 &

for _ in $(seq 40); do
  ip -4 addr show ppp0 2>/dev/null | grep -q "$PC_IP" && break
  sleep 1
done
if ! ip -4 addr show ppp0 2>/dev/null | grep -q "$PC_IP"; then
  bad "ppp0 did not come up"; tail -5 "$HOME/demo_up.log"; exit 1
fi
ok "ppp0 $PC_IP <-> $DEV_IP"

# --- 3. reachability --------------------------------------------------------
step "3/5 link check"
if ping -c 2 -W 3 "$DEV_IP" > /dev/null 2>&1; then
  ok "device answers ping"
else
  bad "device does not answer ping"; exit 1
fi

if timeout 6 bash -c "cat < /dev/null > /dev/tcp/$DEV_IP/$PORT" 2>/dev/null; then
  ok "agent websocket port $PORT open"
else
  bad "port $PORT unreachable"; exit 1
fi

# --- 4. LLM config (device /data is tmpfs; lost on every reboot) ------------
step "4/5 provision LLM config"
curl -sS -X PUT "http://$DEV_IP:$PORT/api/config" \
  -H 'Content-Type: application/json' \
  -d "{\"llm_host\":\"$PC_IP\",\"llm_port\":\"8080\",\"llm_path\":\"/v1/chat/completions\",\"model\":\"mock-model\",\"api_key\":\"dummy-key\"}" \
  > /dev/null 2>&1
MODEL=$(timeout 15 curl -sS "http://$DEV_IP:$PORT/api/config" 2>/dev/null \
        | sed -n 's/.*"model":"\([^"]*\)".*/\1/p')
if [ -n "$MODEL" ]; then
  ok "model=$MODEL"
else
  bad "config did not take"; exit 1
fi

# --- 5. bridge, then prove audio is arriving --------------------------------
step "5/5 voice bridge + stream verification"
rm -f "$BRIDGE_LOG"
setsid nohup "$HOME/voiceenv/bin/python" -u "$HOME/voice_bridge.py" \
  --seconds 0 --save-wav "$HOME/mic_captures" \
  > "$BRIDGE_LOG" 2>&1 &
BRIDGE_PID=$!
echo "$BRIDGE_PID" > "$BRIDGE_PID_FILE"

sleep 8

if ! kill -0 "$BRIDGE_PID" 2>/dev/null; then
  bad "bridge exited during startup"; tail -15 "$BRIDGE_LOG"; exit 1
fi

if grep -q Traceback "$BRIDGE_LOG"; then
  bad "bridge raised"; tail -15 "$BRIDGE_LOG"; exit 1
fi

RATE=$(sed -n 's/.*stream format: \([a-z]*\) \([0-9]*\) Hz.*/\1 \2 Hz/p' \
       "$BRIDGE_LOG" | head -1)
[ -n "$RATE" ] && ok "stream format: $RATE" || bad "no audio_format frame"

# Decisive check: the bridge prints a heartbeat every 10 s once it is really
# pulling audio, so waiting for one proves the whole path is live rather than
# just that the process started.
for _ in $(seq 14); do
  grep -q '\[alive\]' "$BRIDGE_LOG" && break
  kill -0 "$BRIDGE_PID" 2>/dev/null || break
  sleep 1
done

HB=$(grep '\[alive\]' "$BRIDGE_LOG" | tail -1)
if [ -z "$HB" ]; then
  bad "no heartbeat -- audio is not arriving"
  tail -15 "$BRIDGE_LOG"
  exit 1
fi

case "$HB" in
  *CAPTURING*)
    ok "heartbeat:$(printf '%s' "$HB" | sed 's/.*\[alive\]//')"
    ;;
  *)
    bad "bridge is up but the device is not sending audio"
    printf '%s\n' "$HB"
    exit 1
    ;;
esac

cat <<'EOF'

################################################################
  READY -- 现在对着板子上的麦克风孔正常说一句中文即可,例如:
      "现在几点了"
      "请每两分钟巡检一次设备"
      "看一下设备的内存"

  说完等 2~3 秒,然后另开一个终端看结果:
      grep -E '\[asr\]|\[cmd\]|\[dev' ~/voice_test.log | tail -20

  看实时日志:
      tail -f ~/voice_test.log

  停止:
      ~/voice_ready.sh --stop
################################################################
EOF
