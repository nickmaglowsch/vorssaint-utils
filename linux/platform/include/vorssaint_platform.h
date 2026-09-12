/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * The C contract between the Linux platform backends (linux/platform) and the
 * Swift Platform protocols (Sources/VorssaintCore/Platform, WP-12). One struct
 * of function pointers per concern; Sources/VorssaintLinux wraps this table and
 * nothing else.
 *
 * The window (WP-C1), capture (WP-B1), audio (WP-A5) and sensors (WP-A1..A4)
 * sections exist so far. Clipboard, power, input and session sections are
 * added by their own work packages, each as another `vs_<concern>_system`
 * vtable in this header. Sections are independent: a consumer that needs one
 * links one library (`vs_window`, `vs_capture`, `vs_audio`, `vs_sensors`).
 *
 * Conventions every section follows:
 *   - Every call returns `int`: 0 (VS_OK) or a negative `vs_result`.
 *   - Every vtable carries `capabilities`, a bitmask the Swift side surfaces so
 *     a feature can degrade honestly instead of failing silently.
 *   - Nothing in this header allocates with anything but malloc/free; every
 *     `*_out` array returned by a backend is released by its `free_*` member.
 *   - No call blocks for longer than its documented budget.
 *
 * Threading: every vtable in this header is single-threaded. One instance
 * belongs to one thread; two threads must not call into the same instance, even
 * for two different members, and none of these calls is reentrant. Backends
 * start no threads of their own, so events never arrive out of the blue: a
 * backend only ever calls the event callback from inside that instance's own
 * `dispatch`, on the thread that called it. Separate instances are independent
 * and may be used from different threads. The capture section is the one place
 * a backend owns a thread of its own, because PipeWire hands buffers over on
 * its loop and a frame not taken is a frame lost; it still delivers only from
 * its own `dispatch`, and its banner says exactly what that costs.
 */

#ifndef VORSSAINT_PLATFORM_H
#define VORSSAINT_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ common */

typedef enum vs_result {
    VS_OK = 0,
    /** The running session's backend cannot do this at all (check capabilities
     *  first; this is the belt-and-braces answer). */
    VS_ERR_UNSUPPORTED = -1,
    /** The object id is not (or no longer) known to the backend. */
    VS_ERR_NOT_FOUND = -2,
    /** The compositor, display server or bridge refused or failed. */
    VS_ERR_BACKEND = -3,
    /** The backend answered too slowly; the caller may retry. */
    VS_ERR_TIMEOUT = -4,
    VS_ERR_NO_MEM = -5,
    VS_ERR_INVALID = -6,
    /** No backend could be selected for this session. */
    VS_ERR_NO_BACKEND = -7,
    /** The request was sent and acknowledged, but reading the state back showed
     *  it did not take effect. Compositors, D-Bus bridges and X11 window
     *  managers all report success without effect, so the backends read back
     *  and say so. */
    VS_ERR_NOT_APPLIED = -8,
} vs_result;

/** Human-readable form of a `vs_result`, for logs and the CLI harnesses. */
const char *vs_result_string(int result);

typedef struct vs_rect {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} vs_rect;

/* ------------------------------------------------------------------ window */

/*
 * Mirrors `WindowSystem` in WP-12. Shaped by what the macOS services actually
 * consume:
 *
 *   Sources/Vorssaint/Services/Switcher/SwitcherModels.swift `SwitcherItem`
 *     id, title, appName, pid, windowOwnerPID, isOnScreen, isAppHidden,
 *     isMinimized, isFullscreen, isOnHiddenSpace, frame
 *   Sources/Vorssaint/Services/Switcher/WindowEnumerator.swift
 *     a whole-session snapshot, z-order (CGWindowList is front-to-back), and a
 *     per-display filter (`DisplayScope`)
 *   Sources/Vorssaint/Services/Switcher/WindowActivator.swift
 *     raise, unminimize, minimize, close
 *   Sources/Vorssaint/Services/WindowLayout/WindowLayoutService.swift
 *     set position and size, then read the frame back and compare within a
 *     tolerance (`verified(_:)`, `attempt(_:)`)
 *   Sources/Vorssaint/Services/AutoQuit/AutoQuitService.swift
 *     window created / destroyed events per application
 */

typedef uint64_t vs_window_id;

/** Capabilities of a window backend. A feature reads these and hides or
 *  explains what the running session cannot do; see FEATURE_TRIAGE.md. */
typedef enum vs_window_capability {
    /** `list` returns the session's toplevels. */
    VS_WINDOW_CAN_LIST = 1u << 0,
    /** `activate` raises and focuses a window. */
    VS_WINDOW_CAN_ACTIVATE = 1u << 1,
    /** `close` asks a window to close (never kills the process). */
    VS_WINDOW_CAN_CLOSE = 1u << 2,
    /** `set_minimized` both minimizes and restores. */
    VS_WINDOW_CAN_MINIMIZE = 1u << 3,
    /** `move_resize` places a window, and `geometry` reads it back. */
    VS_WINDOW_CAN_MOVE_RESIZE = 1u << 4,
    /** `set_workspace` switches the active workspace/desktop. */
    VS_WINDOW_CAN_WORKSPACE_SWITCH = 1u << 5,
    /** The backend pushes events; `event_fd` is pollable. Without it the caller
     *  must poll `list` itself. */
    VS_WINDOW_HAS_LIVE_EVENTS = 1u << 6,
    /** The backend can name a per-window capture source for live previews.
     *  Reserved for WP-C3; no backend sets it yet. */
    VS_WINDOW_HAS_PREVIEWS = 1u << 7,
} vs_window_capability;

typedef enum vs_window_flag {
    VS_WINDOW_MINIMIZED = 1u << 0,
    VS_WINDOW_FOCUSED = 1u << 1,
    VS_WINDOW_FULLSCREEN = 1u << 2,
    VS_WINDOW_MAXIMIZED = 1u << 3,
    /** The window is on the workspace that is currently shown. The switcher's
     *  `isOnHiddenSpace` is the negation. */
    VS_WINDOW_ON_CURRENT_WORKSPACE = 1u << 4,
    /** `x`, `y`, `width`, `height` carry a real frame. Foreign-toplevel
     *  compositors report no geometry, so the switcher must not assume one. */
    VS_WINDOW_HAS_GEOMETRY = 1u << 5,
    /** `pid` is real rather than the -1 placeholder. */
    VS_WINDOW_HAS_PID = 1u << 6,
    /** The window is mapped and would be drawn if nothing covered it; the
     *  switcher's `isOnScreen`. */
    VS_WINDOW_ON_SCREEN = 1u << 7,
} vs_window_flag;

#define VS_WINDOW_APP_ID_MAX 128
#define VS_WINDOW_APP_NAME_MAX 128
#define VS_WINDOW_TITLE_MAX 512
#define VS_WINDOW_OUTPUT_MAX 64

typedef struct vs_window_info {
    /** Stable for the lifetime of the window within one backend instance.
     *  Opaque: X11 uses the XID, wlroots a handle serial, Hyprland the window
     *  address, the bridges whatever the compositor calls a window id. */
    vs_window_id id;
    /** Wayland `app_id`, X11 `WM_CLASS` instance, or the bridge's equivalent.
     *  Stable identity for per-app rules (`SwitcherAppRule`). */
    char app_id[VS_WINDOW_APP_ID_MAX];
    /** Human-readable application name (`SwitcherItem.appName`): X11 `WM_CLASS`
     *  class, otherwise the same as `app_id`. */
    char app_name[VS_WINDOW_APP_NAME_MAX];
    /** `SwitcherItem.title`. May be empty; the switcher falls back to the app
     *  name itself. */
    char title[VS_WINDOW_TITLE_MAX];
    /** Owning process, or -1 when the backend cannot say. autoQuit needs this
     *  to signal the application. */
    int32_t pid;
    /** Frame in layout coordinates, top-left origin, only when
     *  `VS_WINDOW_HAS_GEOMETRY` is set. Directly comparable with output
     *  geometry, like the macOS side's window-server coordinates. */
    vs_rect frame;
    /** Bitmask of `vs_window_flag`. */
    uint32_t flags;
    /** Workspace / virtual desktop index, or -1 when unknown. */
    int32_t workspace;
    /** Output (monitor) name the window is on, or "" when unknown. */
    char output[VS_WINDOW_OUTPUT_MAX];
    /** Stacking order hint, 0 = bottom-most. `list` returns windows in this
     *  order, so a caller that only needs "front to back" can read the array
     *  backwards. Every backend that has no real stacking information reports
     *  its own natural order and sets `stacking_valid` to false. */
    uint32_t stacking_index;
    bool stacking_valid;
} vs_window_info;

typedef enum vs_window_event_type {
    VS_WINDOW_EVENT_ADDED = 1,
    VS_WINDOW_EVENT_REMOVED,
    /** Title, state or geometry changed. */
    VS_WINDOW_EVENT_CHANGED,
    VS_WINDOW_EVENT_ACTIVATED,
    VS_WINDOW_EVENT_WORKSPACE_CHANGED,
    /** The backend lost the channel its control verbs rode on. `capabilities`
     *  has already shrunk to what still works — often listing alone — so the
     *  caller re-reads it, and recreates the backend when it wants the rest
     *  back. */
    VS_WINDOW_EVENT_BACKEND_LOST,
} vs_window_event_type;

typedef struct vs_window_event {
    vs_window_event_type type;
    /** The window the event is about; 0 for workspace and backend events. */
    vs_window_id id;
    /** Snapshot of the window at event time, or NULL for REMOVED and for
     *  backends that cannot describe the window in the event. Valid only for
     *  the duration of the callback. */
    const vs_window_info *info;
    /** Current workspace for WORKSPACE_CHANGED, otherwise -1. */
    int32_t workspace;
} vs_window_event;

typedef void (*vs_window_event_cb)(const vs_window_event *event, void *user_data);

typedef struct vs_window_system vs_window_system;

