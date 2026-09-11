#!/usr/bin/env bash
# Reproduce the stock xdg-desktop-portal-wlr 0.7.1 failure on a GPU-less
# wlroots session, then switch back to the patched build.
set -u
. /tmp/wp02/env.sh

run_with() {
  local bin=$1 label=$2
  pkill -f 'libexec/xdg-desktop-portal-wlr' 2>/dev/null
  sleep 1
  "$bin" -l DEBUG >"/tmp/wp02/logs/xdpw-$label.log" 2>&1 &
  sleep 2
  echo "### $label: $bin"
  /tmp/wp02/build/capture screencast -o "/tmp/wp02/out/$label.mp4" -d 2 2>&1 \
    | grep -E 'SelectSources_response|Start_response|^STAT frames=|ERROR'
  echo "--- xdpw said ---"
  grep -E 'valid format|screencopy|buffer_type' "/tmp/wp02/logs/xdpw-$label.log" | tail -4
  echo
}

run_with /usr/libexec/xdg-desktop-portal-wlr stock
run_with /tmp/wp02/xdpw-patched/libexec/xdg-desktop-portal-wlr patched
