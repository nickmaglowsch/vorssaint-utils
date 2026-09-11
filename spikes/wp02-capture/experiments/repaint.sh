#!/usr/bin/env bash
# Replace the `foot` painter with one that repaints at a chosen interval, so we
# can tell a capture-rate ceiling apart from a damage-rate floor.
set -u
. /tmp/wp02/env.sh
INTERVAL=${1:-0.02}
pkill -x foot 2>/dev/null
sleep 1
foot -f 'monospace:size=28' \
  sh -c "while :; do date '+%H:%M:%S.%N'; sleep $INTERVAL; done" \
  >/tmp/wp02/logs/foot.log 2>&1 &
sleep 2
pgrep -x foot >/dev/null && echo "foot repainting every ${INTERVAL}s"
