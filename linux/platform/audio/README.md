# `linux/platform/audio`: the mixer, output switching and mic mute backend

The Linux half of three macOS services:

| macOS service | What it needs | Here |
|---|---|---|
| `AppVolumeMixer` | list output devices, list app streams, set a per-app volume above 100 %, route one app to one output | `list`, `set_volume`, `route_stream` |
| `SoundOutputSwitcher` | switch the system output, notice headphones leaving | `set_default_sink`, `VS_AUDIO_EVENT_DEFAULT_SINK_DISCONNECTED` |
| `AudioInputDeviceManager` | list inputs, know the default, set a preferred one | `list` with `VS_AUDIO_NODE_SOURCE`, `set_default_source` |
| `MicMuteService` | mute every input, restore exactly what it muted | `mute_all_inputs` |

Two backends fill the same `vs_audio_system` vtable: PipeWire over
`libpipewire`, and a `libpulse` fallback for hosts that still run PulseAudio.
Which one answered is a capability bit, not a build option.

```
linux/platform/audio/
  vs_audio.c              backend selection, the volume scale, the event queue,
                          and the mute-every-input bookkeeping both backends share
  backend_pipewire.c      registry, Props, metadata, links
  backend_pulse.c         sink/source/sink-input info and the write verbs
  tools/vs_audio_cli.c    the `vs-audio` harness
  scripts/run-stack.sh    a private headless PipeWire stack for the tests
  scripts/test-stream.sh  an application stream to act on inside that stack
  tests/                  one pure-C suite and eight live ones
```

Measured behaviour, with the commands and their output, is in
[`docs/linux-port/AUDIO_BACKEND.md`](../../../docs/linux-port/AUDIO_BACKEND.md).

## Percentages: what "100 %" means

**Every volume in this API is linear amplitude with 1.0 at unity.** That is
exactly the macOS mixer's convention — `AppVolumeMixer` runs 0…2 with 1.0
meaning untouched passthrough — and it is what PipeWire's `Props`
`channelVolumes` holds. A percentage `P` from the panel is `P/100` here, and
nothing else needs to happen on the way.

It is **not** the number `wpctl`, GNOME's sound settings or KDE's mixer show.
Those apply a cubic curve:

| Panel says | This API (`volume`) | `wpctl get-volume` | `channelVolumes` |
|---|---|---|---|
| 0 % | 0.0 | 0.00 | 0.0 |
| 50 % | 0.5 | 0.79 | 0.5 |
| 100 % | 1.0 | 1.00 | 1.0 |
| 150 % | 1.5 | 1.14 | 1.5 |

`wpctl set-volume ID 0.5` writes a linear `0.125`, because 0.5³ = 0.125. Both
numbers describe the same loudness; they are different scales for it. The
conversion lives in `vs_audio_linear_to_cubic` / `vs_audio_cubic_to_linear` and
nowhere else, so a UI that wants to show the same figure a desktop mixer shows
calls one function rather than re-deriving the curve.

The libpulse fallback has the same split internally: `pa_volume_t` is cubic
with `PA_VOLUME_NORM` at unity, and `pa_sw_volume_from_linear` /
`pa_sw_volume_to_linear` are what keep the promise that this API is linear. A
150 % set through the fallback is observable as `channelVolumes: [1.5, 1.5]`
through PipeWire, which is how the two backends were checked against each
other.

The ceiling here is **150 %** (`VS_AUDIO_MAX_VOLUME`), not the 200 % the macOS
mixer allows, because 150 % is where WirePlumber's own tools stop and going
further is a clipping hazard the panel would have to explain. A request above
it is clamped rather than refused, so a slider dragged to the end works.

## How it works

### Enumeration

Every `Audio/Sink`, `Audio/Source`, `Stream/Output/Audio` and
`Stream/Input/Audio` global is **bound**, not merely noticed. The registry's
`global` event carries a filtered subset of a node's properties — measured on
PipeWire 1.0.5, `node.name`, `application.name` and `application.icon-name` are
there, but `application.process.id` and `media.name` are not — so a mixer built
on the registry dict alone lists every row with no pid and no "now playing"
line. The full dictionary is on the node's own `info` event.

`Props` is *subscribed*, not read once: the mixer has to see a volume the user
changed in another application's mixer too.

The identifier handed out is `object.serial`, not the global id. PipeWire
recycles global ids, and a saved route written against one eventually captures
an unrelated later node — which happened during development, when a stale
`target.object` pin caught a freshly started stream that had inherited the same
global id.

### Routing

A per-stream route is the `target.object` key on the `default` metadata object,
keyed by the stream's global id, valued with the sink's `object.serial`.
Clearing the key puts the stream back under the default.

`route_stream` does not return when the metadata write is sent. It returns when
the stream's `Link` objects have actually moved onto the requested sink, and
`VS_ERR_NOT_APPLIED` if they never do. The check is "every one of this stream's
links is on that sink", not "one of them is": while WirePlumber moves a stream
there is a moment with links to both, and accepting that would report the move
before it happened.

