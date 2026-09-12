#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# One ctest entry per capability the capture engine claims, run against the
# headless wlroots + PipeWire + xdg-desktop-portal stack that scripts/ brings
# up. Everything asserted here is asserted against a real portal and a real
# compositor; nothing is faked but the screen content.
#
#   run_stack_test.sh <name> <path-to-vs-capture>
#
# Exits 77 (ctest's "not run") with a reason when the stack cannot exist on this
# machine, rather than failing a build that was never going to have a
# compositor.
set -u

NAME=${1:?test name}
CAPTURE=${2:?path to vs-capture}
HERE=$(cd "$(dirname "$0")" && pwd)
SCRIPTS="$HERE/../scripts"
STATE=${VS_CAPTURE_STATE:-/tmp/vorssaint-capture}
SINK=${VS_CAPTURE_SINK:-vorssaint-null-sink}
SOURCE=${VS_CAPTURE_SOURCE:-vorssaint-virtual-mic}
OUT="$STATE/test-out"

skip() { echo "SKIP: $*"; exit 77; }
fail() { echo "FAIL: $*"; exit 1; }
say()  { echo "--- $*"; }

# The value of `key=` in a STAT line, wherever in the line it sits: several
# lines carry more than one key, so anchoring at the start would silently miss
# the second and hand the caller an empty string.
stat_of() { grep -o "\\b$2=[^ ]*" "$1" | tail -1 | cut -d= -f2-; }

# ---------------------------------------------------------------- preflight

for tool in sway pipewire wireplumber xdg-desktop-portal dbus-daemon; do
    command -v "$tool" >/dev/null 2>&1 || [ -x "/usr/libexec/$tool" ] \
        || skip "$tool is not installed; the headless portal stack cannot be started"
done
[ -x "$CAPTURE" ] || fail "vs-capture not built at $CAPTURE"

XDPW_PREFIX=${VS_CAPTURE_XDPW_PREFIX:-$STATE/xdpw-patched}
if [ ! -x "$XDPW_PREFIX/libexec/xdg-desktop-portal-wlr" ]; then
    # Stock xdg-desktop-portal-wlr 0.7.1 fails every Start() on a renderer with
    # no DMA-BUF, which is every GPU-less machine (WP-02 defect 1). Build the
    # patched one once; it is cached under $STATE afterwards.
    say "patched xdg-desktop-portal-wlr missing, building it"
    if ! "$SCRIPTS/build-patched-xdpw.sh" > "$STATE-xdpw-build.log" 2>&1; then
        tail -20 "$STATE-xdpw-build.log" || true
        skip "could not build the patched xdg-desktop-portal-wlr (see $STATE-xdpw-build.log); \
stock 0.7.1 cannot screencast without DMA-BUF, so there is no portal backend to test against"
    fi
fi

mkdir -p "$OUT"
"$SCRIPTS/run-stack.sh" start >"$STATE/test-stack.log" 2>&1 \
    || { tail -20 "$STATE/test-stack.log"; skip "the stack would not start (see $STATE/test-stack.log)"; }
# Moving content on screen: without it a damage-driven compositor delivers
# nothing at all, so a frame-rate assertion would be measuring the painter.
"$SCRIPTS/run-stack.sh" paint >>"$STATE/test-stack.log" 2>&1 || true

run() {
    local log=$1; shift
    say "$CAPTURE $*"
    "$SCRIPTS/with-stack.sh" "$CAPTURE" "$@" > "$OUT/$log" 2>&1
    local rc=$?
    sed 's/^/    /' "$OUT/$log"
    return $rc
}

# ---------------------------------------------------------------- the tests

case "$NAME" in

