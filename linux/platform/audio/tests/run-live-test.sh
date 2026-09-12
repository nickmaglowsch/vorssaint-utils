#!/bin/bash
# Run one live audio case inside a private, throwaway PipeWire stack.
#
# Each case gets its *own* stack, in its own directory, rather than sharing
# one: the cases mutate global state (default sink, stream volumes, and
# default_sink destroys a sink outright), so sharing would make them
# order-dependent and the failures unreadable. Bringing a stack up costs a few
# seconds, which is the right price for tests that mean something on their own.
#
# Exits 77 -- ctest's "not run" -- when the stack cannot be built here, so a
# machine without PipeWire reports the cases as skipped rather than passed.
#
# usage: run-live-test.sh <test-binary> <case>
set -u

BINARY=${1:?usage: run-live-test.sh <test-binary> <case>}
CASE=${2:?usage: run-live-test.sh <test-binary> <case>}
SCRIPTS=${VS_AUDIO_SCRIPTS:-$(cd "$(dirname "$0")/../scripts" && pwd)}

for tool in pipewire wireplumber pw-cli pw-play dbus-daemon; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "SKIP: $tool is not installed" >&2
        exit 77
    fi
done
if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "SKIP: ffmpeg is not installed (needed to generate the test tone)" >&2
    exit 77
fi

export VS_AUDIO_STACK_DIR="${TMPDIR:-/tmp}/vs-audio-test-$CASE-$$"
cleanup() {
    "$SCRIPTS/test-stream.sh" stop >/dev/null 2>&1
    "$SCRIPTS/run-stack.sh" stop >/dev/null 2>&1
    rm -rf "$VS_AUDIO_STACK_DIR"
}
trap cleanup EXIT

if ! "$SCRIPTS/run-stack.sh" start >/dev/null 2>&1; then
    echo "SKIP: could not start a private PipeWire stack" >&2
    sed 's/^/  /' "$VS_AUDIO_STACK_DIR/pipewire.log" 2>/dev/null >&2
    exit 77
fi

# pipewire-pulse is only needed by the fallback case, but starting it always
# keeps the stack one thing rather than two shapes of it. If its socket never
# appeared, that case cannot run.
if [ "$CASE" = pulse_fallback ] && [ ! -S "$VS_AUDIO_STACK_DIR/run/pulse/native" ]; then
    echo "SKIP: pipewire-pulse did not publish a socket" >&2
    exit 77
fi

# Every case except the device-only ones wants an application stream to act on.
"$SCRIPTS/test-stream.sh" start >/dev/null 2>&1 || {
    echo "SKIP: could not start the test stream" >&2
    exit 77
}

"$SCRIPTS/run-stack.sh" exec env VS_AUDIO_SCRIPTS="$SCRIPTS" "$BINARY" "$CASE"
status=$?
if [ "$status" -ne 0 ]; then
    echo "--- pipewire log ---" >&2
    tail -40 "$VS_AUDIO_STACK_DIR/pipewire.log" >&2 2>/dev/null
    echo "--- wireplumber log ---" >&2
    tail -40 "$VS_AUDIO_STACK_DIR/wireplumber.log" >&2 2>/dev/null
fi
exit "$status"
