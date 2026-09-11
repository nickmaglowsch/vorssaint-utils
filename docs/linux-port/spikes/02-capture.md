# WP-02 spike: portal ScreenCast + Screenshot + PipeWire capture

Status: **go, with three named defects to design around.**

The whole pipeline the port needs — portal `ScreenCast` → PipeWire node →
`libx264` H.264 → MP4, plus system audio from a PipeWire sink monitor as AAC,
plus a portal `Screenshot` — works end to end on a wlroots session, at 29.5 fps
for 1280x720, with ~1.2 ms of capture latency and 42 % of one CPU core. Restore
tokens work and genuinely persist the source.

Three things do **not** work out of the box on this backend, each proven below
with an A/B:

1. `xdg-desktop-portal-wlr` 0.7.1 **cannot screencast at all** on a wlroots
   session whose renderer has no DMA-BUF (no `/dev/dri`). One-line fix.
2. The **`Screenshot` portal is not exported at all** on a plain sway session,
   because no wlroots backend implements `org.freedesktop.impl.portal.Access`.
3. `SelectSources` with `types=WINDOW` is **silently served as a full-monitor
   cast** by xdpw 0.7.1 (an off-by-one in its type check). An app that offers
   "record this window" would silently record the whole screen.

Code: `spikes/wp02-capture/` (`run-stack.sh`, `capture.c`, `CMakeLists.txt`,
`mock-screenshot-impl.py`, `xdpw-shm-only.patch`, `build-patched-xdpw.sh`,
`with-stack.sh`).

Each of the three defects has a drafted upstream report under
`docs/linux-port/spikes/upstream/` — `xdpw-shm-only-start.md`,
`xdpw-window-type-bit.md`, `xdpw-no-access-impl.md`. **None has been
submitted:** filing against another project's tracker is the repository
owner's call.

---

## 1. What this was run on

No GNOME and no KDE session exists in this container, and none can be created:
there is no seat, no login session, no `/dev/dri`, and the kernel
(`6.18.44-fc-v24`, a microVM kernel) ships no loadable modules, so no DRM device
can be conjured. **The GNOME and KDE columns of the matrix in § 8 are therefore
marked "not testable here", not guessed at.** What *was* built is the only real
portal stack this box can host, and it is a genuine one — a real wlroots
compositor, the real reference portal frontend, and the real wlroots portal
backend:

| Piece | Version | Notes |
|---|---|---|
| kernel | 6.18.44-fc-v24 | microVM, no DRM, no `/dev/dri`, no `/lib/modules` |
| CPU | 4 × Intel Xeon @ 2.10 GHz | all software rendering and encoding |
| sway | 1.9-1build2 | `WLR_BACKENDS=headless WLR_RENDERER=pixman` |
| wlroots | 0.17.1-2.1build1 | advertises `zwlr_screencopy_manager_v1` v3 |
| PipeWire | 1.0.5-1ubuntu3.3 | + `wireplumber` 0.4.17, `pipewire-pulse` |
| xdg-desktop-portal | 1.18.4-1ubuntu2.24.04.2 | the frontend |
| xdg-desktop-portal-wlr | 0.7.1-1build2 | the backend under test |
| ffmpeg libs | libavcodec 60.31.102, libavformat 60.16.100 | Ubuntu 24.04 |

### 1.1 The stack script

`spikes/wp02-capture/run-stack.sh` brings the whole thing up from nothing, and
is idempotent:

```
$ ./spikes/wp02-capture/run-stack.sh start
[run-stack] session bus: unix:path=/tmp/dbus-Kbuco1lZVe,guid=5c70cf5bbd73d149b195fb4b6aa48055
[run-stack] pipewire up, sink 'wp02-null-sink'
[run-stack] sway up on WAYLAND_DISPLAY=wayland-1 SWAYSOCK=/tmp/wp02/run/sway-ipc.0.24004.sock
[run-stack] xdpw binary: /tmp/wp02/xdpw-patched/libexec/xdg-desktop-portal-wlr
[run-stack] Access + Screenshot shim up
[run-stack] portal frontend + backends up
[run-stack] wrote /tmp/wp02/env.sh -- source it to use the stack

$ ./spikes/wp02-capture/run-stack.sh status
dbus-daemon          25321 28225
pipewire             28227
wireplumber          28231
pipewire-pulse       28235
sway                 28245
foot                 29573
xdg-desktop-portal   28275 28724
xdg-desktop-portal-wlr 29281
```

In order it: writes a private `XDG_RUNTIME_DIR`/`XDG_CONFIG_HOME`; forks a
private session bus (`dbus-daemon --session --fork --print-address=3`); starts
`pipewire`, `wireplumber` and `pipewire-pulse` with a `conf.d` drop-in that
instantiates a `support.null-audio-sink` named `wp02-null-sink` (so there is a
sink *and a monitor source* to capture "system audio" from); starts sway
headless with a one-output config; starts the portal backend; and starts
`xdg-desktop-portal`. `run-stack.sh paint` then puts non-uniform, *changing*
content on the output — a solid blue background plus a `foot` terminal printing
a nanosecond timestamp in a loop — so nothing downstream can pass by capturing
a flat colour.

