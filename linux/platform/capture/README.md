# `linux/platform/capture`: the capture engine

Pixels and PCM for the screenshot, recorder, screen-text, QR, colour-picker and
camera features, through `xdg-desktop-portal` and PipeWire. The contract is the
`capture` section of [`../include/vorssaint_platform.h`](../include/vorssaint_platform.h);
everything here implements it and nothing else.

```
capture/
  vs_capture.c          engine lifecycle, capabilities, screenshots, pixel helpers
  vs_capture_portal.c   the ScreenCast and Screenshot Request/Response choreography
  vs_capture_sources.c  monitor enumeration (wl_output) and the PipeWire audio probe
  vs_capture_stream.c   the PipeWire streams, the delivery queues, pause/resume
  vs_capture_timing.c   the recording clock and the region clamp -- pure arithmetic
  vs_capture_png.c      a PNG writer in zlib alone, so no image library is needed
  vs_capture_png_read.c the decoder for what the Screenshot portal hands back
  tools/vs_capture_cli.c  the `vs-capture` harness
  scripts/              the headless portal stack, and the four-build-type matrix
  tests/                ctest: pure checks everywhere, stack checks where there is one
```

What it deliberately does **not** do: encode. WP-B5 owns ffmpeg. This library
ends at raw frames and interleaved float PCM, which is what an encoder wants as
input, and nothing in it knows what a codec is. The WP-02 spike's
`capture.c` keeps the reference encoder for WP-B5 to start from.

## Using it

```c
int reason = VS_OK;
vs_capture_engine *engine = vs_capture_engine_create(NULL, &reason);
if (!engine) { /* reason says why; the feature hub explains it */ }

uint32_t caps = vs_capture_engine_capabilities(engine);
if (!(caps & VS_CAPTURE_HAS_WINDOW_SOURCE)) {
    /* Hide "record this window". Do not offer it and hope. */
}

vs_capture_stream_config config = { 0 };
config.source_types = VS_CAPTURE_SOURCE_MONITOR;
config.max_fps = 30;
config.persist = true;
config.restore_token = saved_token;     /* from the consumer's own settings */
config.capture_system_audio = true;

vs_capture_stream *stream = NULL;
if (vs_capture_stream_start(engine, &config, &stream) != VS_OK) { /* ... */ }
save_token(vs_capture_stream_restore_token(stream));

vs_capture_stream_set_frame_callback(stream, on_frame, self);
vs_capture_stream_set_audio_callback(stream, on_audio, self);

/* Delivery happens here, on this thread. */
struct pollfd fd = { .fd = vs_capture_stream_event_fd(stream), .events = POLLIN };
while (recording) {
    poll(&fd, 1, 20);
    vs_capture_stream_dispatch(stream);
}
vs_capture_stream_stop(stream);
```

`vs-capture` does exactly this; read `tools/vs_capture_cli.c` for the whole of
it, including the pull API and the pause.

## The five things a consumer has to know

**1. What the portal grants is not what you asked for.** `SelectSources` takes
a set of source types and can answer with a different one.
`xdg-desktop-portal-wlr` 0.7.1 serves a WINDOW request as a whole MONITOR, so a
"record this window" button that trusts the call silently records the user's
entire screen. Two defences, both in the API: read
`vs_capture_stream_source_type()` after a start, or set
`config.require_source_type` and get `VS_ERR_NOT_APPLIED` instead of a stream.
This is the capture form of the read-back rule in
[`../README.md`](../README.md).

**2. There is no region source.** No portal backend has one, so
`VS_CAPTURE_HAS_REGION` is always clear — it reports the portal, not us.
`config.region` is a crop the engine performs on the way past, as a pointer
offset into the same buffer, so it costs nothing and the frame's `stride` still
belongs to the full-width source.

**3. Frames are damage-driven.** On wlroots a frame arrives when the output
changes, so a still screen delivers nothing and the rate varies inside one
recording. `max_fps` is a ceiling, never a cadence. Anything that needs a
duration must integrate `frame.timeline_ns`, not multiply a frame count by a
nominal rate; a file built the second way drifts. The measured behaviour is in
[`../../docs/linux-port/CAPTURE_ENGINE.md`](../../docs/linux-port/CAPTURE_ENGINE.md).

**4. One clock, two kinds of buffer.** `timeline_ns` on a frame and on a PCM
buffer come from the same `vs_capture_clock`, and a pause subtracts the same gap
from both. That is the whole of what `RecorderSampleTiming` does on macOS,
which is why the recorder needs no retiming layer of its own here: both sources
already timestamp on `CLOCK_MONOTONIC`, so there is nothing to convert between
and only a pause can pull them apart.

**5. The engine persists nothing.** A restore token comes out of
`vs_capture_stream_restore_token()` and goes back in through
`config.restore_token`. Where it is stored is the consumer's decision — a
settings key, per source — because the engine has no idea which of the user's
sources a token belongs to and should not guess.

## How WP-12's `ScreenCapturer` mirrors it

Same two rules as the window section: the C side owns the names and semantics,
and the Swift wrapper adds no behaviour.