enumerate)
    run probe.txt probe || fail "probe failed"
    types=$(stat_of "$OUT/probe.txt" AvailableSourceTypes)
    [ -n "$types" ] || fail "no AvailableSourceTypes reported"
    grep -q "^STAT AvailableSourceTypes=.*monitor=yes" "$OUT/probe.txt" \
        || fail "the session does not offer a monitor source"
    grep -q "^STAT has_region=no" "$OUT/probe.txt" \
        || fail "has_region must always be no: no portal backend has a region source"

    run sources.txt sources || fail "sources failed"
    count=$(stat_of "$OUT/sources.txt" source_count)
    [ "${count:-0}" -ge 1 ] || fail "no monitors enumerated"
    grep -q "^STAT source .*name=HEADLESS-1" "$OUT/sources.txt" \
        || fail "the stack's output HEADLESS-1 was not enumerated"

    # The honest-reporting half. This backend has no WINDOW bit, and asking for
    # a window anyway is served as a whole monitor -- so the engine must both
    # say so and, when asked to, refuse.
    if grep -q "^STAT has_window_source=no" "$OUT/probe.txt"; then
        grep -q "^STAT window_sources=unsupported" "$OUT/sources.txt" \
            || fail "window sources are unsupported but not reported as such"
        run window.txt stream -d 2 --source-type window --require-source-type
        rc=$?
        grep -q "^STAT stream_start=request acknowledged but state did not change" \
            "$OUT/window.txt" \
            || fail "a WINDOW request served as a MONITOR must be refused, not recorded"
        [ "$rc" -ne 0 ] || fail "the refused stream must not exit 0"
        # And without the flag, the mismatch is still visible rather than hidden.
        run window-observed.txt stream -d 2 --source-type window || true
        grep -q "^STAT stream_start=ok source_type_granted=1 mismatch=yes" \
            "$OUT/window-observed.txt" \
            || fail "the granted source type was not reported back"
    else
        say "this session has a WINDOW bit; the mis-serve check does not apply"
    fi
    ;;

frame)
    # The path every bare wlroots session takes, because it has no
    # impl.portal.Access and therefore no Screenshot portal at all.
    run shot-screencast.txt shot -o "$OUT/screencast.png" --method screencast \
        --timeout-ms 8000 || fail "one-frame ScreenCast screenshot failed"
    grep -q "^STAT shot_method=screencast" "$OUT/shot-screencast.txt" \
        || fail "the ScreenCast path was not the one used"
    colours=$(stat_of "$OUT/shot-screencast.txt" shot_unique_colours)
    [ "${colours:-0}" -ge 100 ] \
        || fail "the captured frame has only ${colours:-0} distinct colours; it is blank"
    [ -s "$OUT/screencast.png" ] || fail "no PNG was written"

    # And the portal path, where the session has one (here, via the stack's
    # Access + Screenshot shim).
    run probe.txt probe || fail "probe failed"
    if grep -q "^STAT has_screenshot_portal=yes" "$OUT/probe.txt"; then
        run shot-portal.txt shot -o "$OUT/portal.png" --method portal \
            || fail "portal Screenshot failed"
        grep -q "^STAT shot_method=portal" "$OUT/shot-portal.txt" \
            || fail "the portal path was not the one used"
        pcolours=$(stat_of "$OUT/shot-portal.txt" shot_unique_colours)
        [ "${pcolours:-0}" -ge 100 ] \
            || fail "the portal screenshot has only ${pcolours:-0} distinct colours"
    else
        say "no Screenshot portal on this session (no impl.portal.Access); \
the ScreenCast fallback above is the whole screenshot story here"
    fi
    ;;

stream)
    for api in callback pull; do
        run "stream-$api.txt" stream -d 5 --max-fps 30 --api "$api" \
            || fail "$api stream failed"
        frames=$(stat_of "$OUT/stream-$api.txt" frames)
        [ "${frames:-0}" -ge 100 ] \
            || fail "$api: ${frames:-0} frames in 5 s, expected at least 100"
        grep -q "^STAT timestamps_monotonic=yes" "$OUT/stream-$api.txt" \
            || fail "$api: timestamps are not monotonic"
        with_pts=$(sed -n 's/^STAT frames=[0-9]* frames_with_pts=\([0-9]*\)/\1/p' \
                   "$OUT/stream-$api.txt" | tail -1)
        [ "${with_pts:-0}" = "${frames:-x}" ] \
            || fail "$api: only ${with_pts:-0} of ${frames:-0} frames carried a \
spa_meta_header; the timeline would be guesswork"
    done
    ;;

