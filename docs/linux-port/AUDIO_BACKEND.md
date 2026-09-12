# Audio backend: measured behaviour

What `linux/platform/audio` was observed to do, with the commands and their
real output. The API and the design reasoning are in
[`linux/platform/audio/README.md`](../../linux/platform/audio/README.md); this
file is the evidence.

Everything below was run on Ubuntu 24.04, PipeWire 1.0.5, WirePlumber 1.0.5,
libpulse 16.1, in a container with no sound card and no desktop session. The
audio server is the private stack `scripts/run-stack.sh` builds: its own
`dbus-daemon`, `pipewire`, `wireplumber` and `pipewire-pulse`, with two
`support.null-audio-sink` sinks (`vs-sink-a`, `vs-sink-b`) and one null source
(`vs-source-a`) standing in for hardware. The application stream is a `pw-play`
of a generated tone, given the properties a real client supplies.

```
$ scripts/run-stack.sh start
export XDG_RUNTIME_DIR=/tmp/wp-a5/recap/run
export DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/dbus-Jx99HNkCgn,guid=ba506e9a5da3982b2e70ea3d6aa549d1
export XDG_STATE_HOME=/tmp/wp-a5/recap/state
export XDG_CONFIG_HOME=/tmp/wp-a5/recap/config
export XDG_DATA_HOME=/tmp/wp-a5/recap/data

$ wpctl status
Audio
 ├─ Sinks:
 │      39. Vorssaint Test Sink B               [vol: 1.00]
 │  *   44. Vorssaint Test Sink A               [vol: 1.00]
 ├─ Sources:
 │  *   49. Vorssaint Test Source A             [vol: 1.00]
```

## Summary against the work package

