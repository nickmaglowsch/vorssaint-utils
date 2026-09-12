# The Linux capture engine (WP-B1)

`linux/platform/capture` is the backend behind screenshot, screen recording,
screen text, QR, colour picker and camera preview on Linux. This file is the
measured behaviour and the support matrix; the API is the `capture` section of
`linux/platform/include/vorssaint_platform.h`, and how to use it is
`linux/platform/capture/README.md`.

Everything in the "measured" column below was produced by `vs-capture` on the
headless stack that `linux/platform/capture/scripts/run-stack.sh` brings up, on
the machine described in § 1, and the command that produced each number is
given. **GNOME and KDE cannot be run here at all** — no seat, no `/dev/dri`, a
microVM kernel with no loadable modules — so their cells are expectations from
`PLAN.md` § 6, marked as such, to be replaced with measurements by the squads
that reach a real session.

---

## 1. What this was measured on

| Piece | Version | Note |
|---|---|---|
| CPU | 4 cores, 2.1 GHz Xeon | software rendering throughout |
| sway | 1.9-1build1 | `WLR_BACKENDS=headless`, `WLR_RENDERER=pixman` |
| wlroots | 0.17.1-2.1build1 | advertises `zwlr_screencopy_manager_v1` v3 |
| PipeWire | 1.0.5-1ubuntu3.3 | + `wireplumber` 0.4.17, `pipewire-pulse` |
| xdg-desktop-portal | 1.18.4-1ubuntu2.24.04.2 | the frontend |
| xdg-desktop-portal-wlr | 0.7.1-1build2, **patched** | see § 2 |
| Output | HEADLESS-1, 1280x720@60 | `max_fps=30` in the xdpw config |

The screen carries a solid background plus a `foot` terminal printing a
nanosecond timestamp every 10 ms, so the compositor is damaged continuously and
a frame-rate measurement measures the capture path rather than the painter.

---

## 2. The two things the stack has to patch, and why they are not local quirks

Both were found by the WP-02 spike (`spikes/02-capture.md` §§ 2–3) and both are
properties of a *session*, not of this container. The engine is built around
them rather than around their absence.

**Stock `xdg-desktop-portal-wlr` 0.7.1 cannot screencast without DMA-BUF.** Its
`start_screencast()` treats a missing DMA-BUF format as fatal whenever the
compositor advertises screencopy v3, which sway always does, so every `Start()`
fails on a pixman-rendered session — a VM without virtio-gpu, a broken GPU
driver, some VNC setups. `scripts/xdpw-shm-only.patch` makes the DMA-BUF format
optional and keeps the SHM one required. The engine's own answer is to report a
failed `Start` as "this session cannot screencast" rather than a generic error,
so the feature hub can say something true.

**`org.freedesktop.portal.Screenshot` does not exist on a bare wlroots
session.** The frontend will not export it without an
`org.freedesktop.impl.portal.Access` backend, and wlroots implements none; the
same gate removes `Camera`, `Device` and `Location`. This is why
`vs_capture_screenshot` has two paths and why the ScreenCast one is tested
first: on a real Sway desktop it is the *only* path. The stack runs a
`grim`-backed Access + Screenshot shim so the portal path is exercised too;
`VS_CAPTURE_ACCESS_SHIM=0` reproduces the bare session.

Drafts of the upstream reports are in `docs/linux-port/spikes/upstream/`. None
has been submitted; filing against another project's tracker is the repository
owner's call.

---

## 3. Measured numbers

### 3.1 Enumeration and capabilities

```
$ linux/platform/capture/scripts/with-stack.sh vs-capture probe
STAT backend=portal
STAT AvailableSourceTypes=1 monitor=yes window=no virtual=no
STAT AvailableCursorModes=3
STAT has_screenshot_portal=yes      # via the stack's Access shim; no on a bare session
STAT has_window_source=no
STAT has_region=no
STAT has_restore_token=yes
STAT has_audio_monitor=yes
STAT has_microphone=yes
STAT has_virtual_source=no
STAT has_cursor_embedded=yes

$ ... vs-capture sources
STAT source_count=1
STAT source id=44 type=1 name=HEADLESS-1 bounds=1280x720+0+0 valid=yes \
     refresh_mhz=60000 scale=1.00 description=Headless output 1
STAT window_sources=unsupported (AvailableSourceTypes=1 has no WINDOW bit; \
     a WINDOW request on this backend is served as a MONITOR)
```