region)
    run region.txt stream -d 3 --max-fps 30 --region 100,50,320,240 \
        --save-frame "$OUT/region.png" || fail "region stream failed"
    grep -q "^STAT saved_frame_size=320x240" "$OUT/region.txt" \
        || fail "the engine's crop did not produce a 320x240 frame"
    colours=$(stat_of "$OUT/region.txt" saved_frame_unique_colours)
    [ "${colours:-0}" -ge 20 ] \
        || fail "the cropped frame has only ${colours:-0} colours; the crop may \
be pointing at empty background"
    ;;

audio)
    command -v pw-play >/dev/null 2>&1 || skip "pw-play is not installed"
    "$HERE/play_tone.sh" 440 8 >"$OUT/tone.log" 2>&1 &
    tone_pid=$!
    sleep 1
    # Both kinds at once, which is what a recording with commentary is: the
    # default sink's monitor for system sound, an Audio/Source for the voice.
    run audio.txt stream -d 5 --max-fps 30 --audio-sink "$SINK" --mic "$SOURCE"
    rc=$?
    kill "$tone_pid" 2>/dev/null
    wait "$tone_pid" 2>/dev/null
    [ "$rc" -eq 0 ] || fail "audio stream failed"
    system=$(sed -n 's/^STAT audio_buffers=[0-9]* system=\([0-9]*\).*/\1/p' \
             "$OUT/audio.txt" | tail -1)
    mic=$(sed -n 's/^STAT audio_buffers=[0-9]* system=[0-9]* microphone=\([0-9]*\).*/\1/p' \
          "$OUT/audio.txt" | tail -1)
    [ "${system:-0}" -ge 10 ] \
        || fail "only ${system:-0} system-audio buffers; the sink monitor delivered nothing"
    [ "${mic:-0}" -ge 10 ] \
        || fail "only ${mic:-0} microphone buffers; the source delivered nothing"
    grep -q "^STAT audio_silent=no" "$OUT/audio.txt" \
        || fail "the captured audio is silent; a 440 Hz tone was playing into $SINK"
    peak=$(stat_of "$OUT/audio.txt" audio_peak)
    rms=$(stat_of "$OUT/audio.txt" audio_rms)
    # A 0.5-amplitude sine has an RMS of 0.5/sqrt(2) = 0.354. Checking the shape
    # and not just "louder than zero" is what tells a captured tone from noise,
    # a stuck DC level, or a buffer of uninitialised memory.
    python3 -c "
import sys
peak, rms = float('$peak'), float('$rms')
ok = 0.45 <= peak <= 0.55 and 0.30 <= rms <= 0.38
print(('  ok   ' if ok else '  FAIL ') +
      'captured a 0.5-amplitude sine: peak %.3f, rms %.3f (expect ~0.500, ~0.354)'
      % (peak, rms))
sys.exit(0 if ok else 1)" || fail "the captured audio is not the tone that was played"
    ;;

pause)
    # A synthetic pause in the middle of a recording. The assertion is the one
    # that matters for the recorder: the gap comes off the video and the audio
    # timeline by the same amount, so the two stay together.
    command -v pw-play >/dev/null 2>&1 || skip "pw-play is not installed"
    "$HERE/play_tone.sh" 440 12 >"$OUT/tone.log" 2>&1 &
    tone_pid=$!
    sleep 1
    run pause.txt stream -d 8 --max-fps 30 --audio-sink "$SINK" \
        --pause-at 3 --pause-for 2
    rc=$?
    kill "$tone_pid" 2>/dev/null
    wait "$tone_pid" 2>/dev/null
    [ "$rc" -eq 0 ] || fail "paused stream failed"

    python3 - "$OUT/pause.txt" <<'PY' || exit 1
import re, sys
text = open(sys.argv[1]).read()
def one(pattern):
    m = re.search(pattern, text, re.M)
    if not m:
        print("FAIL: no line matching %s" % pattern)
        sys.exit(1)
    return float(m.group(1))

