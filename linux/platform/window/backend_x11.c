/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * X11 backend, EWMH over libxcb. Works with any window manager that claims
 * EWMH compliance through _NET_SUPPORTING_WM_CHECK; tested against openbox.
 *
 * Geometry is frame geometry, matching what the macOS Accessibility API
 * reports: the client rectangle grown by _NET_FRAME_EXTENTS. `move_resize`
 * converts back to a client rectangle and asks with StaticGravity so the
 * request means exactly what it reads back.
 */

#include "vs_window_internal.h"

#include <time.h>

#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>

#define X11_ICONIC_STATE 3
/* EWMH source indication: 2 = pager/taskbar, which is what we are. */
#define X11_SOURCE_PAGER 2

typedef struct {
    xcb_connection_t *connection;
    xcb_ewmh_connection_t ewmh;
    int screen;
    xcb_window_t root;

    xcb_atom_t atom_wm_change_state;
    xcb_atom_t atom_net_wm_window_type_normal;
    xcb_atom_t atom_net_wm_state_skip_taskbar;

    vs_window_event_cb callback;
    void *callback_data;

    xcb_window_t *tracked;
    size_t tracked_count;
    xcb_window_t last_active;
    int32_t last_workspace;
} x11_state;

/* -------------------------------------------------------------- utilities */