### Default output and headphone disconnect

`set_default_sink` writes both `default.audio.sink` (what is in effect now) and
`default.configured.audio.sink` (the choice WirePlumber restores at the next
login), then waits for the metadata to come back changed. Writing only the
configured key leaves the session on the old sink; writing only the effective
one is forgotten at logout.

Headphones leaving is a sink global disappearing *while it is the default*. The
event carries the id and name of the sink that left, because by the time the UI
re-lists there is nothing to name — which is what the macOS
`loweringOutputVolumeIfHeadphonesDisconnected` path needs to decide whether to
lower the speakers it just fell back to.

### Mute every input

Muting records each source's prior state; unmuting restores **only** the
sources this feature muted. A source the user muted themselves while the mic
was off stays muted, because unmuting it would open a microphone the user
closed — the rule `MicMuteSupport.restoreTargets` enforces on macOS.

The record is a file under `$XDG_RUNTIME_DIR`, not a field on the handle, for
two reasons. It is keyed by `node.name`, the one identifier that means the same
thing to both backends and survives a restart (PipeWire serials and PulseAudio
indices are per-session). And it has to outlive the process: a crash with the
mic muted would otherwise leave a microphone silently off with nothing left
that knows to turn it back on. `XDG_RUNTIME_DIR` is cleared at logout, which is
also when a stale record stops meaning anything.

Monitor sources are excluded from the source listing. A sink's monitor is
system-audio capture, not a microphone, and muting it would silence the screen
recorder rather than the user's voice.

### Events and threading

The backend starts no threads. It owns a `pw_loop` (or a `pa_mainloop`) that
the caller drives: `dispatch` turns it once without blocking and delivers
whatever is queued, and the blocking verbs turn it themselves until the server
has confirmed what they asked for.

That is why there is an event queue. A `set_volume` has to process everything
else the server sent while it waited, and those events cannot be handed to the
caller's callback from inside a write — so they are queued and delivered by the
next `dispatch`. The queue is bounded at 256; past that the detail is dropped
and the next drain delivers one `VS_AUDIO_EVENT_CHANGED`, which is the right
instruction anyway, since the cure for "you missed some events" is "re-read the
graph".

`VS_AUDIO_EVENT_CHANGED` is debounced (80 ms, on a timer inside the backend's
own loop, so a poll on `event_fd` still wakes for it). A device appearing
brings a dozen registry and param changes and the mixer wants one refresh.

`event_fd` is a real pollable descriptor on PipeWire. On the libpulse fallback
it is -1: `pa_mainloop` polls a set of descriptors it does not expose, and
inventing a timerfd would be readable on a schedule rather than on an event,
which is not what the member promises. `VS_AUDIO_HAS_EVENTS` is still set —
events do arrive, there is just nothing to wait on, so the caller calls
`dispatch` on a timer.

## Capability flags

| Flag | PipeWire | libpulse | Meaning |
|---|---|---|---|
| `VS_AUDIO_HAS_PIPEWIRE` | ✓ | – | A pipewire daemon answered |
| `VS_AUDIO_HAS_PULSE_FALLBACK` | – | ✓ | The libpulse path is in use |
| `VS_AUDIO_CAN_ROUTE_PER_STREAM` | ✓ | ✓ | One app can be sent to one output |
| `VS_AUDIO_CAN_BOOST_OVER_100` | ✓ | ✓ | Volumes above unity are honoured |
| `VS_AUDIO_HAS_EVENTS` | ✓ | ✓ | Changes are reported through `dispatch` |

The two backend bits are mutually exclusive, so a caller can always tell which
one it got. The fallback keeps `CAN_ROUTE_PER_STREAM` because
`pa_context_move_sink_input_by_index` is a real move — what it loses is the
independent read-back: with no graph to observe, `effective_id` is the sink the
input says it is on rather than where the audio was seen to arrive.

## How WP-12's `AudioGraph` protocol mirrors it

`Sources/VorssaintCore/Platform/AudioGraph.swift` (WP-12) declares the same
shape in Swift; `Sources/VorssaintLinux` implements it as a thin wrapper over
this table and nothing else. The mapping is mechanical, and follows the rules
in [`../README.md`](../README.md):

| C | Swift |
|---|---|
| `vs_audio_system` | `protocol AudioGraph` |
| `capabilities` (`vs_audio_capability`) | `var capabilities: AudioGraphCapabilities` (`OptionSet`) |
| `vs_audio_node` | `struct AudioNode` |
| `vs_audio_node_kind` | `enum AudioNode.Kind` |
| `vs_audio_node_flag` | `AudioNode.Flags` (`OptionSet`) |
| `vs_audio_id` | `AudioNode.ID` (a `RawRepresentable` `UInt32`) |
| `vs_result` | `enum AudioGraphError: Error`, `VS_OK` a normal return |
| `vs_audio_event` + callback | an `AsyncStream<AudioEvent>` or a Combine publisher |
| `list` + `free_list` | one call returning `[AudioNode]` |
| `vs_audio_linear_to_cubic` | `AudioVolume.cubic` |