struct vs_window_system {
    /** Backend identity, for logs and the capabilities page: "x11", "wlr",
     *  "hyprland", "kwin", "gnome". */
    const char *name;
    /** Bitmask of `vs_window_capability`. A member whose capability bit is
     *  clear still exists and returns VS_ERR_UNSUPPORTED.
     *
     *  Capabilities only ever shrink, and only when the channel that carried
     *  them goes away — a compositor withdrawing a global, a bridge leaving the
     *  bus. That is reported as VS_WINDOW_EVENT_BACKEND_LOST, so a caller that
     *  cached this field re-reads it on that event; nothing else changes it. */
    uint32_t capabilities;
    /** Backend-private state. */
    void *impl;

    /** Snapshot of every toplevel, bottom-most first. The caller owns the array
     *  until it passes it to `free_list`. Budget: 100 ms. May run the backend's
     *  connection, so queued events can be delivered by a later `dispatch`
     *  rather than by this call. */
    int (*list)(vs_window_system *self, vs_window_info **windows_out, size_t *count_out);
    void (*free_list)(vs_window_system *self, vs_window_info *windows, size_t count);

    /** Raise and focus, restoring it first when minimized. */
    int (*activate)(vs_window_system *self, vs_window_id id);
    /** Ask the window to close, so unsaved-changes dialogs still appear. */
    int (*close)(vs_window_system *self, vs_window_id id);
    int (*set_minimized)(vs_window_system *self, vs_window_id id, bool minimized);

    /** Place the window. The backend reads the frame back and returns
     *  VS_ERR_NOT_APPLIED when the window landed outside `tolerance` pixels of
     *  the request, which is how WindowLayoutService decides success. Pass a
     *  tolerance of 0 to skip the read-back. */
    int (*move_resize)(vs_window_system *self, vs_window_id id, vs_rect frame, int32_t tolerance);
    /** Current frame, for the caller's own read-back and for layout history. */
    int (*geometry)(vs_window_system *self, vs_window_id id, vs_rect *frame_out);

    int (*current_workspace)(vs_window_system *self, int32_t *workspace_out);
    int (*set_workspace)(vs_window_system *self, int32_t workspace);

    /** Install the event sink. Pass NULL to remove it. The callback runs inside
     *  `dispatch`, on the calling thread, and may call back into this same
     *  instance only after `dispatch` returns. */
    int (*set_event_callback)(vs_window_system *self, vs_window_event_cb callback, void *user_data);
    /** Pollable descriptor that becomes readable when events are pending, or -1
     *  when the backend has none (`VS_WINDOW_HAS_LIVE_EVENTS` clear). */
    int (*event_fd)(vs_window_system *self);
    /** Drain what is pending and deliver it to the callback. Never blocks. The
     *  only place the callback runs. Returns the number of events delivered, or
     *  a negative `vs_result`. */
    int (*dispatch)(vs_window_system *self);

    void (*destroy)(vs_window_system *self);
};

/** Probe the session and build the best backend for it.
 *
 *  `preferred` forces one backend by name ("x11", "wlr", "hyprland", "kwin",
 *  "gnome") and fails with VS_ERR_NO_BACKEND if it is not usable here; NULL
 *  probes in order: Hyprland (HYPRLAND_INSTANCE_SIGNATURE), KWin
 *  (org.kde.KWin on the session bus), GNOME (org.vorssaint.WindowBridge on the
 *  session bus), wlroots foreign-toplevel (WAYLAND_DISPLAY plus the advertised
 *  global), X11 (DISPLAY plus an EWMH-compliant window manager).
 *
 *  Returns NULL when nothing matched; `*result_out` (optional) says why. */
vs_window_system *vs_window_system_create(const char *preferred, int *result_out);

/** Names of the backends compiled into this build, NULL-terminated. */
const char *const *vs_window_backend_names(void);

/* ----------------------------------------------------------------- capture */

/*
 * Mirrors `ScreenCapturer` in WP-12. Shaped by what the macOS capture services
 * actually ask of ScreenCaptureKit:
 *
 *   Sources/Vorssaint/Services/QuickTools/ScreenshotCaptureEngine.swift
 *     enumerate displays and pickable windows; one full-resolution image of a
 *     display, of a window, or of a pixel rect inside a display
 *   Sources/Vorssaint/Services/QuickTools/ScreenCaptureService.swift
 *     one chooser over screenshot / recorder / screen text / colour, so the
 *     same source list serves all four
 *   Sources/Vorssaint/Services/Recorder/RecorderCaptureEngine.swift
 *     a stream of timestamped frames at a frame-rate cap, system audio and a
 *     microphone alongside it, and a stop that drains
 *   Sources/Vorssaint/Services/Recorder/RecorderSampleTiming.swift
 *     every buffer retimed onto one recording clock, so picture, system sound
 *     and voice stay on a single timeline across a pause
 *
 * Three things differ from ScreenCaptureKit, and the difference is in this API
 * rather than hidden behind it, because a caller that does not know them ships
 * a privacy bug or a drifting file (see docs/linux-port/CAPTURE_ENGINE.md):
 *
 *   - Source selection belongs to the compositor, not to us. The portal shows
 *     its own chooser, and what comes back is not always what was asked for:
 *     xdg-desktop-portal-wlr 0.7.1 answers a WINDOW request with a whole
 *     MONITOR. So the start call reads back what was granted, and
 *     `require_source_type` turns that read-back into a refusal -- the capture
 *     form of the VS_ERR_NOT_APPLIED rule the window section follows.
 *   - The portal has no region source. `region` in the stream config is a crop
 *     the engine performs, and `VS_CAPTURE_HAS_REGION` is therefore always
 *     clear: it reports the portal's capability, not ours.
 *   - Frame delivery is damage-driven on wlroots. `max_fps` is a ceiling, not
 *     a cadence, and a still screen delivers nothing at all. Timelines come
 *     from `pts_ns`, never from a frame counter times a nominal rate.
 *
 * Threading. The engine follows this header's rule: one instance, one thread,
 * events delivered only from `vs_capture_stream_dispatch`, on the thread that
 * calls it, with `vs_capture_stream_event_fd` to poll. A capture stream does
 * own a PipeWire thread internally, because PipeWire hands buffers over on its
 * own loop and a frame not taken is a frame lost; that thread is invisible by
 * default -- it copies into the stream's queue and makes the event fd readable,
 * and nothing of the caller's runs on it. `direct_callbacks` is the one way to
 * opt out, and it exists for one caller: WP-B5's encoder, which wants the
 * mapped buffer with no copy and is willing to run on that thread to get it.
 * Independently of which mode is chosen, every `vs_capture_stream_*` call is
 * safe from any thread and from inside a callback, since a recorder's stop and
 * pause arrive from the UI thread while frames are arriving.
 */

typedef struct vs_capture_engine vs_capture_engine;
typedef struct vs_capture_stream vs_capture_stream;

/** Capabilities of a capture backend. A feature reads these and hides or
 *  explains what the running session cannot do; see FEATURE_TRIAGE.md. Fixed
 *  for the life of the engine: the portal is a bus name that either answers or
 *  does not, so there is no channel to lose halfway through. */
typedef enum vs_capture_capability {
    /** `org.freedesktop.portal.Screenshot` exists on this session. Absent on a
     *  bare wlroots session, which has no `impl.portal.Access` backend; the
     *  engine then serves a screenshot from a one-frame ScreenCast instead. */
    VS_CAPTURE_HAS_SCREENSHOT_PORTAL = 1u << 0,
    /** `AvailableSourceTypes` carries the WINDOW bit. Clear does not only mean
     *  "no window capture": xdg-desktop-portal-wlr 0.7.1 accepts a WINDOW
     *  request and serves a whole MONITOR, so a caller must hide the option
     *  rather than try it. */
    VS_CAPTURE_HAS_WINDOW_SOURCE = 1u << 1,
    /** A region source at the portal level. **Always clear**: no portal
     *  backend offers one. Region capture works regardless, by the engine
     *  cropping frames; this bit exists so the distinction stays visible. */
    VS_CAPTURE_HAS_REGION = 1u << 2,
    /** `SelectSources` accepts `persist_mode` and `Start` returns a token that
     *  restores the same source on a later run. */
    VS_CAPTURE_HAS_RESTORE_TOKEN = 1u << 3,
    /** A PipeWire sink monitor is present, so system audio can be recorded. */
    VS_CAPTURE_HAS_AUDIO_MONITOR = 1u << 4,
    /** A PipeWire audio source (microphone) is present. */
    VS_CAPTURE_HAS_MICROPHONE = 1u << 5,
    /** `AvailableSourceTypes` carries the VIRTUAL bit. */
    VS_CAPTURE_HAS_VIRTUAL_SOURCE = 1u << 6,
    /** `AvailableCursorModes` carries EMBEDDED, so the pointer can be drawn
     *  into the frame. */
    VS_CAPTURE_HAS_CURSOR_EMBEDDED = 1u << 7,
} vs_capture_capability;

/** Portal source-type bits, same numbering as the ScreenCast portal's
 *  `AvailableSourceTypes` so nothing has to be translated. */
typedef enum vs_capture_source_type {
    VS_CAPTURE_SOURCE_MONITOR = 1u << 0,
    VS_CAPTURE_SOURCE_WINDOW = 1u << 1,
    VS_CAPTURE_SOURCE_VIRTUAL = 1u << 2,
} vs_capture_source_type;

/** Portal cursor-mode bits, same numbering as `AvailableCursorModes`. */
typedef enum vs_capture_cursor_mode {
    VS_CAPTURE_CURSOR_HIDDEN = 1u << 0,
    VS_CAPTURE_CURSOR_EMBEDDED = 1u << 1,
    VS_CAPTURE_CURSOR_METADATA = 1u << 2,
} vs_capture_cursor_mode;

#define VS_CAPTURE_NAME_MAX 64
#define VS_CAPTURE_DESC_MAX 160

/** One thing that can be captured. Monitors are enumerated from the session
 *  (the compositor names them); windows only where the session serves a real
 *  window source, which is why a window list can legitimately be empty on a
 *  backend whose `VS_CAPTURE_HAS_WINDOW_SOURCE` is clear. */