frame_period_ms = 1000.0 / 30.0
paused = one(r"^STAT paused_total_ms=([0-9.]+)")
video_span = one(r"^STAT video_timeline_ms .*span=([0-9.-]+)")
audio_span = one(r"^STAT audio_timeline_ms .*span=([0-9.-]+)")
start_skew = one(r"^STAT alignment_start_skew_ms=([0-9.]+)")
end_skew = one(r"^STAT alignment_end_skew_ms=([0-9.]+)")
span_skew = one(r"alignment_span_skew_ms=([0-9.]+)")
gap = one(r"^STAT resumed_at_ms=[0-9.]+ gap_ms=([0-9.]+)")
dropped = one(r"^STAT frames_dropped rate=[0-9]+ paused=([0-9]+)")

ok = True
def check(condition, message):
    global ok
    print(("  ok   " if condition else "  FAIL ") + message)
    ok = ok and condition

check(abs(paused - gap) < 20,
      "the engine recorded the gap the harness measured: %.1f vs %.1f ms" % (paused, gap))
check(paused > 1900, "the pause really lasted ~2 s: %.1f ms" % paused)
check(dropped > 0, "frames arriving during the pause were dropped: %d" % dropped)
# The recording is as long as it was recording: 8 s wall, 2 s paused, ~6 s of
# timeline. A regression that forgot to subtract the gap would land near 8000.
check(abs(video_span - 6000) < 400, "video timeline spans ~6 s: %.1f ms" % video_span)
check(abs(audio_span - 6000) < 400, "audio timeline spans ~6 s: %.1f ms" % audio_span)
# Alignment, stated where it is actually bounded. The two kinds do not arrive
# at the same instant -- audio lands every ~10 ms, video every ~33 -- so at any
# one instant the timelines can differ by up to a frame interval, and no more.
# The span is the difference of two such instants, one at each end of the run,
# so its bound is two intervals; asserting one there would be asserting that
# the two streams started and stopped simultaneously, which they never do.
check(start_skew <= frame_period_ms,
      "video and audio start within one frame period: %.1f <= %.1f ms"
      % (start_skew, frame_period_ms))
check(end_skew <= frame_period_ms,
      "video and audio end within one frame period: %.1f <= %.1f ms"
      % (end_skew, frame_period_ms))
check(span_skew <= 2 * frame_period_ms,
      "video and audio spans agree within two frame periods: %.1f <= %.1f ms"
      % (span_skew, 2 * frame_period_ms))
sys.exit(0 if ok else 1)
PY
    ;;

token)
    tokenfile="$OUT/restore-token"
    rm -f "$tokenfile"
    run token1.txt stream -d 2 --max-fps 15 --restore-token "$tokenfile" \
        || fail "first token run failed"
    grep -q "^STAT restore_token_sent=(none)" "$OUT/token1.txt" \
        || fail "the first run should have sent no token"
    first=$(stat_of "$OUT/token1.txt" restore_token_received)
    [ -n "$first" ] && [ "$first" != "(absent)" ] \
        || skip "this session minted no restore token (persist_mode unsupported)"
    [ "$(cat "$tokenfile")" = "$first" ] \
        || fail "the token the engine returned was not the one persisted"

    run token2.txt stream -d 2 --max-fps 15 --restore-token "$tokenfile" \
        || fail "second token run failed"
    grep -q "^STAT restore_token_sent=$first" "$OUT/token2.txt" \
        || fail "the stored token was not sent on the second run"
    second=$(stat_of "$OUT/token2.txt" restore_token_received)
    [ -n "$second" ] && [ "$second" != "(absent)" ] \
        || fail "the restored session minted no token"
    frames=$(stat_of "$OUT/token2.txt" frames)
    [ "${frames:-0}" -gt 0 ] || fail "the restored session captured nothing"
    say "token round trip: $first -> $second"
    ;;

*)
    fail "unknown test '$NAME'"
    ;;
esac

echo "PASS: capture_stack_$NAME"
