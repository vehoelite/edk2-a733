#!/bin/bash
# Usage: recv.sh [lines_to_tail]
COMS="$(dirname "$(realpath "$0")")"
tail -${1:-20} "$COMS/chat.log" 2>/dev/null