static xcb_atom_t x11_intern(xcb_connection_t *connection, const char *name)
{
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(connection, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(connection, cookie, NULL);
    xcb_atom_t atom = reply ? reply->atom : XCB_ATOM_NONE;
    free(reply);
    return atom;
}

static bool x11_has_window_manager(x11_state *state)
{
    xcb_window_t owner = XCB_WINDOW_NONE;
    xcb_get_property_cookie_t cookie = xcb_ewmh_get_supporting_wm_check(&state->ewmh, state->root);
    if (!xcb_ewmh_get_supporting_wm_check_reply(&state->ewmh, cookie, &owner, NULL)) return false;
    return owner != XCB_WINDOW_NONE;
}

static bool x11_client_list(x11_state *state, xcb_window_t **windows_out, uint32_t *count_out,
                            bool *stacking_out)
{
    xcb_ewmh_get_windows_reply_t reply;
    bool stacking = true;
    xcb_get_property_cookie_t cookie =
        xcb_ewmh_get_client_list_stacking(&state->ewmh, state->screen);
    if (!xcb_ewmh_get_client_list_stacking_reply(&state->ewmh, cookie, &reply, NULL)) {
        stacking = false;
        cookie = xcb_ewmh_get_client_list(&state->ewmh, state->screen);
        if (!xcb_ewmh_get_client_list_reply(&state->ewmh, cookie, &reply, NULL)) return false;
    }

    xcb_window_t *windows = NULL;
    if (reply.windows_len > 0) {
        windows = malloc(reply.windows_len * sizeof(*windows));
        if (!windows) {
            xcb_ewmh_get_windows_reply_wipe(&reply);
            return false;
        }
        memcpy(windows, reply.windows, reply.windows_len * sizeof(*windows));
    }
    *windows_out = windows;
    *count_out = reply.windows_len;
    if (stacking_out) *stacking_out = stacking;
    xcb_ewmh_get_windows_reply_wipe(&reply);
    return true;
}

static void x11_read_title(x11_state *state, xcb_window_t window, vs_window_info *info)
{
    xcb_ewmh_get_utf8_strings_reply_t utf8;
    xcb_get_property_cookie_t cookie = xcb_ewmh_get_wm_name(&state->ewmh, window);
    if (xcb_ewmh_get_wm_name_reply(&state->ewmh, cookie, &utf8, NULL)) {
        size_t length = utf8.strings_len;
        if (length >= VS_WINDOW_TITLE_MAX) length = VS_WINDOW_TITLE_MAX - 1;
        memcpy(info->title, utf8.strings, length);
        info->title[length] = '\0';
        xcb_ewmh_get_utf8_strings_reply_wipe(&utf8);
        if (info->title[0]) return;
    }

    xcb_icccm_get_text_property_reply_t text;
    xcb_get_property_cookie_t legacy = xcb_icccm_get_wm_name(state->connection, window);
    if (xcb_icccm_get_wm_name_reply(state->connection, legacy, &text, NULL)) {
        size_t length = text.name_len;
        if (length >= VS_WINDOW_TITLE_MAX) length = VS_WINDOW_TITLE_MAX - 1;
        memcpy(info->title, text.name, length);
        info->title[length] = '\0';
        xcb_icccm_get_text_property_reply_wipe(&text);
    }
}

static void x11_read_class(x11_state *state, xcb_window_t window, vs_window_info *info)
{
    xcb_icccm_get_wm_class_reply_t wm_class;
    xcb_get_property_cookie_t cookie = xcb_icccm_get_wm_class(state->connection, window);
    if (!xcb_icccm_get_wm_class_reply(state->connection, cookie, &wm_class, NULL)) return;
    vs_window_set_string(info->app_id, sizeof(info->app_id), wm_class.instance_name);
    vs_window_set_string(info->app_name, sizeof(info->app_name), wm_class.class_name);
    xcb_icccm_get_wm_class_reply_wipe(&wm_class);
}

/** True when the window is one a user would switch to: normal type (or no type
 *  at all) and not asking to be kept out of taskbars. */
static bool x11_is_switchable(x11_state *state, xcb_window_t window)
{
    bool switchable = true;
    xcb_ewmh_get_atoms_reply_t types;
    xcb_get_property_cookie_t cookie = xcb_ewmh_get_wm_window_type(&state->ewmh, window);
    if (xcb_ewmh_get_wm_window_type_reply(&state->ewmh, cookie, &types, NULL)) {
        switchable = false;
        for (uint32_t i = 0; i < types.atoms_len; i++) {
            if (types.atoms[i] == state->atom_net_wm_window_type_normal) switchable = true;
        }
        xcb_ewmh_get_atoms_reply_wipe(&types);
    }
    if (!switchable) return false;

    xcb_ewmh_get_atoms_reply_t states;
    cookie = xcb_ewmh_get_wm_state(&state->ewmh, window);
    if (xcb_ewmh_get_wm_state_reply(&state->ewmh, cookie, &states, NULL)) {
        for (uint32_t i = 0; i < states.atoms_len; i++) {
            if (states.atoms[i] == state->atom_net_wm_state_skip_taskbar) switchable = false;
        }
        xcb_ewmh_get_atoms_reply_wipe(&states);
    }
    return switchable;
}

static void x11_read_state_flags(x11_state *state, xcb_window_t window, vs_window_info *info)
{
    xcb_ewmh_get_atoms_reply_t states;
    xcb_get_property_cookie_t cookie = xcb_ewmh_get_wm_state(&state->ewmh, window);
    if (!xcb_ewmh_get_wm_state_reply(&state->ewmh, cookie, &states, NULL)) return;
    bool maximized_horizontally = false;
    bool maximized_vertically = false;
    for (uint32_t i = 0; i < states.atoms_len; i++) {
        xcb_atom_t atom = states.atoms[i];
        if (atom == state->ewmh._NET_WM_STATE_HIDDEN) info->flags |= VS_WINDOW_MINIMIZED;
        else if (atom == state->ewmh._NET_WM_STATE_FULLSCREEN) info->flags |= VS_WINDOW_FULLSCREEN;
        else if (atom == state->ewmh._NET_WM_STATE_MAXIMIZED_HORZ) maximized_horizontally = true;
        else if (atom == state->ewmh._NET_WM_STATE_MAXIMIZED_VERT) maximized_vertically = true;
    }
    if (maximized_horizontally && maximized_vertically) info->flags |= VS_WINDOW_MAXIMIZED;
    xcb_ewmh_get_atoms_reply_wipe(&states);
}

static bool x11_frame_extents(x11_state *state, xcb_window_t window, xcb_ewmh_get_extents_reply_t *out)
{
    xcb_get_property_cookie_t cookie = xcb_ewmh_get_frame_extents(&state->ewmh, window);
    return xcb_ewmh_get_frame_extents_reply(&state->ewmh, cookie, out, NULL);
}

/** Frame rectangle in root coordinates: the client rectangle grown by the
 *  window manager's decorations. */
static bool x11_frame_geometry(x11_state *state, xcb_window_t window, vs_rect *out)
{
    xcb_get_geometry_cookie_t geometry_cookie = xcb_get_geometry(state->connection, window);
    xcb_get_geometry_reply_t *geometry =
        xcb_get_geometry_reply(state->connection, geometry_cookie, NULL);
    if (!geometry) return false;

    xcb_translate_coordinates_cookie_t translate_cookie =
        xcb_translate_coordinates(state->connection, window, state->root, 0, 0);
    xcb_translate_coordinates_reply_t *translate =
        xcb_translate_coordinates_reply(state->connection, translate_cookie, NULL);
    if (!translate) {
        free(geometry);
        return false;
    }

    vs_rect frame = {
        .x = translate->dst_x,
        .y = translate->dst_y,
        .width = geometry->width,
        .height = geometry->height,
    };
    free(translate);
    free(geometry);

    xcb_ewmh_get_extents_reply_t extents;
    if (x11_frame_extents(state, window, &extents)) {
        frame.x -= (int32_t)extents.left;
        frame.y -= (int32_t)extents.top;
        frame.width += (int32_t)(extents.left + extents.right);
        frame.height += (int32_t)(extents.top + extents.bottom);
    }
    *out = frame;
    return true;
}

static bool x11_fill(x11_state *state, xcb_window_t window, xcb_window_t active,
                     int32_t current_desktop, vs_window_info *info)
{
    vs_window_info_init(info);
    info->id = window;

    x11_read_title(state, window, info);
    x11_read_class(state, window, info);
    x11_read_state_flags(state, window, info);

    uint32_t pid = 0;
    xcb_get_property_cookie_t pid_cookie = xcb_ewmh_get_wm_pid(&state->ewmh, window);
    if (xcb_ewmh_get_wm_pid_reply(&state->ewmh, pid_cookie, &pid, NULL)) {
        info->pid = (int32_t)pid;
        info->flags |= VS_WINDOW_HAS_PID;
    }

    uint32_t desktop = 0;
    xcb_get_property_cookie_t desktop_cookie = xcb_ewmh_get_wm_desktop(&state->ewmh, window);
    if (xcb_ewmh_get_wm_desktop_reply(&state->ewmh, desktop_cookie, &desktop, NULL)) {
        /* 0xFFFFFFFF means "on every desktop". */
        info->workspace = desktop == 0xFFFFFFFFu ? -1 : (int32_t)desktop;
        if (desktop == 0xFFFFFFFFu || (current_desktop >= 0 && info->workspace == current_desktop)) {
            info->flags |= VS_WINDOW_ON_CURRENT_WORKSPACE;
        }
    } else {
        info->flags |= VS_WINDOW_ON_CURRENT_WORKSPACE;
    }

    vs_rect frame;
    if (x11_frame_geometry(state, window, &frame)) {
        info->frame = frame;
        info->flags |= VS_WINDOW_HAS_GEOMETRY;
    }

    if (window == active) info->flags |= VS_WINDOW_FOCUSED;
    if (!(info->flags & VS_WINDOW_MINIMIZED) && (info->flags & VS_WINDOW_ON_CURRENT_WORKSPACE)) {
        info->flags |= VS_WINDOW_ON_SCREEN;
    }
    if (!info->app_name[0]) vs_window_set_string(info->app_name, sizeof(info->app_name), info->app_id);
    return true;
}

static xcb_window_t x11_active_window(x11_state *state)
{
    xcb_window_t active = XCB_WINDOW_NONE;
    xcb_get_property_cookie_t cookie = xcb_ewmh_get_active_window(&state->ewmh, state->screen);
    xcb_ewmh_get_active_window_reply(&state->ewmh, cookie, &active, NULL);
    return active;
}

static int32_t x11_current_desktop_value(x11_state *state)
{
    uint32_t desktop = 0;
    xcb_get_property_cookie_t cookie = xcb_ewmh_get_current_desktop(&state->ewmh, state->screen);
    if (!xcb_ewmh_get_current_desktop_reply(&state->ewmh, cookie, &desktop, NULL)) return -1;
    return (int32_t)desktop;
}

/* ------------------------------------------------------------------- vtable */

static int x11_list(vs_window_system *self, vs_window_info **windows_out, size_t *count_out)
{
    x11_state *state = self->impl;
    xcb_window_t *windows = NULL;
    uint32_t count = 0;
    bool stacking = false;
    if (!x11_client_list(state, &windows, &count, &stacking)) return VS_ERR_BACKEND;

    xcb_window_t active = x11_active_window(state);
    int32_t desktop = x11_current_desktop_value(state);

    vs_window_vec vec = { 0 };
    for (uint32_t i = 0; i < count; i++) {
        if (!x11_is_switchable(state, windows[i])) continue;
        vs_window_info info;
        if (!x11_fill(state, windows[i], active, desktop, &info)) continue;
        info.stacking_index = (uint32_t)vec.count;
        info.stacking_valid = stacking;
        if (!vs_window_vec_push(&vec, &info)) {
            free(windows);
            vs_window_vec_free(&vec);
            return VS_ERR_NO_MEM;
        }
    }
    free(windows);

    *windows_out = vec.items;
    *count_out = vec.count;
    return VS_OK;
}

static int x11_send_root_message(x11_state *state, xcb_window_t window, xcb_atom_t type,
                                 const uint32_t data[5])
{
    xcb_client_message_event_t event = { 0 };
    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 32;
    event.window = window;
    event.type = type;
    for (int i = 0; i < 5; i++) event.data.data32[i] = data[i];

    xcb_void_cookie_t cookie = xcb_send_event_checked(
        state->connection, 0, state->root,
        XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
        (const char *)&event);
    xcb_generic_error_t *error = xcb_request_check(state->connection, cookie);
    if (error) {
        vs_window_log("x11: client message failed, code %u", error->error_code);
        free(error);
        return VS_ERR_BACKEND;
    }
    xcb_flush(state->connection);
    return VS_OK;
}

static bool x11_window_exists(x11_state *state, xcb_window_t window)
{
    xcb_get_geometry_cookie_t cookie = xcb_get_geometry(state->connection, window);
    xcb_get_geometry_reply_t *reply = xcb_get_geometry_reply(state->connection, cookie, NULL);
    if (!reply) return false;
    free(reply);
    return true;
}

static int x11_activate(vs_window_system *self, vs_window_id id)
{
    x11_state *state = self->impl;
    xcb_window_t window = (xcb_window_t)id;
    if (!x11_window_exists(state, window)) return VS_ERR_NOT_FOUND;

    /* A minimized window must be mapped again before the window manager will
     * treat _NET_ACTIVE_WINDOW as anything. */
    xcb_map_window(state->connection, window);

    const uint32_t data[5] = { X11_SOURCE_PAGER, XCB_CURRENT_TIME, 0, 0, 0 };
    return x11_send_root_message(state, window, state->ewmh._NET_ACTIVE_WINDOW, data);
}

static int x11_close(vs_window_system *self, vs_window_id id)
{
    x11_state *state = self->impl;
    xcb_window_t window = (xcb_window_t)id;
    if (!x11_window_exists(state, window)) return VS_ERR_NOT_FOUND;
    const uint32_t data[5] = { XCB_CURRENT_TIME, X11_SOURCE_PAGER, 0, 0, 0 };
    return x11_send_root_message(state, window, state->ewmh._NET_CLOSE_WINDOW, data);
}

static int x11_set_minimized(vs_window_system *self, vs_window_id id, bool minimized)
{
    x11_state *state = self->impl;
    xcb_window_t window = (xcb_window_t)id;
    if (!x11_window_exists(state, window)) return VS_ERR_NOT_FOUND;
    if (!minimized) return x11_activate(self, id);

    /* ICCCM 4.1.4: iconify by asking the window manager, not by unmapping. */
    const uint32_t data[5] = { X11_ICONIC_STATE, 0, 0, 0, 0 };
    return x11_send_root_message(state, window, state->atom_wm_change_state, data);
}

static int x11_geometry(vs_window_system *self, vs_window_id id, vs_rect *frame_out)
{
    x11_state *state = self->impl;
    if (!frame_out) return VS_ERR_INVALID;
    if (!x11_frame_geometry(state, (xcb_window_t)id, frame_out)) return VS_ERR_NOT_FOUND;
    return VS_OK;
}

static int x11_move_resize(vs_window_system *self, vs_window_id id, vs_rect frame, int32_t tolerance)
{
    x11_state *state = self->impl;
    xcb_window_t window = (xcb_window_t)id;
    if (frame.width <= 0 || frame.height <= 0) return VS_ERR_INVALID;
    if (!x11_window_exists(state, window)) return VS_ERR_NOT_FOUND;

    /* _NET_MOVERESIZE_WINDOW takes the frame's reference point for x,y under
     * the requested gravity and the *client* size for width,height, so the
     * decorations come off the size but not off the position. Gravity 0 leaves
     * the window's own gravity in force, which is what every EWMH window
     * manager anchors its frame with. */
    vs_rect client = frame;
    xcb_ewmh_get_extents_reply_t extents;
    if (x11_frame_extents(state, window, &extents)) {
        client.width -= (int32_t)(extents.left + extents.right);
        client.height -= (int32_t)(extents.top + extents.bottom);
        if (client.width <= 0 || client.height <= 0) return VS_ERR_INVALID;
    }

    const uint32_t flags = (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) |
                           ((uint32_t)X11_SOURCE_PAGER << 12);
    const uint32_t data[5] = { flags, (uint32_t)client.x, (uint32_t)client.y,
                               (uint32_t)client.width, (uint32_t)client.height };
    int result = x11_send_root_message(state, window, state->ewmh._NET_MOVERESIZE_WINDOW, data);
    if (result != VS_OK) return result;
    if (tolerance < 0) return VS_OK;

    /* Read back. A window manager acknowledges the message and then does what
     * it likes: tiling, size hints and maximized state all override us. */
    for (int attempt = 0; attempt < 20; attempt++) {
        xcb_flush(state->connection);
        vs_rect actual;
        if (x11_frame_geometry(state, window, &actual) && vs_rect_close(actual, frame, tolerance)) {
            return VS_OK;
        }
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 25 * 1000 * 1000 };
        nanosleep(&pause, NULL);
    }
    vs_window_log("x11: move_resize did not settle within tolerance %d", tolerance);
    return VS_ERR_NOT_APPLIED;
}