typedef struct vs_capture_source {
    /** Stable within one engine instance. Opaque: a wlroots output id, a
     *  compositor's monitor index, or a hash of the connector name. */
    uint64_t id;
    /** Connector or compositor name ("HEADLESS-1", "DP-2"), or "" when the
     *  session will not say. */
    char name[VS_CAPTURE_NAME_MAX];
    /** Human-readable ("Dell U2720Q", an application title), or "". */
    char description[VS_CAPTURE_DESC_MAX];
    /** Exactly one `vs_capture_source_type` bit. */
    uint32_t type;
    /** Layout position and pixel size, valid only when `bounds_valid`. */
    vs_rect bounds;
    bool bounds_valid;
    /** Refresh rate in mHz (60000 == 60 Hz), or -1 when unknown: 0 Hz is not a
     *  refresh rate a caller should ever be handed as if it were one. */
    int32_t refresh_mhz;
    /** Fractional scale, or -1 when unknown. */
    double scale;
} vs_capture_source;

typedef enum vs_capture_pixel_format {
    VS_CAPTURE_PIXEL_UNKNOWN = 0,
    /** 4 bytes per pixel, byte order B,G,R,x -- what xdg-desktop-portal-wlr
     *  negotiates on a software renderer, and what the editor wants. */
    VS_CAPTURE_PIXEL_BGRX,
    VS_CAPTURE_PIXEL_BGRA,
    VS_CAPTURE_PIXEL_RGBX,
    VS_CAPTURE_PIXEL_RGBA,
} vs_capture_pixel_format;

/** Bytes per pixel, or 0 for VS_CAPTURE_PIXEL_UNKNOWN. */
uint32_t vs_capture_pixel_bytes(vs_capture_pixel_format format);
const char *vs_capture_pixel_format_name(vs_capture_pixel_format format);

/** A still image the caller owns and releases with `vs_capture_image_free`.
 *  `data` is `height * stride` bytes; `stride` may exceed `width` times
 *  bytes-per-pixel when it came straight off a capture buffer, so never assume
 *  the two are equal. */
typedef struct vs_capture_image {
    uint8_t *data;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    vs_capture_pixel_format format;
    size_t byte_length;
} vs_capture_image;

void vs_capture_image_free(vs_capture_image *image);
/** Write `image` as an 8-bit RGBA PNG. The encoder is internal (zlib only), so
 *  nothing here depends on an image library. */
int vs_capture_image_write_png(const vs_capture_image *image, const char *path);

/** One delivered frame. Every pointer in it is borrowed: it is valid for the
 *  duration of the callback, or until `vs_capture_stream_release_frame` for the
 *  pull API. `vs_capture_image_from_frame` is how a caller keeps one. */
typedef struct vs_capture_frame {
    const uint8_t *data;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    vs_capture_pixel_format format;
    /** `spa_meta_header.pts` of the buffer: the compositor's capture instant on
     *  CLOCK_MONOTONIC. Valid only when `has_pts`. */
    int64_t pts_ns;
    bool has_pts;
    /** CLOCK_MONOTONIC when the engine took delivery. `arrival_ns - pts_ns` is
     *  the transport latency the WP-02 spike measured at ~1.2 ms. */
    int64_t arrival_ns;
    /** Position on the recording timeline: the capture instant with the stream
     *  start and every pause gap removed, so it is directly usable as a
     *  presentation timestamp and matches the audio timeline. */
    int64_t timeline_ns;
    /** Delivered frames so far, from 0. There are no gaps: frames the engine
     *  dropped are counted in `vs_capture_stats`, not numbered here. */
    uint64_t sequence;
} vs_capture_frame;

/** Copy a frame, or a rectangle of one, into an image the caller owns and can
 *  keep after the callback returns. `region` NULL copies the whole frame; a
 *  region entirely outside it is VS_ERR_INVALID rather than an empty image. The
 *  result is packed (`stride == width * bytes-per-pixel`) even when the source
 *  was padded, and is released with `vs_capture_image_free`. */
int vs_capture_image_from_frame(const uint8_t *data, uint32_t width, uint32_t height,
                                uint32_t stride, vs_capture_pixel_format format,
                                const vs_rect *region, vs_capture_image *image_out);

typedef enum vs_capture_audio_kind {
    VS_CAPTURE_AUDIO_SYSTEM = 1,
    VS_CAPTURE_AUDIO_MICROPHONE = 2,
} vs_capture_audio_kind;

/** One delivered PCM buffer, interleaved 32-bit float, on the same timeline as
 *  the frames, and borrowed on the same terms. WP-B5 feeds this straight to its
 *  encoder; nothing in this layer knows what a codec is. */
typedef struct vs_capture_audio_buffer {
    vs_capture_audio_kind kind;
    const float *samples;
    /** Samples per channel; the buffer holds `frame_count * channels` floats. */
    uint32_t frame_count;
    uint32_t rate;
    uint32_t channels;
    int64_t pts_ns;
    bool has_pts;
    int64_t arrival_ns;
    int64_t timeline_ns;
    uint64_t sequence;
} vs_capture_audio_buffer;

/** What to capture and how. Zeroed and then filled member by member, so adding
 *  a member later never changes an existing caller's meaning; every zero is a
 *  documented default. */
typedef struct vs_capture_stream_config {
    /** Bits of `vs_capture_source_type` to ask for; 0 means MONITOR. What is
     *  granted is read back and may differ -- see `require_source_type`. */
    uint32_t source_types;
    /** Refuse the session when the granted source type is not among the
     *  requested ones, returning VS_ERR_NOT_APPLIED and closing it, rather than
     *  handing back a stream of something else. Every caller that offers
     *  "record this window" sets this. */
    bool require_source_type;
    /** One `vs_capture_cursor_mode` bit; 0 means HIDDEN. */
    uint32_t cursor_mode;
    /** Token from a previous session, or NULL. */
    const char *restore_token;
    /** Ask the portal to mint a token for next time (`persist_mode = 2`). */
    bool persist;
    /** Crop applied by the engine, in source pixels, top-left origin. Ignored
     *  unless `region_enabled`; clamped to the negotiated frame size. */
    vs_rect region;
    bool region_enabled;
    /** Ceiling on delivered frames per second; 0 leaves the compositor's own
     *  rate alone. Frames above it are dropped, not queued. */
    uint32_t max_fps;
    /** Frames and audio buffers the engine may hold for the caller; 0 means 4.
     *  Overflow is dropped and counted, never blocked on. */
    uint32_t queue_depth;
    /** Deliver callbacks from the engine's PipeWire thread the moment a buffer
     *  arrives, with no copy, instead of queueing for `dispatch`. See the
     *  threading note above: this is WP-B5's mode, not the default. */
    bool direct_callbacks;
    bool capture_system_audio;
    /** PipeWire sink whose monitor to record, or NULL for the default sink. */
    const char *audio_sink;
    bool capture_microphone;
    /** PipeWire source to record, or NULL for the default source. */
    const char *audio_source;
    /** 0 means 48000 and 2. */
    uint32_t audio_rate;
    uint32_t audio_channels;
} vs_capture_stream_config;

/** Delivered from `vs_capture_stream_dispatch`, on the thread that called it --
 *  or, with `direct_callbacks`, from the engine's PipeWire thread. The frame is
 *  valid for the duration of the call only. */
typedef void (*vs_capture_frame_cb)(const vs_capture_frame *frame, void *user_data);
typedef void (*vs_capture_audio_cb)(const vs_capture_audio_buffer *buffer, void *user_data);

typedef struct vs_capture_stats {
    uint64_t frames_delivered;
    uint64_t frames_with_pts;
    /** Dropped by the engine's own frame-rate cap. */
    uint64_t frames_dropped_rate;
    /** Arrived while paused. */
    uint64_t frames_dropped_paused;
    /** The queue was full because the caller had not dispatched or pulled. */
    uint64_t frames_dropped_queue;
    /** `pw_stream_dequeue_buffer` returned NULL: PipeWire itself had none. */
    uint64_t buffers_missed;
    uint64_t audio_buffers_system;
    uint64_t audio_frames_system;
    uint64_t audio_buffers_microphone;
    uint64_t audio_frames_microphone;
    uint64_t audio_dropped_paused;
    uint64_t audio_dropped_queue;
    /** `arrival_ns - pts_ns` over the frames that carried a pts. */
    double latency_avg_ms;
    double latency_min_ms;
    double latency_max_ms;
    /** Total time spent paused, which is what was subtracted from both the
     *  video and the audio timeline. */
    int64_t paused_total_ns;
    /** First and last timeline position delivered, per kind, so a caller can
     *  assert the video and audio spans agree without keeping its own tallies. */
    int64_t video_timeline_first_ns;
    int64_t video_timeline_last_ns;
    int64_t audio_timeline_first_ns;
    int64_t audio_timeline_last_ns;
} vs_capture_stats;

/** Probe the session and build the capture engine for it. `preferred` names a
 *  backend ("portal" is the only one so far) or is NULL to probe. Probing needs
 *  a session bus carrying `org.freedesktop.portal.Desktop` with a ScreenCast
 *  interface on it. Returns NULL and sets `*result_out` (optional) when nothing
 *  is usable. */
vs_capture_engine *vs_capture_engine_create(const char *preferred, int *result_out);
/** Releases the engine. Every stream it opened must be stopped first. */
void vs_capture_engine_destroy(vs_capture_engine *engine);

const char *vs_capture_engine_name(const vs_capture_engine *engine);
/** Bitmask of `vs_capture_capability`, fixed for the life of the instance. */
uint32_t vs_capture_engine_capabilities(const vs_capture_engine *engine);
/** The portal's own `AvailableSourceTypes` and `AvailableCursorModes`, raw. */
uint32_t vs_capture_engine_source_types(const vs_capture_engine *engine);
uint32_t vs_capture_engine_cursor_modes(const vs_capture_engine *engine);
/** Names of the backends compiled into this build, NULL-terminated. */
const char *const *vs_capture_backend_names(void);

/** Sources of the given types (a mask; 0 means every type the session offers).
 *  The caller owns the array until `vs_capture_free_sources`. Asking for a type
 *  the session does not serve returns VS_OK with a count of 0, never a
 *  fabricated entry. Budget: 200 ms. */
