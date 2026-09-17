#!/bin/bash
# Check one model actually calls tools, and how fast it answers.
#
# A model that only chats is useless here: every action the agent takes goes
# through a tool call, so "no tool_calls in the reply" means "cannot run this
# device" no matter how good the prose is.
#
#   ./test_model.sh deepseek-flash

set -u
ENV_FILE="$HOME/.llm_env"
[ -r "$ENV_FILE" ] || { echo "missing $ENV_FILE"; exit 1; }
# shellcheck disable=SC1090
. "$ENV_FILE"

MODEL="${1:-$LLM_MODEL}"
URL="https://$LLM_HOST:$LLM_PORT$LLM_PATH"

BODY=$(cat <<JSON
{
  "model": "$MODEL",
  "messages": [{"role": "user", "content": "Read the free memory on this Linux device."}],
  "tools": [{
    "type": "function",
    "function": {
      "name": "run_shell",
      "description": "Run a shell command on the device",
      "parameters": {"type": "object",
                     "properties": {"command": {"type": "string"}},
                     "required": ["command"]}
    }
  }],
  "tool_choice": "auto"
}
JSON
)

echo "model=$MODEL"
RESP=$(timeout 60 curl -sS -w '\n__HTTP__%{http_code}__TIME__%{time_total}' \
  -X POST "$URL" -H "Authorization: Bearer $LLM_API_KEY" \
  -H 'Content-Type: application/json' -d "$BODY" 2>&1)

CODE=$(printf '%s' "$RESP" | sed -n 's/.*__HTTP__\([0-9]*\)__TIME__.*/\1/p')
TIME=$(printf '%s' "$RESP" | sed -n 's/.*__TIME__//p' | head -1)
PAYLOAD=$(printf '%s' "$RESP" | sed 's/__HTTP__[0-9]*__TIME__.*//')

echo "HTTP $CODE  in ${TIME}s"
if printf '%s' "$PAYLOAD" | grep -q '"tool_calls"'; then
  echo "-> calls tools: YES"
else
  echo "-> calls tools: NO  (unusable for this agent)"
  printf '%s' "$PAYLOAD" | head -c 300; echo
fi
