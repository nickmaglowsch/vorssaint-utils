#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# Play a sine into the stack's null sink, so the audio tests capture a signal
# rather than the silence an idle machine produces. A silent capture and a
# broken capture look identical without one.
#
#   play_tone.sh [hz] [seconds]
set -eu

HZ=${1:-440}
SECONDS_TO_PLAY=${2:-8}
STATE=${VS_CAPTURE_STATE:-/tmp/vorssaint-capture}
SINK=${VS_CAPTURE_SINK:-vorssaint-null-sink}
HERE=$(cd "$(dirname "$0")" && pwd)
WAV="$STATE/tone-${HZ}-${SECONDS_TO_PLAY}.wav"

# shellcheck disable=SC1090
. "$STATE/env.sh"

if [ ! -s "$WAV" ]; then
    python3 - "$WAV" "$HZ" "$SECONDS_TO_PLAY" <<'PY'
import math, struct, sys, wave

path, hz, seconds = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
rate, amplitude = 48000, 0.5
frames = int(rate * seconds)
with wave.open(path, "wb") as out:
    out.setnchannels(2)
    out.setsampwidth(2)
    out.setframerate(rate)
    samples = bytearray()
    for i in range(frames):
        value = int(amplitude * 32767 * math.sin(2 * math.pi * hz * i / rate))
        samples += struct.pack("<hh", value, value)
    out.writeframes(bytes(samples))
PY
fi

# --target names the sink; the capture side records that sink's monitor.
exec pw-play --target "$SINK" "$WAV"