int vs_capture_enumerate_sources(vs_capture_engine *engine, uint32_t types,
                                 vs_capture_source **sources_out, size_t *count_out);
void vs_capture_free_sources(vs_capture_source *sources, size_t count);

typedef enum vs_capture_shot_method {
    /** Portal Screenshot when it exists, otherwise a one-frame ScreenCast. */
    VS_CAPTURE_SHOT_AUTO = 0,
    VS_CAPTURE_SHOT_PORTAL = 1,
    VS_CAPTURE_SHOT_SCREENCAST = 2,
} vs_capture_shot_method;

typedef struct vs_capture_shot_request {
    uint32_t source_types;
    /** Crop applied by the engine after the pixels arrive. */
    vs_rect region;
    bool region_enabled;
    bool include_cursor;
    vs_capture_shot_method method;
    /** Restore token, so "screenshot this display again" does not re-prompt.
     *  ScreenCast path only; the Screenshot portal has no session to restore. */
    const char *restore_token;
    bool persist;
    /** Milliseconds to wait for the first frame on the ScreenCast path; 0 means
     *  5000. A damage-driven session delivers nothing until something repaints,
     *  so this is a real timeout, not a formality, and it is the one call in
     *  this section whose budget is the caller's to set. */
    uint32_t timeout_ms;
} vs_capture_shot_request;

/** One frame of whatever the session grants. `image_out` is filled on success
 *  and released with `vs_capture_image_free`. `method_used_out` (optional) says
 *  which path answered, and `token_out` (optional) receives a malloc'd restore
 *  token the caller frees, or NULL when none was minted. */
int vs_capture_screenshot(vs_capture_engine *engine,
                          const vs_capture_shot_request *request,
                          vs_capture_image *image_out,
                          vs_capture_shot_method *method_used_out,
                          char **token_out);

/** Negotiate a session and start delivering. Returns VS_ERR_NOT_APPLIED when
 *  `require_source_type` is set and the session granted a different type --
 *  the capture form of "the request was acknowledged and did something else".
 *  Budget: the portal's own, which includes a user picking a source, so this is
 *  the one blocking call here and callers run it off their UI thread. */
int vs_capture_stream_start(vs_capture_engine *engine,
                            const vs_capture_stream_config *config,
                            vs_capture_stream **stream_out);
/** Stop delivering, close the portal session and release the stream. Blocks
 *  only until the engine's own thread has joined, so once it returns no
 *  callback is running or can start and the caller's encoder can finalise. */
void vs_capture_stream_stop(vs_capture_stream *stream);

/** Install or remove (NULL) the sinks. Safe at any time, including from inside
 *  a callback. */
void vs_capture_stream_set_frame_callback(vs_capture_stream *stream,
                                          vs_capture_frame_cb callback, void *user_data);
void vs_capture_stream_set_audio_callback(vs_capture_stream *stream,
                                          vs_capture_audio_cb callback, void *user_data);

/** Pollable descriptor that becomes readable when a frame or an audio buffer is
 *  waiting, or -1 under `direct_callbacks`, where there is nothing to wait for.
 *  Owned by the stream; never closed by the caller. */
int vs_capture_stream_event_fd(const vs_capture_stream *stream);
/** Deliver everything queued to the installed callbacks, on this thread, and
 *  return how many buffers were delivered, or a negative `vs_result`. Never
 *  blocks: a caller that has nothing to do gets 0. */
int vs_capture_stream_dispatch(vs_capture_stream *stream);

/** Pull API, for a caller that wants the buffer rather than a callback -- the
 *  screenshot path and the tests. Waits up to `timeout_ms` (0 polls, negative
 *  waits until a frame or the stream's stop). Returns VS_OK with `frame_out`
 *  filled, or VS_ERR_TIMEOUT. Every VS_OK is matched by
 *  `vs_capture_stream_release_frame` before the next call; the frame is
 *  borrowed until then. It draws from the same queue as `dispatch`, so a stream
 *  uses one style or the other, not both. */
int vs_capture_stream_next_frame(vs_capture_stream *stream, vs_capture_frame *frame_out,
                                 int timeout_ms);
void vs_capture_stream_release_frame(vs_capture_stream *stream);
/** The same, for PCM. */
int vs_capture_stream_next_audio(vs_capture_stream *stream,
                                 vs_capture_audio_buffer *buffer_out, int timeout_ms);
void vs_capture_stream_release_audio(vs_capture_stream *stream);

/** Pause and resume. Buffers that arrive while paused are dropped and counted,
 *  and the gap is subtracted from the video and the audio timeline by the same
 *  amount, so the two stay aligned across a pause -- the Linux form of
 *  RecorderSampleTiming's single-clock rule. Both are idempotent. */
void vs_capture_stream_pause(vs_capture_stream *stream);
void vs_capture_stream_resume(vs_capture_stream *stream);
bool vs_capture_stream_is_paused(const vs_capture_stream *stream);

/** The source type the session actually granted, which is what a "record this
 *  window" feature must check when it did not set `require_source_type`:
 *  xdg-desktop-portal-wlr 0.7.1 answers a WINDOW request with a MONITOR
 *  stream. */
uint32_t vs_capture_stream_source_type(const vs_capture_stream *stream);
/** True when the granted type is not among the requested ones. */
bool vs_capture_stream_source_mismatch(const vs_capture_stream *stream);
/** Negotiated frame geometry, before the engine's crop. VS_ERR_TIMEOUT until
 *  the format has been negotiated, which is a round trip after the start. */
int vs_capture_stream_size(const vs_capture_stream *stream,
                           uint32_t *width_out, uint32_t *height_out);
vs_capture_pixel_format vs_capture_stream_format(const vs_capture_stream *stream);
/** Token minted by this session, or NULL. The consumer persists it and hands it
 *  back in `vs_capture_stream_config.restore_token`; the engine stores nothing
 *  itself. Borrowed, and valid until the stream is stopped. */
const char *vs_capture_stream_restore_token(const vs_capture_stream *stream);
void vs_capture_stream_stats(const vs_capture_stream *stream, vs_capture_stats *stats_out);

/** Pure timing arithmetic, exposed so the pause/resume rule can be unit-tested
 *  without a compositor, and so WP-B5 can retime buffers it produces itself (an
 *  encoder flush, a synthesized silent buffer) onto exactly the same timeline. */
typedef struct vs_capture_clock {
    int64_t epoch_ns;
    int64_t paused_total_ns;
    int64_t pause_started_ns;
    bool paused;
} vs_capture_clock;

void vs_capture_clock_start(vs_capture_clock *clock, int64_t now_ns);
void vs_capture_clock_pause(vs_capture_clock *clock, int64_t now_ns);
void vs_capture_clock_resume(vs_capture_clock *clock, int64_t now_ns);
/** Timeline position of a buffer captured at `capture_ns`. Never negative, and
 *  never advances while paused. */
int64_t vs_capture_clock_timeline(const vs_capture_clock *clock, int64_t capture_ns);

/** Clamp `region` into a `width` by `height` frame, top-left origin. Returns
 *  false when nothing is left, which is the caller's cue to refuse the crop
 *  rather than record a zero-sized file. */
bool vs_capture_clamp_region(vs_rect *region, uint32_t width, uint32_t height);
/* ------------------------------------------------------------------- audio */

/*
 * Mirrors `AudioGraph` in WP-12. Shaped by what the macOS services consume:
 *
 *   Sources/Vorssaint/Services/Audio/AppVolumeMixer.swift
 *     `MixerOutputDevice` (uid, name, isDefault), `MixerApp` (name, ownerPid,
 *     volume, selectedOutputDeviceUID, effectiveOutputDeviceUID), per-app
 *     volume up to 200 %, per-app output routing, the device and default-device
 *     listeners, and the headphone-disconnect detection that
 *     `loweringOutputVolumeIfHeadphonesDisconnected` hangs on
 *   Sources/Vorssaint/Services/Audio/SoundOutputSwitcher.swift
 *     cycling the system output through a chosen set of devices
 *   Sources/Vorssaint/Services/Audio/AudioInputDeviceManager.swift
 *     `MixerInputDevice`, the default input, input add/remove
 *   Sources/Vorssaint/Services/QuickTools/MicMuteService.swift
 *     mute every input and restore exactly what this feature muted
 *
 * The Linux mechanisms are PipeWire's registry, the `Props` param, the
 * `default` metadata object and the `Link` objects; the fallback is libpulse.
 * `linux/platform/audio/README.md` documents both and the percentage mapping.
 */

/* --- volume scale ---------------------------------------------------------
 *
 * Every volume here is LINEAR AMPLITUDE with 1.0 at unity, which is what
 * PipeWire's `channelVolumes` holds and what the macOS mixer means by 100 %
 * (`AppVolumeMixer` runs 0...2, 1.0 being untouched passthrough).
 *
 * It is deliberately *not* the number `wpctl` and the GNOME/KDE sliders show.
 * Those are cubic: `wpctl set-volume ID 0.5` writes a linear 0.125. Converting
 * is the UI's job and goes through `vs_audio_linear_to_cubic` /
 * `vs_audio_cubic_to_linear` so it is done in one place.
 */

/** Largest linear volume the mixer accepts: 200 %.
 *
 *  Inherited from the macOS mixer's `AppVolumeMixer.maxVolume`, not chosen
 *  here. The scale is linear on both platforms, so a settings backup carries
 *  a boosted row across unchanged — and a lower ceiling on Linux would
 *  silently turn someone's 200 % into 150 % on import, which loses the
 *  user's setting without telling them. WirePlumber's own tools stop at
 *  150 %, so the panel owns warning about clipping above that. */
#define VS_AUDIO_MAX_VOLUME 2.0f
/** Where WirePlumber's tools stop; above this the panel warns about clipping. */
#define VS_AUDIO_CLIPPING_HAZARD_VOLUME 1.5f
/** What the panel shows as "100 %". */
#define VS_AUDIO_UNITY_VOLUME 1.0f

