#!/usr/bin/env bash
# Three 10-second A/V captures, with `top -b` sampling the process externally
# as a cross-check on the /proc/self/stat number the CLI prints itself.
set -u
. /tmp/wp02/env.sh
CAP=/tmp/wp02/build/capture

# keep the screen changing faster than the 30 fps cap for the whole run
pkill -x foot 2>/dev/null; sleep 1
foot -f 'monospace:size=28' sh -c 'while :; do date "+%H:%M:%S.%N"; sleep 0.01; done' \
  >/tmp/wp02/logs/foot.log 2>&1 &
sleep 2
# keep the sink monitor non-silent
ffmpeg -hide_banner -loglevel error -f lavfi \
  -i "sine=frequency=440:sample_rate=48000:duration=120" -ac 2 -f wav /tmp/wp02/tone.wav -y
pw-play --target "$WP02_SINK" /tmp/wp02/tone.wav >/dev/null 2>&1 &
sleep 1

for i in 1 2 3; do
  echo "================= run $i ================="
  $CAP screencast -o "/tmp/wp02/out/measure$i.mp4" -d 10 --audio-sink "$WP02_SINK" \
    2>/dev/null &
  CPID=$!
  sleep 2
  # sample the real process for 6 s, 1 Hz
  top -b -n 6 -d 1 -p "$CPID" 2>/dev/null \
    | awk '/capture/ {printf "top %%CPU=%s RES=%s\n", $9, $6}'
  wait $CPID
done
