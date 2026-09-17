#!/bin/bash
# Collect the real LLM endpoint into ~/.llm_env.
#
#   ./setup_llm.sh
#
# Asks for each value instead of taking them as arguments, so the API key
# never lands in the command line, the shell history, or anything that
# captures either. The file is written with mode 600 and lives outside every
# repository, so it cannot reach git or the submitted patch set.

set -u

ENV_FILE="$HOME/.llm_env"

echo "Where will the agent send its LLM requests?"
echo
echo "  1) DeepSeek        api.deepseek.com            deepseek-chat"
echo "  2) 阿里云百炼       dashscope.aliyuncs.com      qwen-plus"
echo "  3) 智谱 GLM        open.bigmodel.cn            glm-4-flash"
echo "  4) Moonshot        api.moonshot.cn             moonshot-v1-8k"
echo "  5) OpenAI          api.openai.com              gpt-4o-mini"
echo "  6) 其他 / custom"
echo
read -rp "choose [1-6]: " choice

case "$choice" in
  1) DEF_HOST="api.deepseek.com";       DEF_PATH="/v1/chat/completions";                 DEF_MODEL="deepseek-chat" ;;
  2) DEF_HOST="dashscope.aliyuncs.com"; DEF_PATH="/compatible-mode/v1/chat/completions"; DEF_MODEL="qwen-plus" ;;
  3) DEF_HOST="open.bigmodel.cn";       DEF_PATH="/api/paas/v4/chat/completions";        DEF_MODEL="glm-4-flash" ;;
  4) DEF_HOST="api.moonshot.cn";        DEF_PATH="/v1/chat/completions";                 DEF_MODEL="moonshot-v1-8k" ;;
  5) DEF_HOST="api.openai.com";         DEF_PATH="/v1/chat/completions";                 DEF_MODEL="gpt-4o-mini" ;;
  *) DEF_HOST="";                       DEF_PATH="/v1/chat/completions";                 DEF_MODEL="" ;;
esac

echo
read -rp "host   [$DEF_HOST]: " HOST
HOST="${HOST:-$DEF_HOST}"

read -rp "path   [$DEF_PATH]: " PATHV
PATHV="${PATHV:-$DEF_PATH}"

read -rp "model  [$DEF_MODEL]: " MODEL
MODEL="${MODEL:-$DEF_MODEL}"

read -rp "port   [443]: " PORT
PORT="${PORT:-443}"

# -s so it is not echoed; nothing here touches the history file.
echo
read -rsp "api key (not shown): " KEY
echo

if [ -z "$HOST" ] || [ -z "$MODEL" ] || [ -z "$KEY" ]; then
  echo
  echo "host, model and key are all required; nothing written." >&2
  exit 1
fi

umask 077
cat > "$ENV_FILE" <<EOF
# Real LLM endpoint for the agent. Outside every repository on purpose.
# Written by setup_llm.sh on $(date '+%Y-%m-%d %H:%M').
LLM_HOST=$HOST
LLM_PORT=$PORT
LLM_PATH=$PATHV
LLM_MODEL=$MODEL
LLM_API_KEY=$KEY
EOF
chmod 600 "$ENV_FILE"

echo
echo "wrote $ENV_FILE:"
sed -E 's/^(LLM_API_KEY=).*/\1<set>/' "$ENV_FILE" | sed 's/^/  /'
echo
echo "next:  ./set_llm.sh real     (points the device at it)"
