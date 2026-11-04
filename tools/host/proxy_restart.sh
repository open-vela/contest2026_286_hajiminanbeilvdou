#!/bin/bash
# Start the real-API proxy in place of the scripted mock.
#
# Both want port 8080, because the device is configured to talk to the host
# on that port and its config does not need to know which one is behind it.
# So starting one stops the other.
#
#   ./proxy_restart.sh              # real API
#   ./proxy_restart.sh --mock       # back to the scripted mock
#
# Patterns live in this file so they never appear on a caller's command line.

MOCK_PORT=8081
PROXY_PORT=8080

if [ "${1:-}" = "--mock" ]; then
  pkill -f 'mock_ll[m].py' 2>/dev/null
  pkill -f 'llm_prox[y].py' 2>/dev/null
  sleep 1
  MOCK_DEBUG=1 setsid nohup python3 -u "$HOME/mock_llm.py" "$MOCK_PORT" \
    > "$HOME/mock_llm.log" 2>&1 < /dev/null &
  sleep 2
  echo "mock procs: $(pgrep -cf 'mock_ll[m].py') on :$MOCK_PORT"
  exit 0
fi

# Run both halves at once: the device settings page switches between them by
# changing only llm_port (8081 local, 8080 cloud), so neither may take the
# other's port.
pkill -f 'mock_ll[m].py' 2>/dev/null
pkill -f 'llm_prox[y].py' 2>/dev/null
sleep 1
MOCK_DEBUG=1 setsid nohup python3 -u "$HOME/mock_llm.py" "$MOCK_PORT" \
  > "$HOME/mock_llm.log" 2>&1 < /dev/null &
sleep 1
setsid nohup python3 -u "$HOME/llm_proxy.py" "$PROXY_PORT" --fallback-mock \
  > "$HOME/llm_proxy.log" 2>&1 < /dev/null &
sleep 2
echo "mock procs:  $(pgrep -cf 'mock_ll[m].py') on :$MOCK_PORT"
echo "proxy procs: $(pgrep -cf 'llm_prox[y].py') on :$PROXY_PORT"
head -2 "$HOME/llm_proxy.log"