The monitor list comes from `wl_output`, because the portal has no enumeration
call at all — it reports which *kinds* of source exist and then shows its own
chooser. `has_region` is hard-clear by construction: no portal backend has a
region source anywhere, so the bit reports the portal and the engine crops.

### 3.2 A WINDOW request is refused, not silently widened

The privacy bug this exists to prevent: xdpw 0.7.1 accepts `types=WINDOW` and
returns a stream whose own `source_type` is MONITOR — the whole screen.

```
$ ... vs-capture stream -d 2 --source-type window --require-source-type
STAT stream_start=request acknowledged but state did not change   # VS_ERR_NOT_APPLIED
$ ... vs-capture stream -d 2 --source-type window
STAT stream_start=ok source_type_granted=1 mismatch=yes
```

Both are asserted by `ctest -R capture_stack_enumerate`.

### 3.3 One frame, both paths

```
$ ... vs-capture shot -o screencast.png --method screencast
STAT shot_result=ok elapsed_ms=59.3
STAT shot_method=screencast
STAT shot_size=1280x720 stride=5120 format=BGRx bytes=3686400
STAT shot_unique_colours=1284 mean_luma=47.2 blank=no

$ ... vs-capture shot -o portal.png --method portal
STAT shot_result=ok elapsed_ms=76.9
STAT shot_method=portal
STAT shot_size=1280x720 stride=5120 format=RGBA bytes=3686400
STAT shot_unique_colours=1284 mean_luma=47.0 blank=no
```

The two paths return the same picture — 1284 distinct colours either way — which
is the cross-check that the ScreenCast fallback is a real screenshot and not a
degraded one. Both are under 80 ms, comfortably inside "feels instant".

The PNG writer is ~200 lines over zlib, so the engine carries no image library.
An independent reader agrees with it, on the file and on the colour count:

```
$ identify screencast.png
screencast.png PNG 1280x720 1280x720+0+0 8-bit sRGB 90519B
$ convert screencast.png -format 'unique=%k\n' info:
unique=1284
```

### 3.4 Frame stream

Ten seconds, both delivery modes, `max_fps` set above the compositor's own cap
so the ceiling being measured is the session's:

| Mode | frames | fps | with `spa_meta_header` | latency avg / min / max | CPU |
|---|---|---|---|---|---|
| dispatch (default, copies) | 295 | 29.49 | 295 / 295 | 1.097 / 0.808 / 3.705 ms | 2.5 % of one core |
| `direct_callbacks` (zero copy) | 291 | 29.09 | 291 / 291 | 1.066 / 0.808 / 2.260 ms | 0.5 % of one core |

```
$ ... vs-capture stream -d 10 --max-fps 60
STAT api=callback-dispatch
STAT frames=295 frames_with_pts=295
STAT fps=29.49
STAT timestamps_monotonic=yes non_monotonic=0
STAT latency_ms avg=1.097 min=0.808 max=3.705
STAT cpu_seconds=0.250 cpu_percent_of_one_core=2.5
```

Three things to read out of this.

**Latency is a non-issue.** 1.1 ms from the compositor's capture instant
(`spa_meta_header.pts`) to delivery, 3.7 ms worst case over ~300 frames, on a
software renderer. It matches the WP-02 spike's 1.15 ms to within noise.

**The cost of the safe default is the copy, and it is 2 % of a core.** The
dispatch mode copies each frame (1280x720x4 = 3.7 MB, ~109 MB/s at 29.5 fps) so
that nothing of the caller's runs on the PipeWire thread. WP-B5 can have the
other 2 % back with `direct_callbacks` when it is ready to encode in place.
Neither figure includes encoding: the WP-02 spike measured 42 % of a core for
the same stream through `libx264 -preset veryfast`, so the encoder, not the
capture, is what the recorder's energy badge should reflect.

**Every frame carried a header.** `frames == frames_with_pts` in every run, which
is what makes a real timeline possible rather than a frame count times a
nominal rate.

### 3.5 Frame delivery is damage-driven