The chooser is made non-interactive exactly as the WP asked, via
`$XDG_CONFIG_HOME/xdg-desktop-portal-wlr/config`:

```ini
[screencast]
output_name=HEADLESS-1
max_fps=30
chooser_type=none
```

`chooser_type=none` makes xdpw take `output_name` directly instead of
exec'ing `slurp`/`wofi`. This is the same mechanism a CI job would use.

Two deviations from a stock install are needed, both forced by the defects in
§ 2 and § 3 and both switchable back with an env var so the defect can be
reproduced: `WP02_XDPW` selects the stock or patched backend binary, and
`WP02_ACCESS_SHIM=0` disables the `Access` shim.

Screen content, verified rather than assumed (`grim` straight off the
compositor):

```
$ ./spikes/wp02-capture/with-stack.sh grim /tmp/wp02/grim-check.png
$ identify /tmp/wp02/grim-check.png
/tmp/wp02/grim-check.png PNG 1280x720 1280x720+0+0 8-bit sRGB 92280B
$ convert /tmp/wp02/grim-check.png -format 'mean=%[mean] stddev=%[standard-deviation] uniquecolors=%k\n' info:
mean=12002.8 stddev=11494.3 uniquecolors=1284
```

---

## 2. Defect 1 — xdpw 0.7.1 cannot screencast without DMA-BUF

Stock `xdg-desktop-portal-wlr` 0.7.1 fails every `Start()`:

```
$ capture screencast -o /tmp/wp02/out/cast.mp4 -d 10
STAT SelectSources_response=0 types_requested=1 persist_mode=2
STAT Start_response=2
[capture] ERROR: Start failed

# /tmp/wp02/logs/xdpw.log
wlroots: unable to receive a valid format from wlr_screencopy
```

The cause is in `src/screencast/screencast.c:199` of the 0.7.1 source
(`apt-get source xdg-desktop-portal-wlr`):

```c
if (cast->screencopy_frame_info[WL_SHM].format == DRM_FORMAT_INVALID ||
        (cast->ctx->state->screencast_version >= 3 &&
         cast->screencopy_frame_info[DMABUF].format == DRM_FORMAT_INVALID)) {
    logprint(INFO, "wlroots: unable to receive a valid format from wlr_screencopy");
    return -1;
}
```

When the compositor advertises `zwlr_screencopy_manager_v1` **v3** — sway 1.9 /
wlroots 0.17 always does — xdpw treats a missing DMA-BUF format as fatal. On a
pixman-rendered or GPU-less session the compositor simply never sends the
`linux_dmabuf` event, so this is unconditionally fatal, even though the
shared-memory path is fine: 40 lines further down, `build_formats()` already
falls back to an SHM-only `EnumFormat` when no modifier list can be built, and
the entire buffer path handles `buffer_type == WL_SHM`.

`spikes/wp02-capture/xdpw-shm-only.patch` makes the DMA-BUF format optional and
keeps the SHM one required; `build-patched-xdpw.sh` fetches, patches and builds
it into `/tmp/wp02/xdpw-patched`. Same stack, same config, only the backend
binary swapped (`/tmp/wp02/stock-xdpw.sh`):

```
### stock: /usr/libexec/xdg-desktop-portal-wlr
STAT SelectSources_response=0 types_requested=1 persist_mode=2
STAT Start_response=2
[capture] ERROR: Start failed
--- xdpw said ---
wlroots: |-- registered to interface zwlr_screencopy_manager_v1 (Version 3)
wlroots: unable to receive a valid format from wlr_screencopy

### patched: /tmp/wp02/xdpw-patched/libexec/xdg-desktop-portal-wlr
STAT SelectSources_response=0 types_requested=1 persist_mode=2
STAT Start_response=0
STAT frames=15 frames_with_header=15 dropped=0
--- xdpw said ---
wlroots: |-- registered to interface zwlr_screencopy_manager_v1 (Version 3)
pipewire: buffer_type: 0 (4)      # 0 == WL_SHM
```

**What this means for the port.** It is not just a container artefact. Any user
on a wlroots session whose compositor runs the pixman renderer (`WLR_RENDERER=pixman`,
a VM without virtio-gpu, a broken GPU driver, some remote/VNC setups) will get a
`Start` failure with no explanation. The port must therefore (a) treat a
`Start` response of 2 as "this session cannot screencast" and say so in the
feature hub rather than showing a generic error, and (b) the headless CI harness
in WP-P3 cannot use the distro's xdpw as-is — it needs either a patched build or
a DRM device. This is worth an upstream report.

