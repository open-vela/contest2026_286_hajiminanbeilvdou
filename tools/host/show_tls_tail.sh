#!/bin/bash
# Everything the device said after its TLS handshake with the LLM endpoint.
#
# Variables assigned inline keep failing to expand through the wsl.exe layer,
# so the extraction lives in a file.

TXT=/tmp/console_text.txt
strings -n 5 "$HOME/bridge.raw" > "$TXT" 2>/dev/null

n=$(grep -n 'Handshake OK' "$TXT" | tail -1 | cut -d: -f1)

if [ -z "$n" ]; then
  echo "no successful handshake in the capture; last TLS lines instead:"
  grep -n 'vela_tls' "$TXT" | tail -10
  exit 0
fi

echo "=== handshake succeeded at line $n, from there on ==="
sed -n "${n},$((n + 40))p" "$TXT"