/** Capabilities of an audio backend. */
typedef enum vs_audio_capability {
    /** A pipewire daemon answered; the full backend is in use. */
    VS_AUDIO_HAS_PIPEWIRE = 1u << 0,
    /** The libpulse fallback is in use (a PulseAudio-only host). */
    VS_AUDIO_HAS_PULSE_FALLBACK = 1u << 1,
    /** `route_stream` can send one application's audio to a chosen sink. */
    VS_AUDIO_CAN_ROUTE_PER_STREAM = 1u << 2,
    /** Volumes above 1.0 are honoured rather than clamped. */
    VS_AUDIO_CAN_BOOST_OVER_100 = 1u << 3,
    /** The backend reports graph changes through `dispatch`. Without it the
     *  caller must poll `list` itself. */
    VS_AUDIO_HAS_EVENTS = 1u << 4,
} vs_audio_capability;

/**
 * Identifies a device or stream for the life of the backend instance.
 *
 * PipeWire's `object.serial`, which is monotonic: a removed node's number is
 * never handed to a later one, unlike the global id, which is recycled. In the
 * libpulse fallback it is the sink/source/sink-input index with the object kind
 * in its top byte, because PulseAudio numbers the four kinds separately. 0 is
 * never a valid id.
 */
typedef uint32_t vs_audio_id;

typedef enum vs_audio_node_kind {
    /** `media.class` Audio/Sink: an output device. */
    VS_AUDIO_NODE_SINK = 1u << 0,
    /** Audio/Source: an input device. Monitor sources are excluded. */
    VS_AUDIO_NODE_SOURCE = 1u << 1,
    /** Stream/Output/Audio: an application playing. A mixer row. */
    VS_AUDIO_NODE_STREAM_OUTPUT = 1u << 2,
    /** Stream/Input/Audio: an application recording. */
    VS_AUDIO_NODE_STREAM_INPUT = 1u << 3,
} vs_audio_node_kind;

#define VS_AUDIO_NODE_ANY_DEVICE (VS_AUDIO_NODE_SINK | VS_AUDIO_NODE_SOURCE)
#define VS_AUDIO_NODE_ANY_STREAM \
    (VS_AUDIO_NODE_STREAM_OUTPUT | VS_AUDIO_NODE_STREAM_INPUT)
#define VS_AUDIO_NODE_ANY (VS_AUDIO_NODE_ANY_DEVICE | VS_AUDIO_NODE_ANY_STREAM)

typedef enum vs_audio_node_flag {
    /** The default sink (SINK) or default source (SOURCE). */
    VS_AUDIO_NODE_IS_DEFAULT = 1u << 0,
    /** The node is muted. */
    VS_AUDIO_NODE_MUTED = 1u << 1,
    /** `volume` is real. A node with no `Props` volume at all shows no slider
     *  rather than a slider that does nothing. */
    VS_AUDIO_NODE_HAS_VOLUME = 1u << 2,
    /** `pid` is real rather than the -1 placeholder. */
    VS_AUDIO_NODE_HAS_PID = 1u << 3,
    /**
     * Sound is actually moving through this node right now, as opposed to an
     * application that holds a stream open while silent. `AudioStream.isActive`
     * in WP-12, `MixerApp.isPlaying` on macOS: the mixer shows a live indicator
     * for it and sorts silent rows down, but still lists them, because a row
     * that vanished when the app paused would take its volume slider with it.
     */
    VS_AUDIO_NODE_ACTIVE = 1u << 4,
} vs_audio_node_flag;

#define VS_AUDIO_APP_ID_MAX 128
#define VS_AUDIO_TRANSPORT_MAX 32
#define VS_AUDIO_NAME_MAX 256
#define VS_AUDIO_DESCRIPTION_MAX 256
#define VS_AUDIO_APP_NAME_MAX 128
#define VS_AUDIO_ICON_NAME_MAX 128
#define VS_AUDIO_MEDIA_NAME_MAX 256

typedef struct vs_audio_node {
    vs_audio_id id;
    vs_audio_node_kind kind;
    /** `node.name`. Stable across restarts: this is what to persist a saved
     *  volume or route against, the way the macOS mixer persists by bundle id. */
    char name[VS_AUDIO_NAME_MAX];
    /** `node.description`: what to show. Falls back to `name` when empty. */
    char description[VS_AUDIO_DESCRIPTION_MAX];
    /** `application.name` for streams (`MixerApp.name`). */
    char app_name[VS_AUDIO_APP_NAME_MAX];
    /**
     * Streams only: a stable identity for the application, and the key a saved
     * volume or route is persisted against. `AudioStream.applicationID` in
     * WP-12, and the counterpart of the bundle id the macOS mixer saves under
     * -- `name` cannot do that job, because every `pw-play` shares the
     * `node.name` "pw-play".
     *
     * Taken from `application.id`, then `application.process.binary`, then
     * `application.name`. The fall back to a display name is the same rule
     * `MixerRoutingSupport.rowIdentity` applies on macOS for a process with no
     * bundle id, and for the same reason: a game or a bare executable is still
     * worth remembering a volume for, and its name is the only stable thing it
     * has. Empty only when the client set none of the three, in which case the
     * row is adjustable but saves nothing -- `MixerApp.persistenceID` being nil.
     */
    char app_id[VS_AUDIO_APP_ID_MAX];
    /** `application.icon-name`: an XDG icon name, the Linux answer to the
     *  macOS mixer's per-app icon. May be empty. */
    char icon_name[VS_AUDIO_ICON_NAME_MAX];
    /** `media.name`: what the app is playing right now, and it changes as the
     *  track changes. May be empty. */
    char media_name[VS_AUDIO_MEDIA_NAME_MAX];
    /** Owning process (`MixerApp.ownerPid`), or -1 when the backend cannot say. */
    int32_t pid;
    /**
     * Devices only: the bus, for the icon the mixer draws -- "bluetooth",
     * "usb", "hdmi", "builtin", or "" when the backend cannot tell.
     * `AudioSink.transport` in WP-12, deliberately free-form there because
     * PipeWire's `device.bus` and CoreAudio's transport type do not enumerate
     * the same set.
     */
    char transport[VS_AUDIO_TRANSPORT_MAX];
    /** Linear amplitude; see the scale note above. Meaningful only with
     *  `VS_AUDIO_NODE_HAS_VOLUME`. */
    float volume;
    /** Bitmask of `vs_audio_node_flag`. */
    uint32_t flags;
    /**
     * Streams only: the sink this stream is pinned to, or 0 when it follows
     * the default. `MixerApp.selectedOutputDeviceUID`.
     */
    vs_audio_id target_id;
    /**
     * Streams only: the node this stream's audio is actually reaching now, or
     * 0 when it is not connected. `MixerApp.effectiveOutputDeviceUID`.
     *
     * The difference between this and `target_id` is the whole read-back:
     * asking for a route is not the same as the audio arriving there. The
     * PipeWire backend reads it from the `Link` objects; the libpulse fallback
     * reads the sink the input sits on, which is the strongest answer that
     * protocol has.
     */
    vs_audio_id effective_id;
} vs_audio_node;

typedef enum vs_audio_event_type {
    /**
     * The graph changed in a way worth re-reading. Debounced: a device
     * appearing brings a burst of a dozen registry and param changes, and the
     * mixer wants one refresh after the burst rather than twelve.
     */
    VS_AUDIO_EVENT_CHANGED = 1,
    /** The default sink changed. `id` is the new one, 0 if there is none. */
    VS_AUDIO_EVENT_DEFAULT_SINK_CHANGED,
    VS_AUDIO_EVENT_DEFAULT_SOURCE_CHANGED,
    /**
     * A sink went away while it was the default: headphones unplugged, a USB
     * DAC pulled, a Bluetooth headset dropped. `id` and `name` describe the
     * sink that left, because by the time this is delivered it is gone from
     * any listing. This is what the macOS "lower the volume when headphones
     * disconnect" behaviour hangs on.
     */
    VS_AUDIO_EVENT_DEFAULT_SINK_DISCONNECTED,
} vs_audio_event_type;

typedef struct vs_audio_event {
    vs_audio_event_type type;
    /** The node the event is about, or 0. */
    vs_audio_id id;
    /** Its `node.name`, or "". Valid only for the duration of the callback. */
    const char *name;
} vs_audio_event;

typedef void (*vs_audio_event_cb)(const vs_audio_event *event, void *user_data);

typedef struct vs_audio_system vs_audio_system;

struct vs_audio_system {
    /** Backend identity, for logs and the capabilities page: "pipewire" or
     *  "libpulse". */
    const char *name;
    /** Bitmask of `vs_audio_capability`. A member whose capability bit is clear
     *  still exists and returns VS_ERR_UNSUPPORTED. These never change for the
     *  life of the instance: losing the audio server means losing the
     *  connection, and the caller recreates the backend. */
    uint32_t capabilities;
    void *impl;

    /** Snapshot of every node whose kind is in `kind_mask` (a bitmask of
     *  `vs_audio_node_kind`). The caller owns the array until it passes it to
     *  `free_list`; a count of 0 comes with a NULL pointer.
     *
     *  Budget: 2 s, and nothing like it in practice. The PipeWire backend
     *  answers from its own mirror of the graph and turns the loop once without
     *  blocking, so it cannot wait at all; the libpulse fallback has no mirror
     *  and asks the server, which is where the cap applies -- one round trip
     *  per node kind plus one for the defaults.
     *
     *  Either way this may run the backend's connection, so events it uncovers
     *  are delivered by a later `dispatch` rather than by this call. */
    int (*list)(vs_audio_system *self, uint32_t kind_mask,
                vs_audio_node **nodes_out, size_t *count_out);
    void (*free_list)(vs_audio_system *self, vs_audio_node *nodes, size_t count);

    /** Set a node's linear volume on every channel, clamped to
     *  [0, VS_AUDIO_MAX_VOLUME], and read it back. VS_ERR_NOT_APPLIED when the
     *  write was accepted and a different value came back, which is what a
     *  server that clamps looks like. Budget: 2 s. */
    int (*set_volume)(vs_audio_system *self, vs_audio_id id, float linear_volume);
    /** Same read-back contract. Budget: 2 s. */
    int (*set_mute)(vs_audio_system *self, vs_audio_id id, bool mute);

