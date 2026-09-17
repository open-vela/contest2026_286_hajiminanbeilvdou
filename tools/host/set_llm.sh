#!/bin/bash
# Point the device's agent at an LLM endpoint.
#
#   ./set_llm.sh real        # the real API, details from ~/.llm_env
#   ./set_llm.sh mock        # back to the local scripted mock (offline demo)
#   ./set_llm.sh show        # what the device is using right now
#
# Configuration lives in ~/.llm_env, which is OUTSIDE every repository:
#
#   LLM_HOST=api.example.com
#   LLM_PORT=443
#   LLM_PATH=/v1/chat/completions
#   LLM_MODEL=some-model
#   LLM_API_KEY=sk-...
#
# The key is deliberately not a parameter and never printed. It must not reach
# git, the patch set, the contest repo, or a screenshot of this terminal -- a
# key pasted into a shell command ends up in the command line, in the shell
# history and in any log that captures either.

set -u

DEV_IP=192.168.223.2
PC_IP=192.168.223.1
PORT=28789
ENV_FILE="$HOME/.llm_env"
API="http://$DEV_IP:$PORT/api/config"

if [ ! -r "$ENV_FILE" ]; then
  echo "missing $ENV_FILE (needed for 'real' and 'show')" >&2
fi

# shellcheck disable=SC1090
[ -r "$ENV_FILE" ] && . "$ENV_FILE"

case "${1:-show}" in
  mock)
    HOST="$PC_IP"; PORTNUM=8080; PATHV="/v1/chat/completions"
    MODEL="mock-model"; KEY="dummy-key"
    WHAT="local mock (offline; no key needed)"
    ;;
  real)
    : "${LLM_HOST:?not set in $ENV_FILE}" "${LLM_PORT:?}" \
      "${LLM_PATH:?}" "${LLM_MODEL:?}" "${LLM_API_KEY:?}"
    HOST="$LLM_HOST"; PORTNUM="$LLM_PORT"; PATHV="$LLM_PATH"
    MODEL="$LLM_MODEL"; KEY="$LLM_API_KEY"
    WHAT="$LLM_HOST:$LLM_PORT $LLM_MODEL"
    ;;
  show)
    echo "=== device LLM config ==="
    timeout 15 curl -sS "$API" 2>/dev/null
    echo
    echo "=== configured for real API ==="
    echo "  host=${LLM_HOST:-<unset>} port=${LLM_PORT:-<unset>} model=${LLM_MODEL:-<unset>}"
    exit 0
    ;;
  *)
    echo "usage: $0 [real|mock|show]" >&2
    exit 1
    ;;
esac

echo "pointing device at: $WHAT"

# --fail-with-body so a 4xx from the device is visible rather than silent,
# and -s to keep the key out of the progress meter.
timeout 25 curl -sS -X PUT "$API" \
  -H 'Content-Type: application/json' \
  -d "{\"llm_host\":\"$HOST\",\"llm_port\":\"$PORTNUM\",\"llm_path\":\"$PATHV\",\"model\":\"$MODEL\",\"api_key\":\"$KEY\"}" \
  > /dev/null 2>&1

echo "--- device reports ---"
# The device masks the key (sk-…), so this is safe to show.
timeout 15 curl -sS "$API" 2>/dev/null
echo
