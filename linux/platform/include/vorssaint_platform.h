/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * The C contract between the Linux platform backends (linux/platform) and the
 * Swift Platform protocols (Sources/VorssaintCore/Platform, WP-12). One struct
 * of function pointers per concern; Sources/VorssaintLinux wraps this table and
 * nothing else.
 *
 * Only the window section exists so far (WP-C1). Clipboard, capture, audio,
 * sensors, power, input and session sections are added by their own work
 * packages, each as another `vs_<concern>_system` vtable in this header.
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
 * and may be used from different threads.
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

#ifdef __cplusplus
}
#endif

#endif /* VORSSAINT_PLATFORM_H */