    /** Send one stream's audio to one sink -- the per-app output routing the
     *  macOS mixer does by re-rendering through an aggregate device. `sink_id`
     *  of 0 clears the pin and the stream follows the default again.
     *
     *  Returns VS_OK only once the audio has actually arrived: the PipeWire
     *  backend waits for the stream's links to land on that sink, and answers
     *  VS_ERR_NOT_APPLIED if they never do. Budget: 3 s. */
    int (*route_stream)(vs_audio_system *self, vs_audio_id stream_id,
                        vs_audio_id sink_id);

    /** Make a sink the default output, and read it back. The PipeWire backend
     *  writes both `default.audio.sink` and `default.configured.audio.sink`, so
     *  the choice is both in effect now and restored at the next login.
     *  Budget: 3 s. */
    int (*set_default_sink)(vs_audio_system *self, vs_audio_id sink_id);
    /** The same for the default input, which is what AudioInputDeviceManager's
     *  preferred-input setting writes. Budget: 3 s. */
    int (*set_default_source)(vs_audio_system *self, vs_audio_id source_id);

    /** Mute every input, or restore.
     *
     *  Turning it on records each source's mute state; turning it off restores
     *  exactly the sources this feature muted, leaving alone any the user muted
     *  themselves in the meantime -- the rule `MicMuteSupport.restoreTargets`
     *  enforces on macOS. The record outlives the process (see
     *  linux/platform/audio/README.md), so a crash with the mic muted is
     *  recoverable. `changed_out` may be NULL. Budget: 2 s per source. */
    int (*mute_all_inputs)(vs_audio_system *self, bool mute, size_t *changed_out);

    /** Install the event sink. Pass NULL to remove it. The callback runs inside
     *  `dispatch`, on the calling thread, and may call back into this same
     *  instance only after `dispatch` returns. */
    int (*set_event_callback)(vs_audio_system *self, vs_audio_event_cb callback,
                              void *user_data);
    /** Pollable descriptor that becomes readable when events are pending.
     *
     *  -1 means the backend has no single pollable descriptor and the caller
     *  must call `dispatch` on a timer instead; the libpulse fallback is in
     *  that position, because `pa_mainloop` polls a set of descriptors it does
     *  not expose. VS_AUDIO_HAS_EVENTS says whether events arrive at all, which
     *  is a separate question from whether there is an fd to wait on. */
    int (*event_fd)(vs_audio_system *self);
    /** Drain what is pending and deliver it to the callback. Never blocks. The
     *  only place the callback runs. Returns the number of events delivered, or
     *  a negative `vs_result`. */
    int (*dispatch)(vs_audio_system *self);

    void (*destroy)(vs_audio_system *self);
};

/** Connect to the audio server and build a backend.
 *
 *  `preferred` forces one backend by name ("pipewire", "libpulse") and fails
 *  with VS_ERR_NO_BACKEND if it is not usable here; NULL tries PipeWire first
 *  and falls back to libpulse when `pw_context_connect` finds nothing to
 *  connect to, which is the whole of the PipeWire-absence detection -- anything
 *  short of a real connection can be wrong, since a socket can exist with
 *  nothing behind it.
 *
 *  Returns NULL when no audio server answered; `*result_out` (optional) says
 *  why. */
vs_audio_system *vs_audio_system_create(const char *preferred, int *result_out);

/** Names of the backends compiled into this build, NULL-terminated. */
const char *const *vs_audio_backend_names(void);

/** Linear amplitude to the cubic number wpctl and the desktop sliders show. */
float vs_audio_linear_to_cubic(float linear);
/** The inverse: a cubic slider position to the linear volume used here. */
float vs_audio_cubic_to_linear(float cubic);

/* ----------------------------------------------------------------- sensors */

/*
 * Mirrors `SystemSensors` in Sources/VorssaintCore/Platform/SystemSensors.swift
 * (WP-12). Shaped by what the macOS services actually consume:
 *
 *   Sources/Vorssaint/Services/SystemMonitor/SystemMonitor.swift `SystemSnapshot`
 *     cpuUsage, gpuUsage, memoryUsed/AppUsed/Total/Compressed/Cached/SwapUsed,
 *     memoryPressure, cpuTemperature, gpuTemperature, batteryTemperature,
 *     fanSpeeds, netDown/UpBytesPerSec, disk, power, peripheralBatteries
 *   Sources/Vorssaint/Services/Metrics/PowerSampler.swift `PowerReading`
 *     systemWatts, adapterWatts, adapterMaxWatts, batteryWatts, chargePercent,
 *     timeRemainingSeconds, healthPercent, cycleCount, isCharging,
 *     externalConnected, hasBattery
 *   Sources/Vorssaint/Services/SystemMonitor/ProcessUsageService.swift
 *     per-process CPU as a delta over the monitor interval, per-process memory,
 *     helper processes consolidated under the app responsible for them
 *   Sources/Vorssaint/Services/Metrics/TemperatureSensorSelector.swift
 *     which sensor is "the" CPU temperature, and the plausibility window
 *
 * This layer returns raw samples, never formatted strings and never derived
 * display values: `MetricFormat`, `MonitorSamplingPolicy`, `SustainedAlertGate`
 * and `BatteryTimeSupport` are pure Swift in `Sources/VorssaintCore` and are
 * reused unchanged above it.
 *
 * Every reader is rooted at `vs_sensors_options.root` (default "/"), so a
 * fixture tree captured from a real machine can be replayed in a test on a
 * container that has no hwmon, no battery and no GPU. The only call that
 * escapes the root is `statvfs` on a mount point, and under a non-"/" root it
 * is applied to the rooted path so that it still measures something real.
 */

typedef enum vs_sensors_capability {
    /** `/sys/class/hwmon` exists and has at least one device: `temperatures`
     *  and `fans` can answer. */
    VS_SENSORS_HAS_HWMON = 1u << 0,
    /** `org.freedesktop.UPower` is on the system bus: `power` and
     *  `peripheral_batteries` come from it. */
    VS_SENSORS_HAS_UPOWER = 1u << 1,
    /** `/sys/class/power_supply` has a battery or a mains supply: the fallback
     *  path for `power` is usable. */
    VS_SENSORS_HAS_POWER_SUPPLY = 1u << 2,
    /** At least one amdgpu card exposes `gpu_busy_percent`. */
    VS_SENSORS_HAS_AMDGPU = 1u << 3,
    /** `libnvidia-ml.so.1` loaded and `nvmlInit` succeeded. */
    VS_SENSORS_HAS_NVML = 1u << 4,
    /** An i915/xe card exposes a `gt` (or `gt/gt0`) frequency tree. */
    VS_SENSORS_HAS_INTEL_GPU = 1u << 5,
    /** `/proc/pressure/memory` exists: `vs_memory_sample.pressure` is real PSI
     *  rather than the MemAvailable estimate. */
    VS_SENSORS_HAS_PSI = 1u << 6,
    /** `/proc/<pid>/io` is readable: a per-process disk breakdown is possible.
     *  Probed and reported; the rows themselves are panel work, not read here. */
    VS_SENSORS_HAS_PROCFS_IO = 1u << 7,
} vs_sensors_capability;

/** How a temperature or fan reading is classified, replacing the SMC-key
 *  prefix rules in `TemperatureSensorSelector`. The driver name decides first;
 *  multi-sensor platform chips (thinkpad, dell_smm, asus, nct6775) fall through
 *  to their per-sensor label. */
typedef enum vs_sensor_kind {
    VS_SENSOR_OTHER = 0,
    VS_SENSOR_CPU,
    VS_SENSOR_GPU,
    VS_SENSOR_BATTERY,
    VS_SENSOR_DRIVE,
    /** Ambient / chassis / chipset: acpitz, pch, SYSTIN. */
    VS_SENSOR_SYSTEM,
} vs_sensor_kind;

/** The `ProcessInfo.ThermalState` replacement (`ThermalPressure` in Swift),
 *  derived from hwmon `temp*_crit` / `temp*_max` trip points. */
typedef enum vs_thermal_pressure {
    VS_THERMAL_NOMINAL = 0,
    VS_THERMAL_FAIR,
    VS_THERMAL_SERIOUS,
    VS_THERMAL_CRITICAL,
} vs_thermal_pressure;

#define VS_SENSORS_NAME_MAX 64
#define VS_SENSORS_LABEL_MAX 96
#define VS_SENSORS_ID_MAX 128
#define VS_SENSORS_PATH_MAX 256
#define VS_SENSORS_MAX_CORES 512

/* --- CPU */

typedef struct vs_cpu_core_sample {
    /** Kernel cpu index, as in `/proc/stat`'s `cpuN`. */
    int32_t index;
    /** 0...1 over the interval since the previous `cpu()` on this instance;
     *  0 when `has_rates` is false. */
    double usage;
    /** `scaling_cur_freq`, else the `/proc/cpuinfo` "cpu MHz" line, else 0. */
    double frequency_mhz;
    /** `topology/physical_package_id` and `topology/core_id`, or -1. */
    int32_t package_id;
    int32_t core_id;
} vs_cpu_core_sample;

typedef struct vs_cpu_sample {
    /** 0...1 over all cores, the `SystemSnapshot.cpuUsage` value. */
    double total_usage;
    /** Busy split, each 0...1: user includes nice, system includes irq and
     *  softirq, as `host_statistics`'s CPU_LOAD_INFO buckets do. */
    double user_usage;
    double system_usage;
    double idle_usage;
    /** `iowait`, which macOS has no counterpart for; the panel may ignore it. */
    double iowait_usage;
    /** False on the first call of an instance, and after a counter reset: there
     *  is no previous sample to subtract, so every usage is 0. The macOS side
     *  returns nil in exactly that case. */
    bool has_rates;
    /** Seconds of wall clock covered by the deltas, 0 when `has_rates` is
     *  false. */
    double interval_seconds;
    /** `/proc/loadavg`. */
    double load_average[3];
    /** Online CPUs seen in `/proc/stat`, capped at VS_SENSORS_MAX_CORES. */
    size_t core_count;
    /** Distinct (package, core) pairs, i.e. physical cores; 0 when the
     *  topology tree is absent. */
    size_t physical_core_count;
    size_t package_count;
    vs_cpu_core_sample cores[VS_SENSORS_MAX_CORES];
} vs_cpu_sample;

