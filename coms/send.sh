#!/bin/bash
# Usage: send.sh AGENT_NAME "message"
COMS="$(dirname "$(realpath "$0")")"
echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] [$1] $2" >> "$COMS/chat.log"