static int x11_current_workspace(vs_window_system *self, int32_t *workspace_out)
{
    x11_state *state = self->impl;
    if (!workspace_out) return VS_ERR_INVALID;
    int32_t desktop = x11_current_desktop_value(state);
    if (desktop < 0) return VS_ERR_UNSUPPORTED;
    *workspace_out = desktop;
    return VS_OK;
}

static int x11_set_workspace(vs_window_system *self, int32_t workspace)
{
    x11_state *state = self->impl;
    if (workspace < 0) return VS_ERR_INVALID;
    const uint32_t data[5] = { (uint32_t)workspace, XCB_CURRENT_TIME, 0, 0, 0 };
    int result = x11_send_root_message(state, state->root, state->ewmh._NET_CURRENT_DESKTOP, data);
    if (result != VS_OK) return result;
    for (int attempt = 0; attempt < 20; attempt++) {
        if (x11_current_desktop_value(state) == workspace) return VS_OK;
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 25 * 1000 * 1000 };
        nanosleep(&pause, NULL);
    }
    return VS_ERR_NOT_APPLIED;
}

/* --------------------------------------------------------------- events */

static void x11_watch(x11_state *state, xcb_window_t window)
{
    const uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(state->connection, window, XCB_CW_EVENT_MASK, &mask);
}