| C | Swift |
|---|---|
| `vs_capture_engine` | `protocol ScreenCapturer` |
| `vs_capture_engine_capabilities` | `var capabilities: ScreenCaptureCapabilities` (`OptionSet`) |
| `vs_capture_source` | `struct CaptureSource` |
| `vs_capture_source_type` | `CaptureSource.Kind` |
| `vs_capture_enumerate_sources` + `vs_capture_free_sources` | one call returning `[CaptureSource]` |
| `vs_capture_screenshot` + `vs_capture_image` | `func image(of:) async throws -> CaptureImage` |
| `vs_capture_stream_config` | `struct CaptureStreamOptions` |
| `vs_capture_stream` | `protocol CaptureStream` |
| `vs_capture_frame` | `struct CaptureFrame` (a borrowed buffer, copied by `withUnsafeBytes` or `vs_capture_image_from_frame`) |
| `vs_capture_audio_buffer` | `struct CaptureAudioBuffer` |
| `event_fd` + `dispatch` + the two callbacks | one `AsyncStream<CaptureBuffer>`, pumped from the fd |
| `pause` / `resume` | `func pause()` / `func resume()` |
| `vs_capture_clock` | `RecorderSampleTiming`'s Linux half, unchanged in behaviour |

The macOS adapter implements the same Swift protocol over ScreenCaptureKit and
AVFoundation, so `ScreenshotService`, `RecorderService` and `ScreenOCRService`
never learn which platform they are on. Two capabilities exist only because
Linux needs them and are simply always set on macOS:
`VS_CAPTURE_HAS_SCREENSHOT_PORTAL` and `VS_CAPTURE_HAS_RESTORE_TOKEN`;
`VS_CAPTURE_HAS_REGION` is the mirror image, always set on macOS
(`SCStreamConfiguration.sourceRect`) and never on Linux.

## `vs_result_string` moved

It was in `window/vs_window.c` when the window section was the only one. It is
now `../vs_result.c`, linked as `vorssaint_platform_common` into both libraries,
so a binary that uses two backends — which the Swift side will — has one
definition rather than one per concern. The strings moved unchanged, including
the two that still say "window" (`VS_ERR_NOT_FOUND`, `VS_ERR_NO_BACKEND`): four
window suites match on that text, and rewording a user-visible surface is a
decision for the lead, not a side effect of adding a second concern. Until it is
made, the capture engine avoids `VS_ERR_NOT_FOUND` on any path a user sees.

## Building and testing

```sh
cmake -S linux/platform -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure -R capture
```

Build dependencies on Ubuntu 24.04, on top of the window section's:

```sh
apt-get install -y libglib2.0-dev libpipewire-0.3-dev libspa-0.2-dev \
                   libwayland-dev zlib1g-dev
```

`capture_support` is pure arithmetic and runs anywhere. The `capture_stack_*`
suites need a headless session and bring one up themselves through
`scripts/run-stack.sh`; each returns ctest's 77 ("not run") with a reason when
a piece is missing, rather than failing a machine that was never going to have a
compositor. Test dependencies: `sway`, `foot`, `pipewire`, `wireplumber`,
`xdg-desktop-portal`, `xdg-desktop-portal-wlr`, `dbus-daemon`, `python3-dbus`,
`python3-gi`, and `meson` for the patched backend below.

```sh
linux/platform/capture/scripts/build-matrix.sh    # all four build types, -Werror, ctest
```

### The stack, and the two things it has to patch

`scripts/run-stack.sh` builds a whole session from nothing inside a container
with no seat, no display and no `/dev/dri`: a private bus, PipeWire with a null
sink and a loopback virtual microphone, headless sway, the portal backend and
the portal frontend. It is idempotent; `paint` puts changing content on the
output, which a damage-driven compositor needs before it will deliver anything
at all.

Two deviations from a stock install, both forced by defects the WP-02 spike
found and documented, and both switchable back so the defect can be reproduced:

- `scripts/build-patched-xdpw.sh` applies `scripts/xdpw-shm-only.patch`, because
  stock `xdg-desktop-portal-wlr` 0.7.1 fails **every** `Start()` on a renderer
  with no DMA-BUF — which is every GPU-less machine, not just this container.
  `VS_CAPTURE_XDPW=/usr/libexec/xdg-desktop-portal-wlr` runs the stock one.
- `scripts/mock-screenshot-impl.py` supplies `org.freedesktop.impl.portal.Access`
  (and a `grim`-backed Screenshot impl), because `xdg-desktop-portal` exports no
  Screenshot portal at all without an `Access` backend and wlroots has none.
  `VS_CAPTURE_ACCESS_SHIM=0` reproduces that, and is what a real bare wlroots
  session looks like — the engine's ScreenCast screenshot path is what answers
  there, and `capture_stack_frame` tests that path first for exactly this
  reason.

Both are properties of the session, not of this container: the support matrix in
`docs/linux-port/CAPTURE_ENGINE.md` says which rows are measured here and which
are expectations for GNOME and KDE.
