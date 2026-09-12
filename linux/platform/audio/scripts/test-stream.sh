#!/bin/bash
# Put a real application stream into the running stack, so routing and
# per-stream volume have something to act on.
#
# The stream is a `pw-play` of a generated tone, given the props a real
# application supplies (application.name, application.process.id,
# application.icon-name, media.name) so the enumeration test can check that
# each one survives the trip through the registry. pw-play does not set
# application.process.id itself; PIPEWIRE_PROPS does, and a real client would.
#
# usage:
#   scripts/test-stream.sh start [name]   start one, print its pid
#   scripts/test-stream.sh stop [name]    stop it
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
DIR=${VS_AUDIO_STACK_DIR:-/tmp/vs-audio-stack}
NAME=${2:-VsTestPlayer}
WAV="$DIR/tone.wav"
PIDFILE="$DIR/stream-$NAME.pid"

die() { echo "test-stream: $*" >&2; exit 1; }

make_tone() {
    [ -f "$WAV" ] && return 0
    # Ten minutes, so a slow test run never has the stream end underneath it.
    # ffmpeg is the generator here only because the container already has it;
    # any silent-but-valid WAV would do, the point is a stream that stays open.
    if command -v ffmpeg >/dev/null 2>&1; then
        ffmpeg -v error -f lavfi -i "sine=frequency=440:duration=600" \
               -ac 2 -ar 48000 "$WAV" -y || die "ffmpeg failed"
    else
        die "no ffmpeg to generate $WAV"
    fi
}

case "${1:-start}" in
start)
    make_tone
    props="{ application.name=$NAME"
    props="$props application.process.id=$$"
    props="$props application.icon-name=audio-x-generic"
    props="$props media.name=VsTestTone }"
    PIPEWIRE_PROPS="$props" \
        setsid "$HERE/run-stack.sh" exec pw-play "$WAV" >/dev/null 2>&1 &
    echo "$!" > "$PIDFILE"
    echo "$!"
    ;;
stop)
    [ -f "$PIDFILE" ] || exit 0
    pid=$(cat "$PIDFILE")
    # pw-play runs under a `run-stack.sh exec`, so the process group is what
    # has to go, not just the shell that started it.
    kill -- "-$pid" 2>/dev/null || kill "$pid" 2>/dev/null
    rm -f "$PIDFILE"
    ;;
*) die "usage: $0 {start|stop} [name]" ;;
esac