static bool x11_tracked_contains(x11_state *state, xcb_window_t window)
{
    for (size_t i = 0; i < state->tracked_count; i++) {
        if (state->tracked[i] == window) return true;
    }
    return false;
}

static void x11_emit(x11_state *state, vs_window_event_type type, xcb_window_t window,
                     const vs_window_info *info, int32_t workspace)
{
    if (!state->callback) return;
    vs_window_event event = { .type = type, .id = window, .info = info, .workspace = workspace };
    state->callback(&event, state->callback_data);
}

/** Diff the client list and emit ADDED/REMOVED. The window manager rewrites
 *  _NET_CLIENT_LIST_STACKING for stacking changes too, so this runs often. */
static int x11_refresh_tracked(x11_state *state)
{
    xcb_window_t *windows = NULL;
    uint32_t count = 0;
    if (!x11_client_list(state, &windows, &count, NULL)) return 0;

    int emitted = 0;
    xcb_window_t active = x11_active_window(state);
    int32_t desktop = x11_current_desktop_value(state);

    for (uint32_t i = 0; i < count; i++) {
        if (x11_tracked_contains(state, windows[i])) continue;
        if (!x11_is_switchable(state, windows[i])) continue;
        x11_watch(state, windows[i]);
        vs_window_info info;
        if (x11_fill(state, windows[i], active, desktop, &info)) {
            x11_emit(state, VS_WINDOW_EVENT_ADDED, windows[i], &info, -1);
            emitted++;
        }
    }
    for (size_t i = 0; i < state->tracked_count; i++) {
        bool present = false;
        for (uint32_t j = 0; j < count; j++) {
            if (state->tracked[i] == windows[j]) { present = true; break; }
        }
        if (present) continue;
        x11_emit(state, VS_WINDOW_EVENT_REMOVED, state->tracked[i], NULL, -1);
        emitted++;
    }

    xcb_window_t *tracked = NULL;
    size_t tracked_count = 0;
    if (count > 0) {
        tracked = malloc(count * sizeof(*tracked));
        if (tracked) {
            for (uint32_t i = 0; i < count; i++) {
                if (!x11_is_switchable(state, windows[i])) continue;
                tracked[tracked_count++] = windows[i];
            }
        }
    }
    free(state->tracked);
    state->tracked = tracked;
    state->tracked_count = tracked_count;
    free(windows);
    xcb_flush(state->connection);
    return emitted;
}

