#!/usr/bin/env bash
# Feed a known signal into the null sink so its monitor is not silence.
set -u
. /tmp/wp02/env.sh
DUR=${1:-30}
ffmpeg -hide_banner -loglevel error -f lavfi \
  -i "sine=frequency=440:sample_rate=48000:duration=$DUR" \
  -ac 2 -f wav /tmp/wp02/tone.wav -y
pw-play --target "$WP02_SINK" /tmp/wp02/tone.wav >/tmp/wp02/logs/pw-play.log 2>&1 &
echo "playing 440 Hz for ${DUR}s into $WP02_SINK (pid $!)"
sleep 1
pw-cli ls Node 2>/dev/null | grep -cE 'node.name'