/* --- memory */

typedef struct vs_memory_sample {
    /** `MemTotal`. */
    uint64_t total_bytes;
    /** "Memory in use": `MemTotal - MemAvailable`. The analogue of Activity
     *  Monitor's Memory Used, which is app + wired + compressed and excludes
     *  reclaimable file cache; `MemAvailable` is the kernel's own estimate of
     *  what a new allocation could take without swapping. */
    uint64_t used_bytes;
    /** "App memory": `AnonPages`, the anonymous memory of processes. macOS
     *  computes internal minus purgeable pages for the same quantity. */
    uint64_t app_bytes;
    /** Droppable file-backed memory: `Buffers + Cached + SReclaimable - Shmem`,
     *  floored at 0. macOS's "Cached Files". */
    uint64_t cached_bytes;
    /** `Zswapped` when the kernel compresses swap pages in RAM, else 0 with
     *  `has_compressed` false. macOS always has a compressor; Linux usually
     *  does not, and the row is hidden rather than shown as zero. */
    uint64_t compressed_bytes;
    bool has_compressed;
    uint64_t swap_total_bytes;
    uint64_t swap_used_bytes;
    /** `MemAvailable`, kept because it is the number the mapping above is
     *  built on and the panel's tooltip explains it. */
    uint64_t available_bytes;
    /** 0...1. PSI `some avg10 / 100` from `/proc/pressure/memory` when
     *  VS_SENSORS_HAS_PSI is set: the closest analogue of macOS's
     *  `kern.memorystatus_vm_pressure_level`, being the share of the last ten
     *  seconds in which some task stalled on memory. Without PSI it is the
     *  MemAvailable shortfall `1 - MemAvailable/MemTotal` clamped to 0...1,
     *  which is a level rather than a stall and is marked as such. */
    double pressure;
    bool pressure_is_psi;
    /** PSI `full avg10 / 100`, 0 without PSI. */
    double pressure_full;
} vs_memory_sample;

/* --- network */

typedef enum vs_network_kind {
    VS_NETWORK_UNKNOWN = 0,
    VS_NETWORK_ETHERNET,
    VS_NETWORK_WIFI,
    VS_NETWORK_LOOPBACK,
    /** No `device` symlink under `/sys/class/net/<if>`: bridges, tun/tap, veth,
     *  VPN tunnels. `MetricFormat.includeNetworkInterface` excludes the same
     *  class of interface on macOS so a VPN does not double-count. */
    VS_NETWORK_VIRTUAL,
} vs_network_kind;

typedef struct vs_network_sample {
    char name[VS_SENSORS_NAME_MAX];
    vs_network_kind kind;
    /** The interface carrying the IPv4 or IPv6 default route. */
    bool is_default_route;
    /** `operstate` is "up". */
    bool is_up;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_errors;
    uint64_t tx_errors;
    uint64_t rx_dropped;
    uint64_t tx_dropped;
    /** Bytes per second since the previous `network()` on this instance; 0 when
     *  `has_rates` is false. Identical arithmetic to `MetricFormat.netSpeed`,
     *  including its rule that a non-increasing counter yields 0 rather than a
     *  spike. */
    double rx_bytes_per_second;
    double tx_bytes_per_second;
    bool has_rates;
} vs_network_sample;

/* --- disk */

typedef struct vs_disk_device_sample {
    /** Kernel name: "nvme0n1", "sda", "sda1". */
    char name[VS_SENSORS_NAME_MAX];
    bool is_partition;
    /** `/proc/diskstats` cumulative counters, converted with the fixed 512-byte
     *  stat sector the kernel documents for these fields. */
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t read_ios;
    uint64_t write_ios;
    /** Milliseconds spent doing I/O (field 13), for a future busy-percent. */
    uint64_t io_ticks;
    double read_bytes_per_second;
    double write_bytes_per_second;
    bool has_rates;
} vs_disk_device_sample;

typedef struct vs_disk_mount_sample {
    /** Mount point, the identity the panel shows. */
    char mount_point[VS_SENSORS_PATH_MAX];
    /** Backing source as `/proc/self/mounts` spells it. */
    char source[VS_SENSORS_PATH_MAX];
    char filesystem[VS_SENSORS_NAME_MAX];
    /** Kernel device name behind `source`, "" when it is not a block device;
     *  this is the join to `vs_disk_device_sample.name`. */
    char device[VS_SENSORS_NAME_MAX];
    bool is_read_only;
    /** `statvfs`: `f_blocks * f_frsize` and `f_bavail * f_frsize` (the
     *  unprivileged free figure, as Finder reports). */
    uint64_t total_bytes;
    uint64_t free_bytes;
} vs_disk_mount_sample;

/* --- temperatures and fans */

typedef struct vs_temperature_sample {
    /** Stable across reboots as far as sysfs is: "<driver>/<sensor>", e.g.
     *  "coretemp/temp1_input". `TemperatureSensorSelector` persists the user's
     *  chosen sensor by the equivalent SMC key. */
    char id[VS_SENSORS_ID_MAX];
    /** `temp*_label` when the driver supplies one, else "<driver> tempN". */
    char label[VS_SENSORS_LABEL_MAX];
    /** hwmon `name`: "coretemp", "k10temp", "nvme", "amdgpu"... */
    char driver[VS_SENSORS_NAME_MAX];
    vs_sensor_kind kind;
    double celsius;
    /** `temp*_max` / `temp*_crit`, 0 when the driver does not publish one. */
    double high_celsius;
    double critical_celsius;
    /** The reading the badge shows by default for its kind: the package sensor
     *  (`Package id N`, `Tctl`, `Tdie`) or, failing that, the hottest plausible
     *  reading of that kind, which is the same fallback
     *  `TemperatureSensorSelector.displayedCPUTemperature` makes. */
    bool is_primary;
} vs_temperature_sample;

typedef struct vs_fan_sample {
    char id[VS_SENSORS_ID_MAX];
    char label[VS_SENSORS_LABEL_MAX];
    char driver[VS_SENSORS_NAME_MAX];
    int32_t rpm;
    /** `fan*_min` / `fan*_max`, -1 when absent. */
    int32_t min_rpm;
    int32_t max_rpm;
    /** A `pwm<N>` file exists next to this fan. Writing it is a privileged path
     *  and belongs to the helper (WP-C6, `docs/linux-port/PRIVILEGES.md`);
     *  nothing here ever writes. */
    bool is_controllable;
    /** Current `pwm<N>` value 0...255, -1 when there is none. */
    int32_t pwm;
    /** Path of the `pwm<N>` file as seen under the configured root, "" when
     *  there is none, so the helper is asked about a file this layer saw. */
    char pwm_path[VS_SENSORS_PATH_MAX];
} vs_fan_sample;

/* --- power */

typedef enum vs_battery_state {
    VS_BATTERY_UNKNOWN = 0,
    VS_BATTERY_CHARGING,
    VS_BATTERY_DISCHARGING,
    VS_BATTERY_EMPTY,
    VS_BATTERY_FULL,
    VS_BATTERY_PENDING_CHARGE,
    VS_BATTERY_PENDING_DISCHARGE,
} vs_battery_state;

/** UPower's `Type`, so a peripheral row can show the right icon.
 *  `PeripheralBatteryKind` in `PeripheralBatterySupport` is the Swift side. */
typedef enum vs_battery_kind {
    VS_BATTERY_KIND_UNKNOWN = 0,
    VS_BATTERY_KIND_BATTERY,
    VS_BATTERY_KIND_UPS,
    VS_BATTERY_KIND_MOUSE,
    VS_BATTERY_KIND_KEYBOARD,
    VS_BATTERY_KIND_HEADSET,
    VS_BATTERY_KIND_PHONE,
    VS_BATTERY_KIND_TOUCHPAD,
    VS_BATTERY_KIND_GAMEPAD,
    VS_BATTERY_KIND_PEN,
    VS_BATTERY_KIND_OTHER,
} vs_battery_kind;

typedef struct vs_battery_sample {
    /** UPower object path tail, or the `/sys/class/power_supply` directory
     *  name. */
    char id[VS_SENSORS_ID_MAX];
    char label[VS_SENSORS_LABEL_MAX];
    char vendor[VS_SENSORS_NAME_MAX];
    vs_battery_kind kind;
    vs_battery_state state;
    /** 0...100. */
    double percentage;
    bool has_percentage;
    /** Watts. Positive while charging, negative while discharging, the sign
     *  convention `PowerReading.batteryWatts` already uses. UPower's
     *  `EnergyRate` is unsigned, so the sign comes from `State`. */
    double watts;
    bool has_watts;
    double time_to_empty_seconds;
    double time_to_full_seconds;
    bool has_time_to_empty;
    bool has_time_to_full;
    /** Present full capacity over design capacity, 0...1
     *  (`PowerReading.healthPercent / 100`). */
    double health;
    bool has_health;
    int32_t cycle_count;
    bool has_cycle_count;
    double temperature_celsius;
    bool has_temperature;
    /** Energy in watt-hours, when the source reports charge in energy units. */
    double energy_wh;
    double energy_full_wh;
    double energy_full_design_wh;
    double voltage_v;
} vs_battery_sample;

typedef struct vs_power_sample {
    /** A battery was found at all (`PowerReading.hasBattery`). */
    bool has_battery;
    vs_battery_sample battery;
    /** A mains/USB-PD supply is online (`PowerReading.externalConnected`). */
    bool external_connected;
    /** Real-time draw from the adapter, watts (`PowerReading.adapterWatts`):
     *  the mains supply's `power_now`, else `voltage_now * current_now`. */
    double adapter_watts;
    bool has_adapter_watts;
    /** The charger's rating, watts (`PowerReading.adapterMaxWatts`): a USB-PD
     *  supply's `voltage_max_design * current_max`, else `input_power_limit`. */
    double adapter_max_watts;
    bool has_adapter_max_watts;
    /** Whole-machine draw, watts (`PowerReading.systemWatts`). Linux has no
     *  SMC `PSTR`: on a laptop running on battery this is the battery's own
     *  discharge rate, and it is absent while plugged in. Never synthesised. */
    double system_watts;
    bool has_system_watts;
} vs_power_sample;