| # | Requirement | Status | Evidence |
|---|---|---|---|
| 1 | Registry enumeration, props, defaults, debounced change events | met | [§1](#1-registry-and-events) |
| 2 | Per-stream volume and mute, cubic/linear explicit, 200 % ceiling, read-back | met | [§2](#2-per-stream-volume-and-mute) |
| 3 | Routing via `target.object`, verified by the links, and cleared | met | [§3](#3-routing) |
| 4 | Default sink switching and headphone-disconnect detection | met | [§4](#4-default-output-and-headphone-disconnect) |
| 5 | Mute all inputs and restore, remembering previous states | met | [§5](#5-mute-every-input) |
| 6 | libpulse fallback, detected by connection failure | met | [§6](#6-the-libpulse-fallback) |
| 7 | Capability flags | met | [§7](#7-capability-flags) |

Beyond the seven, the surface was checked field by field against WP-12's
`AudioGraph` protocol once that landed, and four things it needs were missing:
`AudioSink.transport`, `AudioStream.applicationID`, `AudioStream.isActive` and
`setDefaultSource`. All four were added; see [§8](#8-filling-wp-12s-audiograph).

## 1. Registry and events

Sinks, sources and streams are enumerated with their properties. The default
sink comes from the `default.audio.sink` metadata and is marked.

```
$ vs-audio devices
ID       KIND     VOL%   MUTE  DEFAULT TRANSPORT NAME
45       sink     100.0  no    *       -         Vorssaint Test Sink A
51       sink     100.0  no            -         Vorssaint Test Sink B
57       source   100.0  no    *       -         Vorssaint Test Source A

$ vs-audio streams
ID       KIND     VOL%   MUTE  LIVE   TARGET   EFFECTIVE APP                  PID      MEDIA
65       playback 100.0  no    yes    0        45        VsTestPlayer         25115    VsTestTone

$ vs-audio --json streams
[
  {"id":65,"kind":"playback","name":"pw-play","description":"","app_name":"VsTestPlayer","icon_name":"audio-x-generic","media_name":"VsTestTone","app_id":"VsTestPlayer","transport":"","pid":25115,"volume":1.0000,"cubic":1.0000,"percent":100.0,"mute":false,"has_volume":true,"is_default":false,"active":true,"target_id":0,"effective_id":45}
]
```

Every property the mixer needs is there: `application.name`,
`application.process.id`, `application.icon-name` and `media.name`.

**Finding: the registry's `global` event is not enough.** It carries a filtered
subset of a node's properties. Measured on PipeWire 1.0.5, `node.name`,
`application.name` and `application.icon-name` arrive in the registry
dictionary but `application.process.id` and `media.name` do not, so an
enumeration built on it alone lists every row with `pid` = -1 and no "now
playing" line — which is what this backend did before it bound each node and
read the complete dictionary from the node's own `info` event. The probe that
established it:

```
$ pw-dump 108 | (props of the stream node)
{'node.name': 'pw-play', 'application.name': 'VsTestPlayer',
 'application.process.id': 27429, 'application.icon-name': 'audio-x-generic',
 'media.name': 'VsTestTone', 'object.serial': 108}
```

Events are delivered only from `dispatch`, debounced into one `changed` per
burst. `vs-audio watch` prints them as they arrive:

```
$ vs-audio watch
vs-audio: watching on pipewire (event_fd=3); interrupt to stop

$ vs-audio set-default 45
default sink is now 45

$ vs-audio set-volume 65 0.3
set 65 to 30.0% (linear 0.3000, cubic 0.6694)

$ pw-cli destroy vs-sink-a

(what watch printed)
1789217242.420 changed id=0 name=
1789217244.341 default-sink-changed id=45 name=vs-sink-a
1789217244.426 changed id=0 name=
1789217245.432 changed id=0 name=
1789217246.363 default-sink-disconnected id=45 name=vs-sink-a
1789217246.365 default-sink-changed id=51 name=vs-sink-b
1789217246.447 changed id=0 name=
```

The volume write produced **one** `changed`, not one per param reply, which is
the debounce doing its job. The `events` test also asserts that the callback
never runs outside `dispatch` — including during a blocking `set_volume`, which
pumps the connection internally and queues what it sees.

## 2. Per-stream volume and mute

**The scale, made explicit.** The API is linear amplitude with 1.0 at unity,
which is what PipeWire's `channelVolumes` holds and what the macOS
`AppVolumeMixer` means by 100 %. `wpctl` and the desktop sliders are cubic.
Setting 150 % through this API and reading it back with two independent tools:

```
$ vs-audio set-volume 65 1.5
set 65 to 150.0% (linear 1.5000, cubic 1.1447)

$ wpctl get-volume 51
Volume: 1.14

$ pw-dump (Props of the stream node)
channelVolumes: [1.5, 1.5]  mute: False
```

and at half volume:

```
$ vs-audio set-volume 65 0.5
set 65 to 50.0% (linear 0.5000, cubic 0.7937)
channelVolumes: [0.5, 0.5]
```

So: **linear 0.5 = cubic 0.79 = "50 %" in the panel**, and **linear 1.5 = cubic
1.14 = "150 %"**. The relationship was established independently before the
backend was written, with `wpctl` alone:

```
$ wpctl set-volume 51 0.5
$ wpctl get-volume 51
Volume: 0.50
$ pw-dump (Props)
channelVolumes: [0.125, 0.125]        # 0.5 cubed
```

**150 % is not clamped.** PipeWire accepted `channelVolumes: [1.5, 1.5]` and
reported it back unchanged, which is the whole reason the mixer can offer a
boost without the aggregate-device machinery the macOS version needs.

**The mapping to the macOS mixer.** `MixerApp.volume` on macOS runs 0…2 with
1.0 as untouched passthrough, applied as a linear gain. That is the same
quantity as `vs_audio_node.volume`, so a saved `MixerApp.volume` transfers
without conversion. The ceiling matches too: `VS_AUDIO_MAX_VOLUME` is 200 %,
inherited from the macOS mixer's `AppVolumeMixer.maxVolume` rather than chosen
here, precisely so a settings file moved between the platforms keeps its
numbers. A lower Linux ceiling would have silently turned a saved 200 % row
into 150 % on import, losing the user's setting without saying so. PipeWire
imposes no ceiling of its own. 150 %
(`VS_AUDIO_CLIPPING_HAZARD_VOLUME`) is where WirePlumber's own tools stop and
is the point above which the panel owns warning about clipping; a request
above the ceiling is clamped rather than refused, so a slider dragged to the
end works.

Mute is independent of volume, so unmuting restores the level:

```
set-volume 0.7 → set-mute on  → volume 0.7, muted
              → set-mute off → volume 0.7, not muted
```

Read-back is not a formality: `pw_node_set_param` is asynchronous and returns
before the server has seen the pod, so `set_volume` waits for the value to come
back on the `Props` param and answers `VS_ERR_NOT_APPLIED` if a different one
does.

## 3. Routing

A per-stream route is the `target.object` metadata key, set to the sink's
`object.serial`. The call returns only when the stream's `Link` objects have
actually moved.

```
$ vs-audio route 65 51                      # 51 is sink B
stream 65 routed to sink 51
ID       KIND     VOL%   MUTE  LIVE   TARGET   EFFECTIVE APP                  PID      MEDIA
65       playback 100.0  no    yes    51       51        VsTestPlayer         25115    VsTestTone

$ pw-dump Link
link 55 node 51 -> node 39                  # node 39 is sink B
link 54 node 51 -> node 39

$ vs-audio route 65 0                       # clear the pin
stream 65 follows the default
ID       KIND     VOL%   MUTE  LIVE   TARGET   EFFECTIVE APP                  PID      MEDIA
65       playback 100.0  no    yes    0        45        VsTestPlayer         25115    VsTestTone

$ pw-dump Link
link 54 node 51 -> node 44                  # back on sink A, the default
link 55 node 51 -> node 44
```

`EFFECTIVE` is read from the `Link` objects, not from the request, so the two
columns agreeing is a real observation and not a tautology.

**Finding: the route must be written against `object.serial`, never the global
id.** PipeWire recycles global ids. During development a `target.object` pin
written against global id 51 was still in the metadata when a *later,
unrelated* `pw-play` was assigned the same global id, and that new stream was
silently captured by the old pin — it started on sink B with no one having
asked. Serials are monotonic and do not do this, which is also why the public
`vs_audio_id` is the serial.

## 4. Default output and headphone disconnect

Both metadata keys are written, so the choice is in effect now and restored at
the next login:

```
$ vs-audio set-default 51
default sink is now 51
ID       KIND     VOL%   MUTE  DEFAULT TRANSPORT NAME
45       sink     100.0  no            -         Vorssaint Test Sink A
51       sink     100.0  no    *       -         Vorssaint Test Sink B
57       source   100.0  no    *       -         Vorssaint Test Source A

$ pw-metadata -n default | grep audio.sink
update: id:0 key:'default.audio.sink' value:'{"name":"vs-sink-b"}' type:'Spa:String:JSON'
update: id:0 key:'default.configured.audio.sink' value:'{"name":"vs-sink-b"}' type:'Spa:String:JSON'

$ vs-audio set-default-source 57
default source is now 57
```

Headphone disconnect is a sink global disappearing while it is the default.
Destroying the default sink is the same shape as a USB DAC being pulled or a
Bluetooth headset dropping:

```
$ pw-cli destroy vs-sink-a          # while vs-sink-a is the default

(watch)
1789217246.363 default-sink-disconnected id=45 name=vs-sink-a
1789217246.365 default-sink-changed id=51 name=vs-sink-b

$ vs-audio devices
ID       KIND     VOL%   MUTE  DEFAULT TRANSPORT NAME
51       sink     100.0  no    *       -         Vorssaint Test Sink B
57       source   100.0  no    *       -         Vorssaint Test Source A
```

The event carries the id **and the name** of the sink that left, because by the
time the app re-lists there is nothing left to name — and it arrives before
WirePlumber's own fallback to the next sink, which is the ordering the macOS
`loweringOutputVolumeIfHeadphonesDisconnected` path needs: it has to know both
which device went away and which one the system fell back to.

## 5. Mute every input

```
$ vs-audio mute-inputs on
inputs muted (1 changed)
57       source   100.0  yes   *       -         Vorssaint Test Source A

$ cat $XDG_RUNTIME_DIR/vorssaint-audio-micmute
0 vs-source-a

$ vs-audio mute-inputs off          # a separate process
inputs restored (1 changed)
57       source   100.0  no    *       -         Vorssaint Test Source A
```

The `0` in the record means "this source was not muted before", so restoring
unmutes it. A source recorded as `1` is left alone, which is the promise: a
microphone the user muted themselves stays muted. The `mute_inputs` test covers
that case, and the case where a second backend instance — the app restarting,
or the CLI, which is a new process every invocation — undoes what the first
one did.

**Finding: the record cannot live in the handle.** The first implementation
kept it in memory, which made `vs-audio mute-inputs off` a no-op: the CLI is a
new process and had no record. Keeping it in `$XDG_RUNTIME_DIR`, keyed by
`node.name`, fixes that and also means a crash with the mic muted is
recoverable rather than leaving a silently dead microphone.

Monitor sources are excluded from the listing, so muting every input does not
silence system-audio capture.

## 6. The libpulse fallback

Run here against `pipewire-pulse`, which speaks the same protocol a real
PulseAudio server does; what is being exercised is this backend's use of the
libpulse API.

```
$ vs-audio --backend libpulse devices
ID       KIND     VOL%   MUTE  DEFAULT TRANSPORT NAME
16777261 sink     100.0  no            -         Vorssaint Test Sink A
16777267 sink     100.0  no    *       -         Vorssaint Test Sink B
33554489 source   100.0  no    *       -         Vorssaint Test Source A

$ vs-audio --backend libpulse streams
ID       KIND     VOL%   MUTE  LIVE   TARGET   EFFECTIVE APP                  PID      MEDIA
50331713 playback 100.0  no    yes    16777267 16777267  VsTestPlayer         25115    VsTestTone

$ vs-audio --backend libpulse set-volume 50331713 1.5
set 50331713 to 150.0% (linear 1.5000, cubic 1.1447)

$ vs-audio streams                  # the same stream through PipeWire
ID       KIND     VOL%   MUTE  LIVE   TARGET   EFFECTIVE APP                  PID      MEDIA
65       playback 150.0  no    yes    0        51        VsTestPlayer         25115    VsTestTone

$ vs-audio --backend libpulse set-default 16777267
default sink is now 16777267
```

`pa_context_get_sink_input_info_list`, `pa_context_set_sink_input_volume` and
`pa_context_set_default_sink` are the calls behind those lines, as the work
package asked. A 150 % set through libpulse is visible through PipeWire as a
linear 150 %, which is the cross-check that `pa_sw_volume_from_linear` is being
used correctly and the two backends agree on what a percentage means.

The ids carry the object kind in their top byte because PulseAudio numbers
sinks, sources, sink-inputs and source-outputs in four separate index spaces.

**Detection** is the connection failing, not a probe: `pw_context_connect`
returning NULL is the whole test, because anything less can be wrong — a socket
can exist with nothing behind it. The `capabilities` test exercises the real
path by pointing `PIPEWIRE_REMOTE` at a name no daemon answers to:

```
PIPEWIRE_REMOTE=vs-no-such-pipewire:
  vs_audio_system_create("pipewire") -> NULL, VS_ERR_NO_BACKEND
  vs_audio_system_create(NULL)       -> "libpulse", VS_AUDIO_HAS_PULSE_FALLBACK
```

**Finding: the default-sink read-back needs a wait.**
`pa_context_set_default_sink` completes as soon as the server accepts the
change, and under `pipewire-pulse` the change then has to travel on to
WirePlumber's metadata and back. One immediate `pa_context_get_server_info`
after a successful set still reported the *old* sink, so a write that had
worked was being reported as a failure. The fallback now polls the server info
until it agrees or the budget runs out.

## 7. Capability flags

```
$ vs-audio caps
backend: pipewire
has_pipewire           yes
has_pulse_fallback     no
can_route_per_stream   yes
can_boost_over_100     yes
has_events             yes
max_volume             150%

$ vs-audio --backend libpulse caps
backend: libpulse
has_pipewire           no
has_pulse_fallback     yes
can_route_per_stream   yes
can_boost_over_100     yes
has_events             yes
max_volume             150%
```

The two backend bits are mutually exclusive, which the `capabilities` test
asserts, so a caller can always tell which backend it got. The fallback keeps
`can_route_per_stream` because `pa_context_move_sink_input_by_index` is a real
move; what it loses is the independent read-back, and it reports `event_fd` as
-1 because `pa_mainloop` exposes no single descriptor to wait on.

## Tests

```
$ ctest -R '^audio_' --output-on-failure
    Start  7: audio_support
1/9 Test  #7: audio_support ....................   Passed    0.00 sec
2/9 Test  #8: audio_registry ...................   Passed    3.58 sec
3/9 Test  #9: audio_events .....................   Passed    3.23 sec
4/9 Test #10: audio_volume .....................   Passed    3.06 sec
5/9 Test #11: audio_routing ....................   Passed    3.11 sec
6/9 Test #12: audio_default_sink ...............   Passed    9.38 sec
7/9 Test #13: audio_mute_inputs ................   Passed    3.17 sec
8/9 Test #14: audio_pulse_fallback .............   Passed    3.19 sec
9/9 Test #15: audio_capabilities ...............   Passed    2.94 sec

100% tests passed, 0 tests failed out of 9
```

`audio_support` needs no audio server and always runs. The eight live suites
exit 77 — ctest's "not run" — when `pipewire`, `wireplumber`, `dbus-daemon` or
`ffmpeg` is missing, so a machine without them reports them as skipped rather
than passed.

All five legs of the matrix pass — the four CMake build types clean under
`-Werror`, plus AddressSanitizer/UndefinedBehaviorSanitizer/LeakSanitizer:

```
$ CTEST_ARGS="-R ^audio_" linux/platform/scripts/build-matrix.sh
===== default =====
  -- No CMAKE_BUILD_TYPE given, defaulting to RelWithDebInfo
  CMAKE_BUILD_TYPE = RelWithDebInfo
  build: clean, no compiler warnings
  ctest: 100% tests passed, 0 tests failed out of 9

===== Debug =====
  CMAKE_BUILD_TYPE = Debug
  build: clean, no compiler warnings
  ctest: 100% tests passed, 0 tests failed out of 9

===== Release =====
  CMAKE_BUILD_TYPE = Release
  build: clean, no compiler warnings
  ctest: 100% tests passed, 0 tests failed out of 9

===== RelWithDebInfo =====
  CMAKE_BUILD_TYPE = RelWithDebInfo
  build: clean, no compiler warnings
  ctest: 100% tests passed, 0 tests failed out of 9

===== Sanitizers =====
  CMAKE_BUILD_TYPE = Debug
  sanitizers: address, undefined, leak
  build: clean, no compiler warnings
  ctest: 100% tests passed, 0 tests failed out of 9

===== summary =====
all five legs pass: four build types clean under -Werror, plus
ASan/UBSan/LSan with no sanitizer reports
```

LeakSanitizer was confirmed armed rather than assumed, because a leg that
silently ran without it would be a fifth clean build wearing a new name:

```
$ cat leakcheck.c
#include <stdlib.h>
int main(void){ volatile void*p = malloc(64); (void)p; return 0; }
$ gcc -fsanitize=address,undefined -g -o leakcheck leakcheck.c && ./leakcheck
==20313==ERROR: LeakSanitizer: detected memory leaks
Direct leak of 64 byte(s) in 1 object(s) allocated from:
    #1 0x55fd179d519e in main /tmp/wp-a5/leakcheck.c:2
SUMMARY: AddressSanitizer: 64 byte(s) leaked in 1 allocation(s).
exit=1
```

The ctest suites exercise the library. The **CLI exit paths** were swept
separately, because that is the shape of the defect the rule was written for
(WP-D2 shipped a leaking CLI exit that four clean builds missed) and because an
error exit is the path least likely to be covered by a test that asserts on
success:

```
clean rc=0  caps                 clean rc=0  pulse caps
clean rc=0  caps --json          clean rc=0  pulse devices
clean rc=0  devices              clean rc=0  pulse streams
clean rc=0  devices --json       clean rc=0  pulse set-volume
clean rc=0  streams              clean rc=0  pulse set-default
clean rc=0  streams --json       clean rc=0  pulse route
clean rc=0  set-volume 1.5       clean rc=1  unknown id
clean rc=0  set-volume 1.0       clean rc=2  bad args
clean rc=0  set-mute on          clean rc=2  unknown command
clean rc=0  set-mute off         clean rc=1  unknown backend
clean rc=0  route -> B           clean rc=0  help
clean rc=0  route clear          clean     watch (interrupted with SIGINT)
clean rc=0  set-default B
clean rc=0  set-default A
clean rc=0  mute-inputs on
clean rc=0  mute-inputs off

ALL CLI PATHS CLEAN UNDER ASan/UBSan/LSan
```

Every one of those ran the sanitized `vs-audio` against the live stack; "clean"
means the run produced no `AddressSanitizer`, `LeakSanitizer` or
`runtime error` line, and the non-zero `rc` values are the intended exit codes
for the error paths, not crashes. The matrix also greps each ctest log for
sanitizer reports independently of the exit status, so a leak in a process that
still exited 0 fails the leg rather than passing unnoticed.

## 8. Filling WP-12's `AudioGraph`

`Sources/VorssaintCore/Platform/AudioGraph.swift` landed while this backend was
being written, and comparing the two found four gaps. They were real: without
them the Swift wrapper could not have been written without adding behaviour of
its own, which `linux/platform/README.md` forbids.

```
$ vs-audio devices
ID       KIND     VOL%   MUTE  DEFAULT TRANSPORT NAME
45       sink     100.0  no    *       -         Vorssaint Test Sink A
51       sink     100.0  no            -         Vorssaint Test Sink B
57       source   100.0  no    *       -         Vorssaint Test Source A

$ vs-audio streams
ID       KIND     VOL%   MUTE  LIVE   TARGET   EFFECTIVE APP                  PID      MEDIA
65       playback 100.0  no    yes    0        45        VsTestPlayer         25115    VsTestTone
```

- **`transport`** (`AudioSink.transport`, the bus the icon is drawn from) comes
  from `device.bus` where PipeWire copies it onto the node, then from the BlueZ
  API keys, then from the node-name prefix WirePlumber builds out of the ALSA
  or BlueZ path. A null sink sits on no bus at all, so `-` above is the correct
  answer and anything else would mean the heuristic had invented something. The
  values that matter — bluetooth, usb, hdmi — **could not be produced in this
  container**; see "What was not measured here".
- **`app_id`** (`AudioStream.applicationID`, the key a saved volume persists
  against) is `application.id`, then `application.process.binary`, then
  `application.name`. **Finding:** `pw-play` sets neither of the first two, and
  the first implementation left `app_id` empty for it. The display-name
  fallback is not a patch over that — it is the rule
  `MixerRoutingSupport.rowIdentity` already applies on macOS to a process with
  no bundle id, and for the same reason: a game or a bare executable is still
  worth remembering a volume for. Hence `"app_id":"VsTestPlayer"` above.
- **`VS_AUDIO_NODE_ACTIVE`** (`AudioStream.isActive`, the live indicator and
  the reason silent rows sort down) is PipeWire's node state being `Running`
  rather than `Idle`, and not being `corked` on the libpulse side.
- **`set_default_source`** is the input half of the default switching, which
  `AudioInputDeviceManager`'s preferred-input setting writes. It takes the same
  two metadata keys as the sink and reads back the same way.

Two mapping decisions the Swift wrapper must follow, recorded here because
getting either wrong is silent:

- `AudioGraph`'s ids are `String` and these are `uint32_t`, so the wrapper
  stringifies the id. It must **not** use `node.name` instead: every `pw-play`
  is called "pw-play", and two of them would collapse into one row.
- `AudioStream.sinkID` maps to `effective_id`, **not** `target_id`. The
  protocol asks where the audio is; `target_id` is only where it was asked to
  go, and the two differ for exactly as long as a route is in flight or has
  failed.

## What was not measured here

- **Real hardware.** Every sink and source in this environment is a
  `support.null-audio-sink`. Clamping behaviour at 150 % on a device with a
  hardware volume control, and the exact `node.description` a real card
  reports, are unverified; the backend's read-back is what turns a clamping
  device into `VS_ERR_NOT_APPLIED` rather than a silent wrong value, but no
  such device was available to trigger it.
- **`transport` beyond the empty case.** The null sinks sit on no bus, so the
  only value observed was `""`. The `device.bus` branch, the BlueZ branch and
  the three node-name prefixes (`bluez_`, `.usb-`, `hdmi`) are reasoned from
  PipeWire's key documentation and WirePlumber's naming, **not measured**. This
  is the weakest claim in the package: it decides only which icon the mixer
  draws, and an unrecognised device falls back to `""` and a generic icon
  rather than a wrong one, but a reviewer with real hardware should check it.
- **A real PulseAudio server.** The fallback was tested against
  `pipewire-pulse`. The API calls are the same, but a genuine `pulseaudio`
  daemon has not been exercised.
- **Bluetooth.** A Bluetooth sink disappearing produces the same event as the
  `pw-cli destroy` used here, since both are a node global going away, but no
  BlueZ device was present to confirm the timing.
- **Desktop matrix.** This backend talks to PipeWire, not to a compositor, so
  GNOME / KDE / wlroots / X11 make no difference to it; that claim is reasoned,
  not measured, and the `FEATURE_TRIAGE.md` rows say ✓ on that basis.
