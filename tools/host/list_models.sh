#!/bin/bash
# Ask the configured endpoint which models it actually offers.
#
# Worth checking rather than assuming: a model id that does not exist fails at
# the API with a 4xx that looks nothing like "wrong name", and the ones that
# do exist are not all able to call tools -- which is the only thing this
# agent uses a model for.

set -u
ENV_FILE="$HOME/.llm_env"
[ -r "$ENV_FILE" ] || { echo "missing $ENV_FILE"; exit 1; }
# shellcheck disable=SC1090
. "$ENV_FILE"

# /v1/chat/completions -> /v1/models
BASE=$(printf '%s' "$LLM_PATH" | sed 's#/chat/completions$##')
URL="https://$LLM_HOST:$LLM_PORT$BASE/models"

echo "GET $URL"
echo
timeout 40 curl -sS "$URL" -H "Authorization: Bearer $LLM_API_KEY" 2>&1 \
  | tr ',' '\n' | grep -oE '"id":"[^"]+"' | sed 's/"id":"//;s/"//' | sort