This is the single most important behaviour for WP-B5. `wlr-screencopy`
delivers a frame when the output has damage, so the rate floats between zero and
xdpw's `max_fps` inside one recording:

| screen repaint interval | delivered fps |
|---|---|
| 10 ms (the test painter) | 29.5 (pinned to the `max_fps=30` ceiling) |
| 200 ms | 5.0 (WP-02 § 5.1, same stack) |
| nothing moving | 0 — no frames at all |

So `max_fps` is a ceiling, never a cadence. A recorder that multiplies a frame
count by a nominal rate produces a file whose duration drifts; `RecorderTimeline`
must integrate `frame.timeline_ns`. The engine's own cap is enforced on
`pts`, so it composes with the compositor's rather than fighting it: over 5 s
at `--max-fps 30` the engine dropped 9 frames the compositor offered above the
cap and delivered 138.

### 3.6 Audio

Both kinds at once, with a 0.5-amplitude 440 Hz sine playing into the stack's
null sink:

```
$ ... vs-capture stream -d 5 --max-fps 30 \
      --audio-sink vorssaint-null-sink --mic vorssaint-virtual-mic
STAT audio_buffers=234 system=117 microphone=117 dropped_paused=0
STAT audio_frames system=239616 microphone=239616
STAT audio_peak=0.499969 audio_rms=0.352778 samples=958464
STAT audio_silent=no
```

A 0.5-amplitude sine has an RMS of 0.5/√2 = 0.3536; the captured 0.35278 is that
sine and not noise, a DC offset, or uninitialised memory. 239 616 frames at
48 kHz is 4.99 s of audio for a 5 s recording. `capture_stack_audio` asserts
the shape, not merely "louder than zero".

The system side is the default sink's monitor (`stream.capture.sink=true`); the
microphone side is an ordinary `Audio/Source`. Both are on a **second, ordinary
`pw_context_connect()`**, never the fd from `OpenPipeWireRemote` — that one is a
restricted connection exposing only the screencast node, and an audio stream on
it sits in `paused` forever. This is a property of the portal frontend, so it
will hold on GNOME and KDE too.

### 3.7 Pause and resume keep the two timelines together

The Linux form of `RecorderSampleTiming`: both kinds of buffer already timestamp
on `CLOCK_MONOTONIC`, so there is nothing to convert between, and the only thing
that can pull them apart is a pause. One gap, subtracted from both.

Verbatim from the `capture_stack_pause` run, which is where the assertions
below are checked:

```
$ ... vs-capture stream -d 8 --max-fps 30 --audio-sink vorssaint-null-sink \
      --pause-at 3 --pause-for 2
STAT resumed_at_ms=5003.5 gap_ms=1986.8
STAT frames=172 frames_with_pts=172
STAT frames_dropped rate=4 paused=58 queue=0 buffers_missed=0
STAT paused_total_ms=1986.8
STAT video_timeline_ms first=39.7 last=6017.5 span=5977.7
STAT audio_timeline_ms first=50.5 last=5999.5 span=5949.0
STAT alignment_start_skew_ms=10.8
STAT alignment_end_skew_ms=18.0 alignment_span_skew_ms=28.7
```

Eight seconds of wall clock, 1.987 s of it paused, and both timelines span
~5.96 s: the recording is as long as it was recording. The 58 frames that
arrived during the pause were dropped rather than timestamped. A regression that
forgot to subtract the gap from one clock would put that span near 8000 and the
skew near 2000, so the test is decisive rather than decorative.

`capture_stack_pause` asserts the start and end skews at **one frame period**
(33.3 ms) each and the span skew at **two**. That is where the quantity is
actually bounded: audio lands every ~10 ms and video every ~33, so at any one
instant the two timelines can differ by up to a frame interval — and the span is
the difference of two such instants, one at each end of the run. Asserting one
frame period on the span would be asserting that the two streams start and stop
simultaneously, which they never do.

### 3.8 Restore tokens

From the `capture_stack_token` run:

```
### first run
STAT restore_token_sent=(none)
STAT restore_token_received=c983a79c-caee-4d2a-8377-c9f87afbb5fb
STAT frames=29 frames_with_pts=29
### second run, the token the consumer stored handed back
STAT restore_token_sent=c983a79c-caee-4d2a-8377-c9f87afbb5fb
STAT restore_token_received=c983a79c-caee-4d2a-8377-c9f87afbb5fb
STAT frames=29 frames_with_pts=29
```