Field by field against the protocol as it landed:

| `vs_audio_node` | `AudioSink` / `AudioSource` | `AudioStream` |
|---|---|---|
| `id` | `id` (stringified) | `id` (stringified) |
| `description`, falling back to `name` | `name` | – |
| `transport` | `transport` | – |
| `VS_AUDIO_NODE_IS_DEFAULT` | `isDefault` | – |
| `volume`, `VS_AUDIO_NODE_MUTED` | `volume`, `isMuted` | `volume`, `isMuted` |
| `app_id` | – | `applicationID` |
| `app_name` | – | `applicationName` |
| `effective_id` (stringified) | – | `sinkID` |
| `VS_AUDIO_NODE_ACTIVE` | – | `isActive` |

Two notes on that table. The ids are `uint32_t` here and `String` there, so the
wrapper stringifies; it must not substitute `node.name`, which is not unique
(every `pw-play` is called "pw-play"). And `AudioStream.sinkID` maps to
`effective_id`, not `target_id`: the protocol asks where the audio *is*, and
`target_id` is only where it was asked to go.

Two rules keep the mirror honest, the same two the window section states:

1. **The C side is the source of truth for names and semantics.** A field
   renamed here is renamed there in the same PR.
2. **The Swift side adds no behaviour.** The read-back waits, the clamping, the
   debounce and the mic-mute bookkeeping all live in C, where they can be
   tested against a real audio server without a Swift toolchain. The Swift
   wrapper marshals and nothing else.

Above it, `AppVolumeMixer`, `SoundOutputSwitcher`, `AudioInputDeviceManager`
and `MicMuteService` keep their macOS shape and never learn which platform
they are on. The macOS adapter implements the same protocol over CoreAudio,
where `MixerApp.volume` is already the linear 0…2 this API speaks.

## The CLI harness

```sh
vs-audio devices                  # sinks and sources, with the default marked
vs-audio streams                  # what is playing, with app, pid and route
vs-audio watch                    # change events until interrupted
vs-audio set-volume <id> 1.5      # 150 %, linear
vs-audio set-mute <id> on|off
vs-audio route <stream> <sink>    # sink 0 clears the pin
vs-audio set-default <sink>
vs-audio mute-inputs on|off
vs-audio caps                     # backend and capability flags
vs-audio --backend libpulse ...   # force the fallback
vs-audio --json ...               # machine-readable
```

## Building and testing

```sh
cmake -S linux/platform -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build -R '^audio_' --output-on-failure
```

Build dependencies on Ubuntu 24.04:

```sh
apt-get install -y cmake pkg-config libpipewire-0.3-dev libpulse-dev
```

Test dependencies: `pipewire`, `wireplumber`, `pipewire-pulse`, `dbus-daemon`
and `ffmpeg` (to generate the test tone). Without them the live suites exit 77
and ctest reports them as "not run" rather than passed; only `audio_support`,
which touches no audio server, always runs.

The live suites do not use the machine's audio. `scripts/run-stack.sh` builds a
private one — its own `dbus-daemon`, `pipewire`, `wireplumber` and
`pipewire-pulse`, with two `support.null-audio-sink` sinks and one null source
standing in for hardware — under its own `XDG_RUNTIME_DIR` **and its own
`XDG_STATE_HOME`**. The state directory is not an afterthought: WirePlumber's
stream-restore module remembers a stream's volume and target and applies them
to the next stream of the same name, so without it a run that set 150 % handed
that 150 % to the next run's fresh stream and the volume test would have passed
before it ran.

Each live case gets its own stack, since the cases mutate global state (the
default sink; `default_sink` destroys a sink outright) and sharing one would
make them order-dependent.

All five legs of the matrix must pass — the four CMake build types clean under
`-Werror`, plus AddressSanitizer/UndefinedBehaviorSanitizer/LeakSanitizer:

```sh
CTEST_ARGS="-R ^audio_" linux/platform/scripts/build-matrix.sh
```

The sanitizer leg is not a formality here. The backend holds proxies and hooks
whose callbacks fire long after the call that armed them, and the ordinary
failure mode would be a listener still attached to a freed `node_entry` — a
use-after-free nothing in a warning set can see. It is also the leg that would
catch a `list` whose array escaped without a matching `free_list`.

To run it alone:

```sh
cmake -S linux/platform -B build-asan -DCMAKE_BUILD_TYPE=Debug -DVS_SANITIZE=ON
cmake --build build-asan -j
ctest --test-dir build-asan -R '^audio_' --output-on-failure
```
