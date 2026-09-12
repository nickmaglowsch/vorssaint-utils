/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * The C contract between the Linux platform backends (linux/platform) and the
 * Swift Platform protocols (Sources/VorssaintCore/Platform, WP-12). One struct
 * of function pointers per concern; Sources/VorssaintLinux wraps this table and
 * nothing else.
 *
 * The window (WP-C1) and capture (WP-B1) sections exist so far. Clipboard,
 * audio, sensors, power, input and session sections are added by their own
 * work packages, each as another vtable or handle type in this header.
 *
 * Sections are separated by a banner comment and are independent: a consumer
 * that needs only one links only that library (`vs_window`, `vs_capture`).
 *
 * Conventions every section follows:
 *   - Every call returns `int`: 0 (VS_OK) or a negative `vs_result`.
 *   - Every vtable carries `capabilities`, a bitmask the Swift side surfaces so
 *     a feature can degrade honestly instead of failing silently.
 *   - Nothing in this header allocates with anything but malloc/free; every
 *     `*_out` array returned by a backend is released by its `free_*` member.
 *   - No call blocks for longer than its documented budget.
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
    /** The backend lost its connection; the caller must recreate it. */
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
    /** Bitmask of `vs_window_capability`, fixed for the life of the instance.
     *  A member whose capability bit is clear still exists and returns
     *  VS_ERR_UNSUPPORTED. */
    uint32_t capabilities;
    /** Backend-private state. */
    void *impl;

    /** Snapshot of every toplevel, bottom-most first. The caller owns the array
     *  until it passes it to `free_list`. Budget: 100 ms. */
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

    /** Install the event sink. Pass NULL to remove it. Events are delivered
     *  from `dispatch`, never from another thread. */
    int (*set_event_callback)(vs_window_system *self, vs_window_event_cb callback, void *user_data);
    /** Pollable descriptor that becomes readable when events are pending, or -1
     *  when the backend has none (`VS_WINDOW_HAS_LIVE_EVENTS` clear). */
    int (*event_fd)(vs_window_system *self);
    /** Drain what is pending and deliver it to the callback. Never blocks.
     *  Returns the number of events delivered, or a negative `vs_result`. */
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
 *     its own chooser; the engine reports what came back and never pretends it
 *     asked for something else. `vs_capture_stream_source_type()` is the type
 *     the session actually granted, which is not always the type requested.
 *   - The portal has no region source. `region` in the stream config is a crop
 *     the engine performs, and `VS_CAPTURE_HAS_REGION` is therefore always
 *     clear: it reports the portal's capability, not ours.
 *   - Frame delivery is damage-driven on wlroots. `max_fps` is a ceiling, not
 *     a cadence, and a still screen delivers nothing at all. Timelines come
 *     from `pts_ns`, never from a frame counter times a nominal rate.
 */

typedef struct vs_capture_engine vs_capture_engine;
typedef struct vs_capture_stream vs_capture_stream;

/** Capabilities of a capture backend. A feature reads these and hides or
 *  explains what the running session cannot do; see FEATURE_TRIAGE.md. */
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
    /** Refresh rate in mHz (60000 == 60 Hz), 0 when unknown. */
    int32_t refresh_mhz;
    /** Fractional scale, 0 when unknown. */
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

/** A still image the caller owns. `data` is `height * stride` bytes; `stride`
 *  may exceed `width` times bytes-per-pixel when it came straight off a
 *  capture buffer, so never assume the two are equal. */
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

/** One delivered frame. Every pointer is valid only for the duration of the
 *  callback, or until `vs_capture_stream_release_frame` for the pull API. */
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
 *  was padded. */
int vs_capture_image_from_frame(const uint8_t *data, uint32_t width, uint32_t height,
                                uint32_t stride, vs_capture_pixel_format format,
                                const vs_rect *region, vs_capture_image *image_out);

typedef enum vs_capture_audio_kind {
    VS_CAPTURE_AUDIO_SYSTEM = 1,
    VS_CAPTURE_AUDIO_MICROPHONE = 2,
} vs_capture_audio_kind;

/** One delivered PCM buffer, interleaved 32-bit float, on the same timeline as
 *  the frames. WP-B5 feeds this straight to its encoder. */
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
 *  a member later never changes an existing caller's meaning. */