static int x11_dispatch(vs_window_system *self)
{
    x11_state *state = self->impl;
    int emitted = 0;
    xcb_generic_event_t *event;
    bool client_list_dirty = false;

    while ((event = xcb_poll_for_event(state->connection)) != NULL) {
        uint8_t type = event->response_type & 0x7f;
        if (type == XCB_PROPERTY_NOTIFY) {
            xcb_property_notify_event_t *property = (xcb_property_notify_event_t *)event;
            if (property->window == state->root) {
                if (property->atom == state->ewmh._NET_CLIENT_LIST_STACKING ||
                    property->atom == state->ewmh._NET_CLIENT_LIST) {
                    client_list_dirty = true;
                } else if (property->atom == state->ewmh._NET_ACTIVE_WINDOW) {
                    xcb_window_t active = x11_active_window(state);
                    if (active != state->last_active) {
                        state->last_active = active;
                        vs_window_info info;
                        bool filled = active != XCB_WINDOW_NONE &&
                                      x11_fill(state, active, active,
                                               x11_current_desktop_value(state), &info);
                        x11_emit(state, VS_WINDOW_EVENT_ACTIVATED, active, filled ? &info : NULL, -1);
                        emitted++;
                    }
                } else if (property->atom == state->ewmh._NET_CURRENT_DESKTOP) {
                    int32_t desktop = x11_current_desktop_value(state);
                    if (desktop != state->last_workspace) {
                        state->last_workspace = desktop;
                        x11_emit(state, VS_WINDOW_EVENT_WORKSPACE_CHANGED, 0, NULL, desktop);
                        emitted++;
                    }
                }
            } else if (property->atom == state->ewmh._NET_WM_NAME ||
                       property->atom == XCB_ATOM_WM_NAME ||
                       property->atom == state->ewmh._NET_WM_STATE ||
                       property->atom == state->ewmh._NET_WM_DESKTOP) {
                vs_window_info info;
                if (x11_fill(state, property->window, x11_active_window(state),
                             x11_current_desktop_value(state), &info)) {
                    x11_emit(state, VS_WINDOW_EVENT_CHANGED, property->window, &info, -1);
                    emitted++;
                }
            }
        } else if (type == XCB_CONFIGURE_NOTIFY) {
            xcb_configure_notify_event_t *configure = (xcb_configure_notify_event_t *)event;
            if (x11_tracked_contains(state, configure->window)) {
                vs_window_info info;
                if (x11_fill(state, configure->window, x11_active_window(state),
                             x11_current_desktop_value(state), &info)) {
                    x11_emit(state, VS_WINDOW_EVENT_CHANGED, configure->window, &info, -1);
                    emitted++;
                }
            }
        } else if (type == XCB_DESTROY_NOTIFY) {
            client_list_dirty = true;
        } else if (type == 0) {
            /* An error reply for a window that died between our request and the
             * server's answer; the client list diff cleans up after it. */
            client_list_dirty = true;
        }
        free(event);
    }

    if (xcb_connection_has_error(state->connection)) {
        x11_emit(state, VS_WINDOW_EVENT_BACKEND_LOST, 0, NULL, -1);
        return VS_ERR_BACKEND;
    }
    if (client_list_dirty) emitted += x11_refresh_tracked(state);
    return emitted;
}