The engine returns the token and accepts one; it stores nothing itself, because
it has no idea which of the user's sources a token belongs to. WP-02 § 6 proved
by A/B — with xdpw's config pointed at an output that does not exist, a fresh
session is refused and a token-carrying one still captures — that the token
genuinely restores the *source* and is not just a UUID that round-trips.

### 3.9 Region

The portal has no region source, so the engine crops: a pointer offset into the
same packed buffer, with the frame's original stride, which costs nothing.

```
$ ... vs-capture stream -d 3 --max-fps 30 --region 100,50,320,240 \
      --save-frame region.png
STAT negotiated=1280x720 format=BGRx
STAT saved_frame_size=320x240 stride=1280 format=BGRx bytes=307200
STAT saved_frame_unique_colours=1284 mean_luma=73.7 blank=no
```

The stream negotiated the full 1280x720 and the delivered frame is 320x240: the
crop is the engine's, not the portal's. The saved image is packed to its own
width (`stride=1280` = 320 x 4) while the frame the callback saw kept the
source's 5120-byte stride, which is why the API says never to assume the two
are equal.

The clamp is unit-tested against partly-outside, negative-origin and
entirely-outside rectangles in `capture_support`; a region with nothing left is
refused rather than silently emptied.

### 3.10 Built and tested in five configurations

`linux/platform/capture/scripts/build-matrix.sh` configures the whole platform
tree four times over the CMake build types and once more under
AddressSanitizer, UndefinedBehaviorSanitizer and LeakSanitizer. All five build
warning-free under `-Werror`, and the capture suites pass in all five.

The sanitizer leg paid for itself on its first run, which is the argument for
having it:

```
Direct leak of 64 byte(s) in 1 object(s) allocated from:
    #8 g_dbus_connection_call_sync
    #9 vs_portal_session_close  linux/platform/capture/vs_capture_portal.c:363
    #10 vs_capture_stream_stop  linux/platform/capture/vs_capture_stream.c:667
SUMMARY: AddressSanitizer: 64 byte(s) leaked in 1 allocation(s).
```

`Session.Close` returns an empty tuple that is of no interest and is still a
`GVariant` the caller owns — 64 bytes per recording, invisible to four clean
`-Werror` builds. Fixed, and the leg is now silent.

The leg ships **no suppression file**. One was written for GLib and PipeWire
and then deleted, because it proved to suppress nothing: those libraries keep
process-global state until exit, but it stays *reachable*, and LeakSanitizer
reports lost memory rather than unfreed memory. A suppression list that
suppresses nothing today would quietly cover a real leak in the same libraries
tomorrow.

---

## 4. Support matrix

Rows are marked **measured** (a command above produced it) or **expected**
(`PLAN.md` § 6, unverified here — no GNOME or KDE session can exist on this
machine, and neither can be conjured: no seat, no `/dev/dri`, no loadable
modules).