typedef struct vs_capture_stream_config {
    /** Bits of `vs_capture_source_type` to ask for. The engine still checks
     *  what was granted and reports it. */
    uint32_t source_types;
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
    /** Frames the pull API may hold; 0 means 4. Ignored for a kind that has a
     *  callback installed, which is zero-copy and never queues. */
    uint32_t queue_depth;
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

/** Delivered from the engine's own PipeWire thread, never from the caller's.
 *  The frame is valid for the duration of the call only. */
typedef void (*vs_capture_frame_cb)(const vs_capture_frame *frame, void *user_data);
typedef void (*vs_capture_audio_cb)(const vs_capture_audio_buffer *buffer, void *user_data);

typedef struct vs_capture_stats {
    uint64_t frames_delivered;
    uint64_t frames_with_pts;
    /** Dropped by the engine's own frame-rate cap. */
    uint64_t frames_dropped_rate;
    /** Arrived while paused. */
    uint64_t frames_dropped_paused;
    /** The pull queue was full and the caller had not drained it. */
    uint64_t frames_dropped_queue;
    /** `pw_stream_dequeue_buffer` returned NULL: PipeWire itself had none. */
    uint64_t buffers_missed;
    uint64_t audio_buffers_system;
    uint64_t audio_frames_system;
    uint64_t audio_buffers_microphone;
    uint64_t audio_frames_microphone;
    uint64_t audio_dropped_paused;
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

/** Build the capture engine for this session. `preferred` names a backend
 *  ("portal" is the only one so far) or is NULL to probe. Probing needs a
 *  session bus carrying `org.freedesktop.portal.Desktop`. Returns NULL and
 *  sets `*result_out` (optional) when nothing is usable. */
vs_capture_engine *vs_capture_engine_create(const char *preferred, int *result_out);
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
     *  so this is a real timeout, not a formality. */
    uint32_t timeout_ms;
} vs_capture_shot_request;

/** One frame of whatever the session grants. `image_out` is filled on success
 *  and released with `vs_capture_image_free`. `method_used_out` (optional) says
 *  which path answered, and `token_out` (optional) receives a malloc'd restore
 *  token the caller must free, or NULL when none was minted. */
int vs_capture_screenshot(vs_capture_engine *engine,
                          const vs_capture_shot_request *request,
                          vs_capture_image *image_out,
                          vs_capture_shot_method *method_used_out,
                          char **token_out);

/** Negotiate a session and start delivering. On success the stream is running:
 *  frames reach the callback if one is installed before the first frame, and
 *  are queued for `vs_capture_stream_next_frame` otherwise. */
int vs_capture_stream_start(vs_capture_engine *engine,
                            const vs_capture_stream_config *config,
                            vs_capture_stream **stream_out);
/** Stop delivering, close the portal session and release the stream. Blocks
 *  until no callback is running, so the caller's encoder can finalise knowing
 *  nothing is in flight. */
void vs_capture_stream_stop(vs_capture_stream *stream);

/** Install or remove (NULL) the sinks. Callbacks run on the engine's PipeWire
 *  thread; installing one turns off the pull queue for that kind. */
void vs_capture_stream_set_frame_callback(vs_capture_stream *stream,
                                          vs_capture_frame_cb callback, void *user_data);
void vs_capture_stream_set_audio_callback(vs_capture_stream *stream,
                                          vs_capture_audio_cb callback, void *user_data);

/** Pull API. Waits up to `timeout_ms` (0 polls, negative waits forever) for a
 *  frame. Returns VS_OK with `frame_out` filled, or VS_ERR_TIMEOUT when nothing
 *  arrived. Every VS_OK must be matched by `vs_capture_stream_release_frame`
 *  before the next call. */
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
 *  window" feature must check: xdg-desktop-portal-wlr 0.7.1 answers a WINDOW
 *  request with a MONITOR stream. */
uint32_t vs_capture_stream_source_type(const vs_capture_stream *stream);
/** True when the granted type is not among the requested ones. */
bool vs_capture_stream_source_mismatch(const vs_capture_stream *stream);
/** Negotiated frame geometry, before the engine's crop. */
int vs_capture_stream_size(const vs_capture_stream *stream,
                           uint32_t *width_out, uint32_t *height_out);
vs_capture_pixel_format vs_capture_stream_format(const vs_capture_stream *stream);
/** Token minted by this session, or NULL. The consumer persists it and hands
 *  it back in `vs_capture_stream_config.restore_token`; the engine stores
 *  nothing itself. Valid until the stream is stopped. */
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

#ifdef __cplusplus
}
#endif

#endif /* VORSSAINT_PLATFORM_H */
