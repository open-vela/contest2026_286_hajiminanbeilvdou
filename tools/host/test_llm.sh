#!/bin/bash
# Check the configured endpoint works, and that it can call tools.
#
#   ./test_llm.sh
#
# Run this before pointing the device at it. If this fails the problem is the
# key, the host or the model name, and no amount of device-side debugging will
# find it; if it passes, everything left is on the device side.
#
# The tool call matters as much as the reply: the agent drives the device by
# asking the model to call run_shell and friends, so a model that cannot call
# tools answers questions but never actually does anything.

set -u
ENV_FILE="$HOME/.llm_env"

[ -r "$ENV_FILE" ] || { echo "missing $ENV_FILE -- run ./setup_llm.sh" >&2; exit 1; }
# shellcheck disable=SC1090
. "$ENV_FILE"

: "${LLM_HOST:?}" "${LLM_PORT:?}" "${LLM_PATH:?}" "${LLM_MODEL:?}" "${LLM_API_KEY:?}"

URL="https://$LLM_HOST:$LLM_PORT$LLM_PATH"
echo "POST $URL   model=$LLM_MODEL"
echo

BODY=$(cat <<JSON
{
  "model": "$LLM_MODEL",
  "messages": [{"role": "user", "content": "Read the free memory on a Linux box."}],
  "tools": [{
    "type": "function",
    "function": {
      "name": "run_shell",
      "description": "Run a shell command on the device",
      "parameters": {
        "type": "object",
        "properties": {"command": {"type": "string"}},
        "required": ["command"]
      }
    }
  }],
  "tool_choice": "auto"
}
JSON
)

RESP=$(timeout 60 curl -sS -w '\n__HTTP__%{http_code}' \
  -X POST "$URL" \
  -H "Authorization: Bearer $LLM_API_KEY" \
  -H 'Content-Type: application/json' \
  -d "$BODY" 2>&1)

CODE=$(printf '%s' "$RESP" | sed -n 's/.*__HTTP__//p')
PAYLOAD=$(printf '%s' "$RESP" | sed 's/__HTTP__[0-9]*$//')

echo "HTTP $CODE"

case "$CODE" in
  200) ;;
  401|403) echo "-> the key was rejected"; exit 1 ;;
  404) echo "-> wrong path; check LLM_PATH against the provider's docs"; exit 1 ;;
  000|"") echo "-> could not connect at all (host/port/network)"; echo "$PAYLOAD" | head -3; exit 1 ;;
  *)   echo "$PAYLOAD" | head -5; exit 1 ;;
esac

if printf '%s' "$PAYLOAD" | grep -q '"tool_calls"'; then
  echo "-> OK: the model returned a tool call, so function calling works"
  printf '%s' "$PAYLOAD" | grep -o '"name":"[a-z_]*"' | head -2
else
  echo "-> WARNING: answered without calling the tool."
  echo "   The agent drives the device through tool calls, so a model that"
  echo "   will not call them can chat but cannot run anything. Try another"
  echo "   model id."
  printf '%s' "$PAYLOAD" | head -c 400
  echo
  exit 2
fi