| Capability | wlroots (sway 1.9 + xdpw 0.7.1) | GNOME (Wayland) | KDE Plasma 6 |
|---|---|---|---|
| `ScreenCast` portal | **measured** ✓ v5 | expected ✓ | expected ✓ |
| Monitor source | **measured** ✓ 1280x720 BGRx | expected ✓ | expected ✓ |
| Window source | **measured** ✗ — no WINDOW bit, and a WINDOW request is served as a MONITOR (§ 3.2) | expected ✓ | expected ✓ |
| Region source at the portal | **measured** ✗ (nowhere: the engine crops) | ✗ by spec | ✗ by spec |
| Monitor enumeration (`wl_output`) | **measured** ✓ name, bounds, refresh, scale | expected ✓ | expected ✓ |
| `Screenshot` portal | **measured** ✗ on a bare session (no `impl.portal.Access`); ✓ with the shim | expected ✓ | expected ✓ |
| Screenshot via a ScreenCast frame | **measured** ✓ 59 ms, same pixels as the portal path | expected ✓ | expected ✓ |
| `spa_meta_header` on every frame | **measured** ✓ 295/295 | expected ✓ | expected ✓ |
| Capture latency | **measured** 1.1 ms avg, 3.7 ms max | expected lower (DMA-BUF) | expected lower |
| Frame delivery | **measured** damage-driven, ceiling = xdpw `max_fps` | expected fixed-rate from mutter | expected fixed-rate |
| DMA-BUF buffers | **measured** ✗ — no `/dev/dri`, SHM only | expected ✓ | expected ✓ |
| Works without a GPU | **measured** ✗ stock xdpw; ✓ patched (§ 2) | expected ✓ | expected ✓ |
| Restore token issued and honoured | **measured** ✓ (§ 3.8) | expected ✓ | expected ✓ |
| Cursor modes | **measured** ✓ hidden + embedded (metadata rejected) | expected 3 | expected 3 |
| System audio from a sink monitor | **measured** ✓ 440 Hz verified (§ 3.6) | expected ✓ (same PipeWire API) | expected ✓ |
| Microphone from an `Audio/Source` | **measured** ✓ (§ 3.6) | expected ✓ | expected ✓ |
| Audio over the portal's PipeWire fd | **measured** ✗ restricted; needs a second connect | expected same (a frontend property) | expected same |
| Pause/resume keeps A/V aligned | **measured** ✓ within one frame period (§ 3.7) | expected ✓ (engine-side arithmetic) | expected ✓ |

X11 is not in this table: the portal is the only backend, and an X11 session
that runs `xdg-desktop-portal` at all serves whichever backend its desktop
installed. A session with no ScreenCast portal gets `VS_ERR_NO_BACKEND` from
`vs_capture_engine_create` and the feature hub explains it; a dedicated
XComposite/XShm backend is not in WP-B1's scope and should be opened as its own
work package if the triage matrix wants one.

---

## 5. What this binds for later packages

1. **WP-B5 (recorder).** Drive the timeline from `frame.timeline_ns`, never from
   a frame count; expect a variable rate with a floor of zero. Pause/resume is
   already correct in the engine — do not add a second gap accumulator on top.
   Use `direct_callbacks` to encode in place and save the ~2 % of a core the
   copy costs. Raw frames are BGRx/RGBA with a stride that may exceed the width.
2. **WP-B2 (screenshot).** `has_screenshot_portal` is false on a whole class of
   sessions; the ScreenCast path is not a degraded fallback but the normal one
   there, and it costs a portal round trip plus a repaint, so the selector
   should start the session before the user has finished choosing where possible.
3. **WP-C3 (switcher previews).** Per-window preview needs
   `VS_CAPTURE_HAS_WINDOW_SOURCE`. Where it is clear, there is no window preview
   to be had through the portal at any price, and the switcher shows an icon.
   `VS_WINDOW_HAS_PREVIEWS` on the window backend is what joins the two.
4. **WP-B8 (camera preview).** The `Camera` portal is behind the same
   `impl.portal.Access` gate as `Screenshot`, so a bare wlroots session has none
   and v4l2 directly is the only route there.
5. **WP-P3 (headless CI).** The capture suites cannot use a distro
   `xdg-desktop-portal-wlr` as-is. Either bundle the patch (the test driver
   builds it once and caches it), give the runner a DRM device, or let the
   suites skip — they return ctest's 77 with the reason rather than failing.
6. **WP-12 (`ScreenCapturer`).** The Swift protocol landed while this was
   being written and lines up on almost everything. Five places it does not,
   each a real difference between ScreenCaptureKit and a portal rather than a
   defect on either side, are listed in `linux/platform/capture/README.md`. The
   two that change behaviour a user can see: `CaptureTarget.window(id)` is not
   expressible on a portal at all (the portal picks, the app cannot name a
   window), and `CaptureStreamOptions.excludedWindows` has no portal
   equivalent — the recorder's own overlay will appear in a Linux recording
   unless the recorder hides it while recording.
7. **A decision for the lead.** `vs_result_string` moved to
   `linux/platform/vs_result.c` so two backend libraries can link into one
   binary; its `VS_ERR_NOT_FOUND` and `VS_ERR_NO_BACKEND` strings still say
   "window", which four window suites match on. Rewording them is a
   user-visible rename and was left alone.