---

## 3. Defect 2 — the Screenshot portal is absent on a wlroots session

On the stack as installed, `org.freedesktop.portal.Screenshot` does not exist:

```
$ capture probe
STAT org.freedesktop.portal.ScreenCast version=5
STAT org.freedesktop.portal.Screenshot version=UNAVAILABLE (GDBus.Error:org.freedesktop.DBus.Error.InvalidArgs: No such interface "org.freedesktop.portal.Screenshot")
STAT AvailableSourceTypes=1 monitor=yes window=no virtual=no
STAT AvailableCursorModes=3
```

This is not xdpw failing to implement it — xdpw *does* export
`org.freedesktop.impl.portal.Screenshot` (version 1) and `wlr.portal` declares
it. The frontend refuses to build the skeleton:

```
$ G_MESSAGES_DEBUG=all /usr/libexec/xdg-desktop-portal -v
XDP: loading /usr/share/xdg-desktop-portal/portals/wlr.portal
XDP: portal implementation supports org.freedesktop.impl.portal.Screenshot
XDP: portal implementation supports org.freedesktop.impl.portal.ScreenCast
...
XDP: providing portal org.freedesktop.portal.Realtime
xdg-desktop-portal-WARNING **: No skeleton to export
XDP: Found 'wlr' in configuration for default
XDP: Using wlr.portal for org.freedesktop.impl.portal.ScreenCast (config)
XDP: providing portal org.freedesktop.portal.ScreenCast
```

I isolated the cause with `mock-screenshot-impl.py`, a stand-in
`impl.portal.Screenshot` backend that takes real pixels via `grim` (so it goes
through the same `zwlr_screencopy_v1` xdpw would use) and whose advertised
`version` and extra interfaces are command-line controlled. Three arms, one
variable each (`/tmp/wp02/screenshot-experiment.sh`):

| Arm | mock exports | frontend `org.freedesktop.portal.Screenshot` |
|---|---|---|
| A | `impl.portal.Screenshot` **version 1** | absent |
| B | `impl.portal.Screenshot` **version 2** | absent |
| C | `impl.portal.Screenshot` v2 **+ `impl.portal.Access`** | **present, version 2** |

Arm C:

```
### mock impl backend advertises:
(<uint32 2>,)
### xdp lines mentioning Screenshot:
XDP: Using wp02mock.portal for org.freedesktop.impl.portal.Screenshot in sway (fallback)
XDP: providing portal org.freedesktop.portal.Screenshot
### frontend interfaces:
  interface org.freedesktop.portal.Location {
  interface org.freedesktop.portal.Device {
  interface org.freedesktop.portal.WebExtensions {
  interface org.freedesktop.portal.Screenshot {
  interface org.freedesktop.portal.ScreenCast {
  interface org.freedesktop.portal.Camera {
  ...
### frontend Screenshot version:
(<uint32 2>,)
```

