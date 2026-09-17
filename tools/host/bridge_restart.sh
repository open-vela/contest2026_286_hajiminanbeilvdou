#!/bin/bash
# Restart the PTY bridge cleanly (patterns stay in this file, never on the
# caller's command line).
pkill -f "bridge\.py" 2>/dev/null
sleep 1
rm -f "$HOME/bridge.raw"
setsid nohup python3 -u "$HOME/bridge.py" > "$HOME/bridge.log" 2>&1 &
sleep 3
head -1 "$HOME/bridge.log"
ls -la "$HOME/ttyHS"