static int x11_set_event_callback(vs_window_system *self, vs_window_event_cb callback, void *user_data)
{
    x11_state *state = self->impl;
    state->callback = callback;
    state->callback_data = user_data;
    if (callback) {
        state->last_active = x11_active_window(state);
        state->last_workspace = x11_current_desktop_value(state);
        /* Adopt the current windows silently: the caller has just listed them. */
        xcb_window_t *windows = NULL;
        uint32_t count = 0;
        if (x11_client_list(state, &windows, &count, NULL)) {
            free(state->tracked);
            state->tracked = NULL;
            state->tracked_count = 0;
            if (count > 0) {
                state->tracked = malloc(count * sizeof(*state->tracked));
                if (state->tracked) {
                    for (uint32_t i = 0; i < count; i++) {
                        if (!x11_is_switchable(state, windows[i])) continue;
                        x11_watch(state, windows[i]);
                        state->tracked[state->tracked_count++] = windows[i];
                    }
                }
            }
            free(windows);
            xcb_flush(state->connection);
        }
    }
    return VS_OK;
}

static int x11_event_fd(vs_window_system *self)
{
    x11_state *state = self->impl;
    return xcb_get_file_descriptor(state->connection);
}

static void x11_destroy(vs_window_system *self)
{
    x11_state *state = self->impl;
    if (state) {
        xcb_ewmh_connection_wipe(&state->ewmh);
        if (state->connection) xcb_disconnect(state->connection);
        free(state->tracked);
        free(state);
    }
    free(self);
}