/* --- GPU */

typedef enum vs_gpu_vendor {
    VS_GPU_VENDOR_UNKNOWN = 0,
    VS_GPU_VENDOR_AMD,
    VS_GPU_VENDOR_INTEL,
    VS_GPU_VENDOR_NVIDIA,
} vs_gpu_vendor;

typedef struct vs_gpu_sample {
    /** "card0", or "nvml0" for an NVML device with no sysfs sibling. */
    char id[VS_SENSORS_ID_MAX];
    char name[VS_SENSORS_LABEL_MAX];
    /** The kernel driver, "amdgpu" / "i915" / "xe" / "nvidia". */
    char driver[VS_SENSORS_NAME_MAX];
    vs_gpu_vendor vendor;
    /** 0...1 (`SystemSnapshot.gpuUsage`). amdgpu `gpu_busy_percent`, NVML
     *  utilization.gpu; Intel publishes no equivalent (see the vendor matrix in
     *  docs/linux-port/SENSORS_BACKEND.md) and leaves `has_busy` false. */
    double busy;
    bool has_busy;
    uint64_t vram_used_bytes;
    uint64_t vram_total_bytes;
    bool has_vram;
    double temperature_celsius;
    bool has_temperature;
    double watts;
    bool has_watts;
    double clock_mhz;
    bool has_clock;
} vs_gpu_sample;

/* --- processes */

typedef struct vs_process_sample {
    int32_t pid;
    /** `/proc/<pid>/comm`. */
    char comm[VS_SENSORS_NAME_MAX];
    /** Display name: the matching `.desktop` file's `Name`, else `argv[0]`'s
     *  basename, else `comm`. `ProcessUsage.name` on the Swift side. */
    char name[VS_SENSORS_LABEL_MAX];
    /** What rows are summed under when `group_by_app` is set: the desktop file
     *  id when one matched, else `comm`. The Linux stand-in for macOS's
     *  responsible process. */
    char group_key[VS_SENSORS_ID_MAX];
    /** Cumulative `utime + stime` converted with `sysconf(_SC_CLK_TCK)`. */
    uint64_t cpu_time_ns;
    /** Percentage 0...100 of the whole machine over the interval, by the same
     *  arithmetic as `MetricFormat.processCPUPercentage`; 0 when `has_rate` is
     *  false. */
    double cpu_percent;
    bool has_rate;
    /** `VmRSS`. */
    uint64_t rss_bytes;
    /** `RssAnon`: the closest thing to macOS's physical footprint, which also
     *  excludes file-backed pages. */
    uint64_t anon_bytes;
    uint64_t swap_bytes;
    /** Number of pids summed into this row; 1 unless grouped. */
    uint32_t member_count;
} vs_process_sample;

typedef enum vs_process_sort {
    VS_PROCESS_SORT_CPU = 0,
    VS_PROCESS_SORT_MEMORY,
} vs_process_sort;

typedef struct vs_process_query {
    vs_process_sort sort;
    /** Rows to return. 0 means every row. */
    size_t limit;
    /** Sum rows under `group_key`, as `ProcessUsageService.groupedByApp` does. */
    bool group_by_app;
    /** Drop rows below this CPU percentage when sorting by CPU. The macOS side
     *  uses 0.01. */
    double minimum_cpu_percent;
} vs_process_query;

/* --- events */

typedef enum vs_sensors_event_type {
    /** `thermal_pressure` changed. `ThermalPressure`'s Swift observer is this
     *  event and nothing else. */
    VS_SENSORS_EVENT_THERMAL_PRESSURE = 1,
    /** UPower went away or came back; `capabilities` has already changed. */
    VS_SENSORS_EVENT_BACKEND_LOST,
} vs_sensors_event_type;

typedef struct vs_sensors_event {
    vs_sensors_event_type type;
    vs_thermal_pressure thermal_pressure;
} vs_sensors_event;

typedef void (*vs_sensors_event_cb)(const vs_sensors_event *event, void *user_data);

typedef struct vs_sensors_options {
    /** Prefix every `/proc` and `/sys` path with this. NULL or "" means "/".
     *  A fixture tree captured from a real machine is replayed by passing its
     *  directory here, which is how this backend is tested on a container with
     *  no hwmon, no battery and no GPU. */
    const char *root;
    /** "upower", "sysfs" or NULL to probe (UPower first, sysfs when it is not
     *  on the bus). A non-"/" root forces "sysfs": a fixture tree has no bus. */
    const char *power_backend;
    /** Skip NVML entirely. The default loads it if it is there, so this exists
     *  to prove the no-driver path in a test. */
    bool disable_nvml;
} vs_sensors_options;

typedef struct vs_sensors_system vs_sensors_system;

struct vs_sensors_system {
    /** "procfs" always; the power and GPU sources it found are in
     *  `capabilities` and in `power_backend_name`. */
    const char *name;
    /** "upower", "power_supply", or "none". */
    const char *power_backend_name;
    /** Bitmask of `vs_sensors_capability`. A member whose capability bit is
     *  clear still exists; it returns VS_OK with an empty list, or
     *  VS_ERR_UNSUPPORTED for the single-value calls. Capabilities shrink only
     *  when a channel goes away (UPower leaving the bus), announced as
     *  VS_SENSORS_EVENT_BACKEND_LOST. */
    uint32_t capabilities;
    void *impl;

    /** Whole-machine CPU. Deltas are against this instance's previous call, so
     *  the caller's sampling interval is the interval. Budget: 5 ms. */
    int (*cpu)(vs_sensors_system *self, vs_cpu_sample *out);
    int (*memory)(vs_sensors_system *self, vs_memory_sample *out);

    /** Every interface `/proc/net/dev` lists, filtered by nothing: the panel
     *  decides what to show, and `kind` gives it the same answer
     *  `MetricFormat.includeNetworkInterface` gives on macOS. */
    int (*network)(vs_sensors_system *self, vs_network_sample **out, size_t *count_out);
    void (*free_network)(vs_sensors_system *self, vs_network_sample *samples, size_t count);

    int (*disk_devices)(vs_sensors_system *self, vs_disk_device_sample **out, size_t *count_out);
    void (*free_disk_devices)(vs_sensors_system *self, vs_disk_device_sample *samples, size_t count);
    /** Mounted real filesystems only; pseudo filesystems (proc, sysfs, cgroup,
     *  tmpfs, ...) are skipped. */
    int (*disk_mounts)(vs_sensors_system *self, vs_disk_mount_sample **out, size_t *count_out);
    void (*free_disk_mounts)(vs_sensors_system *self, vs_disk_mount_sample *samples, size_t count);

    int (*temperatures)(vs_sensors_system *self, vs_temperature_sample **out, size_t *count_out);
    void (*free_temperatures)(vs_sensors_system *self, vs_temperature_sample *samples, size_t count);
    int (*fans)(vs_sensors_system *self, vs_fan_sample **out, size_t *count_out);
    void (*free_fans)(vs_sensors_system *self, vs_fan_sample *samples, size_t count);

    /** The machine's own power: internal battery and adapter. Returns
     *  VS_ERR_UNSUPPORTED when neither UPower nor power_supply is there. */
    int (*power)(vs_sensors_system *self, vs_power_sample *out);
    /** Batteries that are not the machine's own: mouse, keyboard, headset.
     *  UPower only; an empty list without it. */
    int (*peripheral_batteries)(vs_sensors_system *self, vs_battery_sample **out, size_t *count_out);
    void (*free_batteries)(vs_sensors_system *self, vs_battery_sample *samples, size_t count);

    int (*gpus)(vs_sensors_system *self, vs_gpu_sample **out, size_t *count_out);
    void (*free_gpus)(vs_sensors_system *self, vs_gpu_sample *samples, size_t count);

    /** Top processes by the query's sort key. Deltas are against this
     *  instance's previous `processes` call. */
    int (*processes)(vs_sensors_system *self, const vs_process_query *query,
                     vs_process_sample **out, size_t *count_out);
    void (*free_processes)(vs_sensors_system *self, vs_process_sample *samples, size_t count);

    /** Recomputed by `temperatures`; reading it does no I/O. */
    int (*thermal_pressure)(vs_sensors_system *self, vs_thermal_pressure *out);

    int (*set_event_callback)(vs_sensors_system *self, vs_sensors_event_cb callback, void *user_data);
    /** -1: this backend polls, so there is nothing to poll on. Events are
     *  produced by `dispatch` comparing the last computed state. */
    int (*event_fd)(vs_sensors_system *self);
    /** Deliver whatever changed since the last dispatch. Never blocks, starts
     *  no thread, and is the only place the callback runs. */
    int (*dispatch)(vs_sensors_system *self);

    void (*destroy)(vs_sensors_system *self);
};

/** Probe what this machine has and build the backend. `options` may be NULL
 *  for the defaults. Never fails on a machine with a `/proc`: a missing sensor
 *  is a clear capability bit, not a missing backend. Returns NULL only when
 *  `/proc/stat` cannot be read under `root`, with `*result_out` saying why. */
vs_sensors_system *vs_sensors_system_create(const vs_sensors_options *options, int *result_out);

const char *vs_sensor_kind_string(vs_sensor_kind kind);
const char *vs_battery_state_string(vs_battery_state state);
const char *vs_battery_kind_string(vs_battery_kind kind);
const char *vs_network_kind_string(vs_network_kind kind);
const char *vs_thermal_pressure_string(vs_thermal_pressure pressure);
const char *vs_gpu_vendor_string(vs_gpu_vendor vendor);

#ifdef __cplusplus
}
#endif

#endif /* VORSSAINT_PLATFORM_H */