So: **the impl version is irrelevant; what is missing is an
`org.freedesktop.impl.portal.Access` backend.** `xdg-desktop-portal`'s
`Screenshot` frontend does its own consent step ("Allow Applications to Take
Screenshots?") through `Access`, and `xdg-desktop-portal-wlr` implements no
`Access` at all. The same gate takes out `Camera`, `Device`, `Location` and
`WebExtensions` — they appear in arm C and are absent otherwise.

`ScreenCast` is unaffected because its frontend delegates the whole consent UI
to the backend's own `SelectSources`, so it never needs `Access`.

**What this means for the port.** On GNOME and KDE this never bites: their
portal backends implement `Access` themselves. On a bare wlroots session
(sway + xdpw and nothing else) the screenshot feature must fall back to
`zwlr_screencopy_v1` directly, or to `grim`, and the feature hub must explain
why. The port should probe `org.freedesktop.portal.Screenshot` at startup
rather than assuming the portal spec's interfaces are all present.

For the rest of this spike the stack runs the mock as an `Access` +
`Screenshot` shim (`WP02_ACCESS_SHIM=1`, the default), so the portal
choreography in `capture.c` is exercised for real.

---

## 4. Screenshot: works, ~45 ms

`capture screenshot` calls `org.freedesktop.portal.Screenshot.Screenshot` with
`parent_window=""` and `interactive=false`, waits for the `Request` `Response`
signal, and copies the returned URI:

```
$ capture screenshot -o /tmp/wp02/out/shot.png
STAT screenshot_response=0 elapsed_ms=37.8
STAT screenshot_uri=file:///tmp/wp02/mock-shots/tmp88diosqo.png
STAT screenshot_saved=/tmp/wp02/out/shot.png

$ identify /tmp/wp02/out/shot.png
/tmp/wp02/out/shot.png PNG 1280x720 1280x720+0+0 8-bit sRGB 93220B
$ convert /tmp/wp02/out/shot.png -format 'mean=%[mean] stddev=%[standard-deviation] colors=%k\n' info:
mean=12013 stddev=11514.4 colors=1284
```

`interactive=false` is honoured and reaches the backend verbatim (the shim logs
what it received):

```
[mock] AccessDialog 'Allow Applications to Take Screenshots?' -> auto-allow
[mock] Screenshot handle=/org/freedesktop/portal/desktop/request/1_57/shot_19359_1 app_id='' interactive=False options={'interactive': False, 'permission_store_checked': True}
```

Note `permission_store_checked: True` — the frontend consulted
`xdg-permission-store` before asking, and after the first grant the `Access`
dialog is not shown again. Five consecutive calls (`/tmp/wp02/shot-timing.sh`):

```
STAT screenshot_response=0 elapsed_ms=43.4
STAT screenshot_response=0 elapsed_ms=45.3
STAT screenshot_response=0 elapsed_ms=57.6
STAT screenshot_response=0 elapsed_ms=44.9
STAT screenshot_response=0 elapsed_ms=36.9
```

37–58 ms end to end (D-Bus round trip + a full `zwlr_screencopy` frame + PNG
encode), on a software renderer. That is comfortably inside "feels instant".

---

## 5. ScreenCast → H.264 → MP4

`capture screencast` does the full choreography: `CreateSession` →
`SelectSources` (`types=1`, `multiple=false`, `cursor_mode=2`,
`persist_mode=2`) → `Start` → `OpenPipeWireRemote`, then opens the returned
node with `pw_stream` on a `pw_thread_loop`, negotiates a raw video format,
and encodes each buffer through `sws_scale` → `libx264` → the MP4 muxer.

```
STAT CreateSession_response=0
STAT session_handle=/org/freedesktop/portal/desktop/session/1_18/sess_6384_2
STAT SelectSources_response=0 types_requested=1 persist_mode=2
STAT Start_response=0
STAT Start_results={'streams': <[(uint32 51, {'position': <(0, 0)>, 'size': <(1280, 720)>, 'source_type': <uint32 1>})]>, 'restore_token': <'8a93b8eb-2518-4d4a-ac56-d959d93856aa'>}
STAT stream_node=51 props={'position': <(0, 0)>, 'size': <(1280, 720)>, 'source_type': <uint32 1>}
STAT pipewire_fd=7 node_id=51
[capture] negotiated video: BGRx 1280x720 @ 30/1
STAT negotiated_format=BGRx width=1280 height=720
```

Format negotiation settles on `BGRx` — xdpw offers only SHM here, so the
buffers are plain `mmap`ed BGRx with a `spa_meta_header`. The CLI requests the
header meta explicitly in `param_changed` and gets it on **every** frame
(`frames == frames_with_header` in every run below), which is what makes the
latency number possible.

### 5.1 The frame rate is damage-driven, not clock-driven

This is the single most important behavioural finding for the recorder feature.
`wlr-screencopy` delivers a frame when the output has damage. With the screen
repainting 5×/s, the cast runs at 5 fps, not 30:

```
# foot printing a timestamp every 0.2 s
STAT frames=52 frames_with_header=52 dropped=0
STAT fps_over_capture_window=4.99
```

With the same stack and the same config, but the screen repainting every
0.01 s, it pins to the configured `max_fps=30`:

```
# foot printing a timestamp every 0.01 s
STAT frames=305 frames_with_header=305 dropped=0
STAT fps_over_capture_window=29.42
STAT fps_first_to_last_frame=29.52
```

So on wlroots the port gets a variable-frame-rate stream whose ceiling is
xdpw's `max_fps` and whose floor is whatever the screen does. A recorder that
assumes constant frame rate will produce files whose duration drifts. `capture.c`
handles this by timestamping from `spa_meta_header.pts` into a 1 µs codec
timebase rather than counting frames — the resulting MP4 is genuinely VFR
(`ffprobe` reports `29.75 fps` nominal against `56 tbr`) and its duration
matches wall clock to 0.07 s over 10 s.

### 5.2 Measurements

Three back-to-back 10-second captures with audio, screen repainting at ~100 Hz,
with `top -b -d 1` sampling the process externally as a cross-check on the
`/proc/self/stat` figure the CLI reports for itself (`/tmp/wp02/measure.sh`):

| run | frames | fps | latency avg / min / max (ms) | CPU (self) | CPU (`top`) | RES |
|---|---|---|---|---|---|---|
| 1 | 292 | 29.57 | 1.153 / 0.835 / 3.475 | 42.5 % | 40–50 % | 99.8 MB |
| 2 | 293 | 29.47 | 1.165 / 0.864 / 3.395 | 42.5 % | 30–45 % | 99.7 MB |
| 3 | 294 | 29.59 | 1.164 / 0.856 / 2.941 | 42.0 % | 39–50 % | 100.0 MB |

`dropped=0` in all three (no `pw_stream_dequeue_buffer` returned NULL).

The latency proxy is `CLOCK_MONOTONIC` at the moment the CLI's `process`
callback runs, minus `spa_meta_header.pts` of that buffer — i.e. compositor
capture timestamp to application delivery, covering the screencopy copy, the
PipeWire buffer handoff and the scheduling. **~1.2 ms typical, 3.5 ms worst
case** across ~880 frames. That is the transport cost only; encode happens after
this point and is counted in the CPU figure, not the latency figure.

CPU is 42 % of one core for 1280x720@30 with `libx264 -preset veryfast -tune
zerolatency`, on a 2.1 GHz Xeon with **software** rendering and a BGRx→YUV420P
`sws_scale` on the CPU. On real hardware with a GPU, xdpw would hand over
DMA-BUFs and the colour conversion could move to the GPU, so this is an upper
bound, not a typical one. `top` agrees with `/proc/self/stat` to within its own
sampling noise, so the self-measurement can be trusted.

Commands, verbatim:

```
$ ./spikes/wp02-capture/run-stack.sh start
$ ./spikes/wp02-capture/run-stack.sh paint
$ ./spikes/wp02-capture/with-stack.sh /tmp/wp02/build/capture \
      screencast -o /tmp/wp02/out/measure1.mp4 -d 10 --audio-sink wp02-null-sink
```

### 5.3 The produced file

```
$ ffprobe -hide_banner /tmp/wp02/out/measure1.mp4
  Duration: 00:00:09.81, start: 0.000000, bitrate: 4127 kb/s
  Stream #0:0[0x1](und): Video: h264 (High) (avc1 / 0x31637661), yuv420p(progressive), 1280x720, 3990 kb/s, 29.75 fps, 56 tbr, 1000k tbn (default)
  Stream #0:1[0x2](und): Audio: aac (LC) (mp4a / 0x6134706D), 48000 Hz, stereo, fltp, 127 kb/s (default)

$ ffprobe -v error -show_entries stream=codec_name,codec_type,width,height,nb_frames,duration ... cast-av.mp4
index=0 codec_name=h264 codec_type=video width=1280 height=720 nb_frames=298 duration=10.072026
index=1 codec_name=aac  codec_type=audio sample_rate=48000 channels=2 nb_frames=475 duration=10.112000
format_name=mov,mp4,m4a,3gp,3g2,mj2 duration=10.112000 size=5160749 bit_rate=4082871
```

Frames are not a flat colour — the whole clip is checked, not one frame:

```
$ ffmpeg -i measure1.mp4 -vf signalstats,metadata=print:file=- -f null -
lavfi.signalstats.YAVG avg=56.346 max=56.698 n=292
lavfi.signalstats.YDIF avg=4.528 max=5.099 n=292
```

`YDIF` (mean absolute luma difference from the previous frame) is 4.5 on every
one of the 292 frames: the content genuinely changes frame to frame, it is not a
frozen or blank capture. A decoded frame from the middle of the clip:

```
$ ffmpeg -ss 5 -i cast-av.mp4 -frames:v 1 frame5s.png
$ convert frame5s.png -format 'mean=%[mean] stddev=%[standard-deviation] colors=%k\n' info:
mean=11930.9 stddev=11138.5 colors=25396
```

25 396 distinct colours and a per-frame `YMIN=5 … YMAX=255` spread — the
terminal text and background survived the round trip.

### 5.4 System audio

The sink monitor is captured by a **second** `pw_stream` with
`stream.capture.sink=true` and `target.object=wp02-null-sink`, encoded to AAC
and muxed into the same MP4:

```
STAT audio_target=wp02-null-sink
STAT audio_encoder=aac rate=48000 channels=2 frame_size=1024
STAT audio_negotiated rate=48000 channels=2
STAT audio_buffers=230 audio_samples=471040 (9.81 s)
```

One non-obvious requirement, found the hard way: **the audio stream must not use
the PipeWire connection returned by `OpenPipeWireRemote`.** That fd is a
*restricted* connection whose permissions expose only the screencast node; an
audio stream created on it negotiates no format and sits in `paused` forever
(`audio_buffers=0`). `capture.c` therefore opens a second, ordinary
`pw_context_connect()` to the user's PipeWire daemon for audio — which is also
correct for the shipping app, since system-audio capture is not a
portal-mediated permission on any target desktop.

The captured audio is the signal that was played, not silence. A 440 Hz sine was
played into the null sink with `pw-play --target wp02-null-sink`, and the AAC
track from the MP4 was analysed:

```
$ ffmpeg -i cast-av.mp4 -map 0:a -af volumedetect -f null -
n_samples: 970752
mean_volume: -24.1 dB
max_volume: -20.8 dB

$ ffmpeg -i cast-av.mp4 -map 0:a -ss 3 -t 1 -ac 1 -ar 48000 -f f32le - | <DFT peak>
peak bin k=75 -> 439.5 Hz (magnitude 504.4)
```

439.5 Hz against a 440 Hz source, with a 5.86 Hz bin width — the tone, captured
through the sink monitor, encoded, muxed and decoded back.

---

## 6. Restore tokens: they work, and they really persist the source

`SelectSources` is called with `persist_mode=2` and, on a second run, the token
from the first. The same token comes back:

```
########## RUN 1 ##########
STAT restore_token_sent=(none)
STAT Start_results={'streams': ..., 'restore_token': <'e1096758-b6ae-487e-938b-063da5600c34'>}
STAT restore_token_received=e1096758-b6ae-487e-938b-063da5600c34

########## RUN 2 (token reused) ##########
STAT restore_token_sent=e1096758-b6ae-487e-938b-063da5600c34
STAT SelectSources_response=0
STAT restore_token_received=e1096758-b6ae-487e-938b-063da5600c34
```

and xdpw confirms it acted on the stored data rather than re-choosing:

```
dbus: restore data available
dbus: restoring session from data
dbus: option restore_data.output_name: HEADLESS-1
```

Returning the same UUID is not by itself proof that the *source* was restored,
so here is an A/B that forces the question (`/tmp/wp02/token-ab.sh`). xdpw's
config is pointed at an output that does not exist, then the same binary is
asked for a cast twice:

```
### break the config: output_name=NO-SUCH-OUTPUT
--- A: fresh session, no token (expect failure) ---
STAT SelectSources_response=1 types_requested=1 persist_mode=2
[capture] ERROR: SelectSources refused (response=1)

--- B: same broken config, WITH the stored token (expect success) ---
STAT restore_token_sent=baac28ca-4c14-4b1d-8810-ff041d1c8430
STAT SelectSources_response=0 types_requested=1 persist_mode=2
STAT Start_response=0
STAT frames=25 frames_with_header=25 dropped=0

--- what xdpw logged for B ---
dbus: restore data available
dbus: option restore_data.output_name: HEADLESS-1
wlroots: capturable output: Unknown model: Unknown: id: 44 name: HEADLESS-1
```

With the config broken, a fresh session has nothing to capture and is refused;
the token-carrying session still captures `HEADLESS-1`, because the output name
came out of the stored restore data. **The restore token genuinely persists the
selected source on `xdg-desktop-portal-wlr` 0.7.1.**

`persist_mode=0` correctly yields no token:

```
STAT SelectSources_response=0 types_requested=1 persist_mode=0
STAT restore_token_received=(absent)
```

The WP text anticipated that xdpw might have no persistence support. It does —
this is the one place the backend exceeded expectations. Note the token is
minted and stored by the *frontend* (`xdg-permission-store`), with the backend
supplying opaque `restore_data`; the port stores only the UUID.

---

## 7. Defect 3 — `types=WINDOW` is silently served as a monitor

`AvailableSourceTypes` correctly says monitor-only:

```
STAT AvailableSourceTypes=1 monitor=yes window=no virtual=no
```

But requesting `WINDOW` anyway does **not** fail. It returns a stream whose own
`source_type` is `1` (MONITOR) — the entire output:

```
$ capture screencast --source-type window -d 2
STAT SelectSources_response=0 types_requested=2 persist_mode=2
STAT Start_results={'streams': <[(uint32 45, {'position': <(0, 0)>, 'size': <(1280, 720)>, 'source_type': <uint32 1>})]>, ...}
```

`types=3` (monitor|window) behaves the same. The cause is an off-by-one in
xdpw 0.7.1, `src/screencast/screencast.c:349`:

```c
} else if (strcmp(key, "types") == 0) {
        uint32_t mask;
        sd_bus_message_read(msg, "v", "u", &mask);
        if (mask & (1<<WINDOW)) {
                logprint(INFO, "dbus: non-monitor cast requested, not replying");
                return -1;
        }
```

with, in `include/screencast_common.h:23`:

```c
enum source_types {
  MONITOR = 1,
  WINDOW = 2,
};
```

The enumerators are already bit *values*, not bit *indices*, so `1<<WINDOW` is
`4` — the **VIRTUAL** bit — and a WINDOW request (`2`) sails straight through
the guard. Confirmed by asking for the bit it actually checks:

```
$ capture screencast --source-type virtual -d 2
STAT SelectSources_response=2 types_requested=4 persist_mode=2
[capture] ERROR: SelectSources refused (response=2)

# xdpw log
dbus: non-monitor cast requested, not replying
```

`types=4` trips the guard; `types=2` does not. (Response 2 rather than 1 because
xdpw returns `-1` without replying at all, so the frontend synthesises an error.)

**What this means for the port.** A "record this window" feature must never
trust `SelectSources` succeeding. It must read `AvailableSourceTypes` first and
hide the option when the WINDOW bit is clear, *and* check the `source_type` of
each returned stream in `Start`'s results before recording — otherwise a user
who asks for one window silently gets their whole screen, including whatever
else is on it. That is a privacy bug, not a cosmetic one.

---

## 8. Support matrix

Filled from evidence for wlroots. GNOME and KDE are **not testable here** — no
seat, no `/dev/dri`, no way to run mutter or kwin_wayland — so their cells state
the `PLAN.md` § 6 expectation and nothing more, to be re-verified by the squad
that owns the recorder feature on real sessions.

| Capability | wlroots (sway 1.9 + xdpw 0.7.1) — evidence | GNOME (Wayland) | KDE Plasma 6 |
|---|---|---|---|
| `ScreenCast` portal present | ✓ version 5 (`capture probe`) | not testable here; expected ✓ per PLAN.md § 6 | not testable here; expected ✓ per PLAN.md § 6 |
| Monitor source | ✓ 1280x720 BGRx, 29.5 fps, 880 frames over 3 runs | not testable here; expected ✓ | not testable here; expected ✓ |
| Window source | ✗ `AvailableSourceTypes=1`; requesting it silently yields a **monitor** (§ 7) | not testable here; expected ✓ | not testable here; expected ✓ |
| Region source | ✗ not offered by the portal at all on any backend (region is the app's crop) | – | – |
| Cursor modes | ✓ `AvailableCursorModes=3` (hidden + embedded); metadata mode rejected by xdpw | not testable here; expected 3 | not testable here; expected 3 |
| Frame delivery | ✓ damage-driven, ceiling = xdpw `max_fps`; 5 fps at 5 Hz damage, 29.4 fps at 100 Hz damage (§ 5.1) | not testable here; expected fixed-rate from mutter | not testable here |
| `spa_meta_header` on every frame | ✓ `frames == frames_with_header` in every run | not testable here; expected ✓ | not testable here; expected ✓ |
| Capture latency | ✓ avg 1.15 ms, max 3.5 ms (§ 5.2) | not testable here | not testable here |
| DMA-BUF buffers | ✗ none — no `/dev/dri`; SHM only (`buffer_type: 0`) | not testable here; expected ✓ | not testable here; expected ✓ |
| Works without a GPU | ✗ **stock xdpw 0.7.1 fails outright** (§ 2); ✓ with the one-line patch | not testable here | not testable here |
| Restore token issued | ✓ UUID returned with `persist_mode=2`, absent with `0` (§ 6) | not testable here; expected ✓ | not testable here; expected ✓ |
| Restore token restores the source | ✓ proven by the broken-config A/B (§ 6) | not testable here; expected ✓ | not testable here; expected ✓ |
| `Screenshot` portal present | ✗ **absent**: no `impl.portal.Access` backend (§ 3) | not testable here; expected ✓ | not testable here; expected ✓ |
| `Screenshot` `interactive=false` | ✓ with an `Access` shim: 37–58 ms, correct pixels (§ 4) | not testable here; expected ✓ | not testable here; expected ✓ |
| System audio from a sink monitor | ✓ `stream.capture.sink=true` + `target.object`, 440 Hz verified out of the MP4 (§ 5.4) | not testable here; expected ✓ (same PipeWire API) | not testable here; expected ✓ |
| Audio over the portal's PipeWire fd | ✗ restricted connection; needs a second `pw_context_connect` (§ 5.4) | expected same — it is a frontend property, not a backend one | expected same |

---

## 9. Encoder and the ffmpeg link approach

**Chosen: `libx264`.** It is present in Ubuntu 24.04's `libavcodec60`, and it
is the only H.264 encoder usable on this machine:

```
$ ffmpeg -encoders | grep -Ei 'h264|openh264|x264|mpeg4|aac'
 V....D libx264              libx264 H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 (codec h264)
 V....D h264_nvenc           NVIDIA NVENC H.264 encoder (codec h264)
 V..... h264_qsv             H.264 / AVC (Intel Quick Sync Video acceleration) (codec h264)
 V..... h264_v4l2m2m         V4L2 mem2mem H.264 encoder wrapper (codec h264)
 V....D h264_vaapi           H.264/AVC (VAAPI) (codec h264)
 V.S... mpeg4                MPEG-4 part 2
 A....D aac                  AAC (Advanced Audio Coding)
```

There is **no `libopenh264` encoder in the Ubuntu ffmpeg build** (the grep
returns nothing for it; `libopenh264-dev` exists as a package but the distro
`libavcodec` is not built against it). `h264_nvenc`, `h264_qsv` and `h264_vaapi`
are listed but all need hardware this box does not have. `capture.c` therefore
tries, in order, `libx264` → `libopenh264` → `h264_vaapi` → `mpeg4`, and prints
which one it got:

```
[capture] video encoder: libx264
STAT video_encoder=libx264
```

Audio: the native `aac` encoder, no external library, F32 interleaved from
PipeWire converted to FLTP with `swresample`.

**The licensing catch, which is a decision for the lead.** Ubuntu's
`libavcodec60` is a **GPL** build (`--enable-gpl --enable-libx264`). Linking the
app against it is fine when the app uses the distro's shared library, but
`PLAN.md` § 6 already flags that bundling x264 into an AppImage pulls GPL into
the bundle. The spike's evidence on that trade-off:

- Using the system `libavcodec` via `pkg-config` is trivial — the entire build
  is nine `pkg_check_modules` lines and works unmodified — but ties the port to
  whatever the distro shipped, which on Ubuntu 24.04 means codec availability
  varies between Ubuntu (x264 present), Fedora (`ffmpeg-free`, **no** x264 and
  no H.264 encoder at all) and Flatpak runtimes (openh264 only).
- So the AppImage/Flatpak build should bundle its own `libavcodec` configured
  **without** `--enable-gpl`, with `--enable-libopenh264` plus VA-API, and
  `capture.c`'s existing preference list already degrades correctly: it would
  pick `libopenh264` and print it. A `mpeg4`-in-MP4 last resort keeps the
  feature alive even then, at obviously worse quality.
- WP-04 should treat "which H.264 encoder ships in the bundle" as one of its
  explicit outputs. This spike does not settle it; it only proves that the code
  path is encoder-agnostic and that the distro build works today.

---

## 10. Reproducing all of it

```
# 1. the stack (idempotent; ~8 s)
./spikes/wp02-capture/run-stack.sh start
./spikes/wp02-capture/run-stack.sh paint
./spikes/wp02-capture/run-stack.sh status

# 2. the patched portal backend (only needed once; see § 2)
./spikes/wp02-capture/build-patched-xdpw.sh

# 3. the CLI
cmake -S spikes/wp02-capture -B /tmp/wp02/build -G Ninja
cmake --build /tmp/wp02/build

# 4. the proofs
W=./spikes/wp02-capture/with-stack.sh
$W /tmp/wp02/build/capture probe
$W /tmp/wp02/build/capture screenshot -o /tmp/wp02/out/shot.png
$W /tmp/wp02/build/capture screencast -o /tmp/wp02/out/cast.mp4 -d 10 \
      --audio-sink wp02-null-sink
$W /tmp/wp02/build/capture screencast -o /tmp/wp02/out/t1.mp4 -d 2 \
      --restore-token /tmp/wp02/token.txt     # run twice for the token test
$W /tmp/wp02/build/capture screencast -o /tmp/wp02/out/w.mp4 -d 2 \
      --source-type window                    # § 7

# 5. reproduce the two defects
WP02_XDPW=/usr/libexec/xdg-desktop-portal-wlr ./spikes/wp02-capture/run-stack.sh start
WP02_ACCESS_SHIM=0                            ./spikes/wp02-capture/run-stack.sh start

./spikes/wp02-capture/run-stack.sh stop
```

The CLI builds clean with `-Wall -Wextra`.

---

## 11. What this changes for the plan

1. **The recorder and screenshot features are feasible on Linux through the
   portals.** Nothing in the pipeline needed a private protocol or a privileged
   helper. `PLAN.md` § 6's rows for "Screen/region capture", "System audio /
   mic" and "Encoding" hold.
2. **`PLAN.md` § 6 says "wlroots ✓ (monitor; window depends on backend)" for
   capture. Sharpen it:** on xdpw 0.7.1 window capture is not merely missing, it
   is *mis-served*. The row should say "monitor only; WINDOW requests are
   silently served as MONITOR — check `AvailableSourceTypes` and each stream's
   `source_type`".
3. **Add a row for `impl.portal.Access`.** Several portals the port may want
   (`Screenshot`, `Camera`, `Device`, `Location`) do not exist at all on a bare
   wlroots session. Any feature built on them needs a non-portal fallback there,
   and the feature hub needs to explain the gap.
4. **The headless CI harness (WP-P3) cannot use a stock distro xdpw.** Either
   patch it, give the runner a DRM device, or run the capture smoke tests only
   where one exists. Budget for this.
5. **Frame rate is variable on wlroots.** The recorder's timeline
   (`RecorderTimeline.swift`) must be driven by capture timestamps, not by a
   frame counter times a nominal rate. Worth a unit test with a synthetic VFR
   sequence when that code is ported.
6. **Latency is a non-issue** (1.2 ms typical) and **CPU is the constraint**
   (42 % of one core for 720p30 in software). The energy badge for the recorder
   should reflect encode cost, and hardware encode (VA-API) is worth having as
   soon as the packaging story allows it.