vs_window_system *vs_window_backend_x11_create(int *result_out)
{
    if (result_out) *result_out = VS_ERR_NO_BACKEND;
    const char *display = getenv("DISPLAY");
    if (!display || !*display) return NULL;

    x11_state *state = calloc(1, sizeof(*state));
    if (!state) {
        if (result_out) *result_out = VS_ERR_NO_MEM;
        return NULL;
    }
    state->last_workspace = -1;

    state->connection = xcb_connect(NULL, &state->screen);
    if (!state->connection || xcb_connection_has_error(state->connection)) {
        if (state->connection) xcb_disconnect(state->connection);
        free(state);
        if (result_out) *result_out = VS_ERR_BACKEND;
        return NULL;
    }

    xcb_intern_atom_cookie_t *cookies = xcb_ewmh_init_atoms(state->connection, &state->ewmh);
    if (!cookies || !xcb_ewmh_init_atoms_replies(&state->ewmh, cookies, NULL)) {
        xcb_disconnect(state->connection);
        free(state);
        if (result_out) *result_out = VS_ERR_BACKEND;
        return NULL;
    }

    const xcb_setup_t *setup = xcb_get_setup(state->connection);
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < state->screen && screens.rem; i++) xcb_screen_next(&screens);
    state->root = screens.data->root;

    if (!x11_has_window_manager(state)) {
        vs_window_log("x11: no EWMH window manager on %s", display);
        xcb_ewmh_connection_wipe(&state->ewmh);
        xcb_disconnect(state->connection);
        free(state);
        if (result_out) *result_out = VS_ERR_NO_BACKEND;
        return NULL;
    }

    state->atom_wm_change_state = x11_intern(state->connection, "WM_CHANGE_STATE");
    state->atom_net_wm_window_type_normal = state->ewmh._NET_WM_WINDOW_TYPE_NORMAL;
    state->atom_net_wm_state_skip_taskbar = state->ewmh._NET_WM_STATE_SKIP_TASKBAR;

    const uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
    xcb_change_window_attributes(state->connection, state->root, XCB_CW_EVENT_MASK, &mask);
    xcb_flush(state->connection);

    vs_window_system *system = calloc(1, sizeof(*system));
    if (!system) {
        xcb_ewmh_connection_wipe(&state->ewmh);
        xcb_disconnect(state->connection);
        free(state);
        if (result_out) *result_out = VS_ERR_NO_MEM;
        return NULL;
    }

    system->name = "x11";
    system->impl = state;
    system->capabilities = VS_WINDOW_CAN_LIST | VS_WINDOW_CAN_ACTIVATE | VS_WINDOW_CAN_CLOSE |
                           VS_WINDOW_CAN_MINIMIZE | VS_WINDOW_CAN_MOVE_RESIZE |
                           VS_WINDOW_CAN_WORKSPACE_SWITCH | VS_WINDOW_HAS_LIVE_EVENTS;
    system->list = x11_list;
    system->free_list = vs_window_free_list_default;
    system->activate = x11_activate;
    system->close = x11_close;
    system->set_minimized = x11_set_minimized;
    system->move_resize = x11_move_resize;
    system->geometry = x11_geometry;
    system->current_workspace = x11_current_workspace;
    system->set_workspace = x11_set_workspace;
    system->set_event_callback = x11_set_event_callback;
    system->event_fd = x11_event_fd;
    system->dispatch = x11_dispatch;
    system->destroy = x11_destroy;

    if (result_out) *result_out = VS_OK;
    return system;
}
