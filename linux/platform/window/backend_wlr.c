/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * wlroots foreign-toplevel backend.
 *
 * `zwlr_foreign_toplevel_management_v1` is the one Wayland protocol that lets
 * an ordinary client list, activate, minimize and close other clients'
 * windows. `ext_foreign_toplevel_list_v1` is its successor for listing only
 * (no control verbs at all), so it is bound when the compositor offers it and
 * used purely to attach the stable per-toplevel identifier that WP-C3 will key
 * previews on.
 *
 * Neither protocol reports geometry, pid or workspace, and no Wayland protocol
 * lets one client move another client's window. On sway the i3 IPC socket
 * supplies all four; without it this backend advertises no move/resize.
 */

#include "vs_window_internal.h"
#include "sway_ipc.h"

#include <poll.h>
#include <time.h>

#include <wayland-client.h>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#define WLR_MANAGER_VERSION 3

typedef struct wlr_toplevel {
    struct wlr_toplevel *next;
    vs_window_id id;
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    char app_id[VS_WINDOW_APP_ID_MAX];
    char title[VS_WINDOW_TITLE_MAX];
    char output[VS_WINDOW_OUTPUT_MAX];
    /** `ext_foreign_toplevel_list_v1` identifier, when the compositor has one. */
    char identifier[64];
    uint32_t flags;
    bool announced;
    bool closed;
    struct wlr_state *state;
} wlr_toplevel;

typedef struct {
    vs_window_event_type type;
    vs_window_id id;
    bool has_info;
    vs_window_info info;
    int32_t workspace;
} wlr_pending_event;

typedef struct wlr_state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct zwlr_foreign_toplevel_manager_v1 *manager;
    struct ext_foreign_toplevel_list_v1 *ext_list;
    struct wl_seat *seat;
    uint32_t seat_name;

    wlr_toplevel *toplevels;
    vs_window_id next_id;

    wlr_pending_event *pending;
    size_t pending_count;
    size_t pending_capacity;

    vs_window_event_cb callback;
    void *callback_data;

    sway_ipc sway;
    bool has_sway;

    /* Outputs, so a toplevel's `output_enter` can be named. */
    struct wl_output **outputs;
    char (*output_names)[VS_WINDOW_OUTPUT_MAX];
    size_t output_count;
} wlr_state;

/* ------------------------------------------------------------ bookkeeping */

static void wlr_queue(wlr_state *state, vs_window_event_type type, vs_window_id id,
                      const vs_window_info *info, int32_t workspace)
{
    if (state->pending_count == state->pending_capacity) {
        size_t capacity = state->pending_capacity ? state->pending_capacity * 2 : 16;
        wlr_pending_event *pending = realloc(state->pending, capacity * sizeof(*pending));
        if (!pending) return;
        state->pending = pending;
        state->pending_capacity = capacity;
    }
    wlr_pending_event *event = &state->pending[state->pending_count++];
    event->type = type;
    event->id = id;
    event->has_info = info != NULL;
    if (info) event->info = *info;
    event->workspace = workspace;
}

static wlr_toplevel *wlr_find(wlr_state *state, vs_window_id id)
{
    for (wlr_toplevel *toplevel = state->toplevels; toplevel; toplevel = toplevel->next) {
        if (toplevel->id == id) return toplevel;
    }
    return NULL;
}

/** Fill the portable record from the protocol state alone, then let the sway
 *  IPC enrich it where it can. */
static void wlr_fill(wlr_state *state, const wlr_toplevel *toplevel, vs_window_info *info,
                     const sway_container *containers, size_t container_count,
                     int32_t current_workspace)
{
    vs_window_info_init(info);
    info->id = toplevel->id;
    vs_window_set_string(info->app_id, sizeof(info->app_id), toplevel->app_id);
    vs_window_set_string(info->app_name, sizeof(info->app_name), toplevel->app_id);
    vs_window_set_string(info->title, sizeof(info->title), toplevel->title);
    vs_window_set_string(info->output, sizeof(info->output), toplevel->output);
    info->flags = toplevel->flags | VS_WINDOW_ON_CURRENT_WORKSPACE;
    if (info->flags & VS_WINDOW_MINIMIZED) info->flags &= ~(uint32_t)VS_WINDOW_ON_SCREEN;
    else info->flags |= VS_WINDOW_ON_SCREEN;

    if (!containers) return;

    /* The protocol gives no handle the compositor's own id, so the only bridge
     * to the IPC tree is (app_id, title). Ambiguous pairs get no enrichment
     * rather than the wrong window's geometry. */
    const sway_container *match = NULL;
    size_t matches = 0;
    for (size_t i = 0; i < container_count; i++) {
        if (strcmp(containers[i].app_id, toplevel->app_id) != 0) continue;
        if (strcmp(containers[i].title, toplevel->title) != 0) continue;
        match = &containers[i];
        matches++;
    }
    if (matches != 1) {
        vs_window_log("wlr: %zu sway containers match %s / %s", matches, toplevel->app_id,
                      toplevel->title);
        return;
    }

    info->pid = match->pid;
    if (match->pid > 0) info->flags |= VS_WINDOW_HAS_PID;
    info->frame = match->frame;
    if (match->frame.width > 0 && match->frame.height > 0) info->flags |= VS_WINDOW_HAS_GEOMETRY;
    info->workspace = match->workspace;
    if (match->output[0]) vs_window_set_string(info->output, sizeof(info->output), match->output);
    if (match->fullscreen) info->flags |= VS_WINDOW_FULLSCREEN;
    if (current_workspace >= 0 && match->workspace >= 0 && match->workspace != current_workspace) {
        info->flags &= ~(uint32_t)VS_WINDOW_ON_CURRENT_WORKSPACE;
        info->flags &= ~(uint32_t)VS_WINDOW_ON_SCREEN;
    }
    (void)state;
}

/* ---------------------------------------------------- toplevel listeners */

static void wlr_handle_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                             const char *title)
{
    (void)handle;
    wlr_toplevel *toplevel = data;
    vs_window_set_string(toplevel->title, sizeof(toplevel->title), title);
}

static void wlr_handle_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                              const char *app_id)
{
    (void)handle;
    wlr_toplevel *toplevel = data;
    vs_window_set_string(toplevel->app_id, sizeof(toplevel->app_id), app_id);
}

static void wlr_handle_output_enter(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                                    struct wl_output *output)
{
    (void)handle;
    wlr_toplevel *toplevel = data;
    wlr_state *state = toplevel->state;
    for (size_t i = 0; i < state->output_count; i++) {
        if (state->outputs[i] != output) continue;
        vs_window_set_string(toplevel->output, sizeof(toplevel->output), state->output_names[i]);
        return;
    }
}

static void wlr_handle_output_leave(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                                    struct wl_output *output)
{
    (void)handle;
    (void)output;
    wlr_toplevel *toplevel = data;
    toplevel->output[0] = '\0';
}

static void wlr_handle_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                             struct wl_array *states)
{
    (void)handle;
    wlr_toplevel *toplevel = data;
    toplevel->flags &= ~(uint32_t)(VS_WINDOW_MINIMIZED | VS_WINDOW_MAXIMIZED |
                                   VS_WINDOW_FULLSCREEN | VS_WINDOW_FOCUSED);
    uint32_t *entry;
    wl_array_for_each(entry, states) {
        switch (*entry) {
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED:
            toplevel->flags |= VS_WINDOW_MINIMIZED;
            break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED:
            toplevel->flags |= VS_WINDOW_MAXIMIZED;
            break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN:
            toplevel->flags |= VS_WINDOW_FULLSCREEN;
            break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED:
            toplevel->flags |= VS_WINDOW_FOCUSED;
            break;
        default:
            break;
        }
    }
}

static void wlr_handle_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    (void)handle;
    wlr_toplevel *toplevel = data;
    wlr_state *state = toplevel->state;
    vs_window_info info;
    wlr_fill(state, toplevel, &info, NULL, 0, -1);
    if (!toplevel->announced) {
        toplevel->announced = true;
        wlr_queue(state, VS_WINDOW_EVENT_ADDED, toplevel->id, &info, -1);
    } else {
        wlr_queue(state, VS_WINDOW_EVENT_CHANGED, toplevel->id, &info, -1);
    }
    if (toplevel->flags & VS_WINDOW_FOCUSED) {
        wlr_queue(state, VS_WINDOW_EVENT_ACTIVATED, toplevel->id, &info, -1);
    }
}

static void wlr_handle_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    wlr_toplevel *toplevel = data;
    wlr_state *state = toplevel->state;
    toplevel->closed = true;
    wlr_queue(state, VS_WINDOW_EVENT_REMOVED, toplevel->id, NULL, -1);

    wlr_toplevel **link = &state->toplevels;
    while (*link && *link != toplevel) link = &(*link)->next;
    if (*link) *link = toplevel->next;
    zwlr_foreign_toplevel_handle_v1_destroy(handle);
    free(toplevel);
}

static void wlr_handle_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                              struct zwlr_foreign_toplevel_handle_v1 *parent)
{
    (void)data;
    (void)handle;
    (void)parent;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener wlr_handle_listener = {
    .title = wlr_handle_title,
    .app_id = wlr_handle_app_id,
    .output_enter = wlr_handle_output_enter,
    .output_leave = wlr_handle_output_leave,
    .state = wlr_handle_state,
    .done = wlr_handle_done,
    .closed = wlr_handle_closed,
    .parent = wlr_handle_parent,
};

static void wlr_manager_toplevel(void *data, struct zwlr_foreign_toplevel_manager_v1 *manager,
                                 struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    (void)manager;
    wlr_state *state = data;
    wlr_toplevel *toplevel = calloc(1, sizeof(*toplevel));
    if (!toplevel) return;
    toplevel->id = state->next_id++;
    toplevel->handle = handle;
    toplevel->state = state;
    toplevel->next = state->toplevels;
    state->toplevels = toplevel;
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &wlr_handle_listener, toplevel);
}

static void wlr_manager_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *manager)
{
    (void)manager;
    wlr_state *state = data;
    wlr_queue(state, VS_WINDOW_EVENT_BACKEND_LOST, 0, NULL, -1);
}

static const struct zwlr_foreign_toplevel_manager_v1_listener wlr_manager_listener = {
    .toplevel = wlr_manager_toplevel,
    .finished = wlr_manager_finished,
};

/* ------------------------------------------ ext-foreign-toplevel-list-v1 */

/* The ext protocol has no control verbs; it is bound only for its stable
 * per-toplevel identifier, which WP-C3 needs to address a window to the
 * ScreenCast portal across a restart. */

typedef struct {
    wlr_state *state;
    struct ext_foreign_toplevel_handle_v1 *handle;
    char app_id[VS_WINDOW_APP_ID_MAX];
    char title[VS_WINDOW_TITLE_MAX];
    char identifier[64];
} wlr_ext_toplevel;

static void wlr_ext_closed(void *data, struct ext_foreign_toplevel_handle_v1 *handle)
{
    wlr_ext_toplevel *entry = data;
    ext_foreign_toplevel_handle_v1_destroy(handle);
    free(entry);
}

static void wlr_ext_done(void *data, struct ext_foreign_toplevel_handle_v1 *handle)
{
    (void)handle;
    wlr_ext_toplevel *entry = data;
    for (wlr_toplevel *toplevel = entry->state->toplevels; toplevel; toplevel = toplevel->next) {
        if (strcmp(toplevel->app_id, entry->app_id) != 0) continue;
        if (strcmp(toplevel->title, entry->title) != 0) continue;
        vs_window_set_string(toplevel->identifier, sizeof(toplevel->identifier), entry->identifier);
    }
}

static void wlr_ext_title(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
                          const char *title)
{
    (void)handle;
    wlr_ext_toplevel *entry = data;
    vs_window_set_string(entry->title, sizeof(entry->title), title);
}

static void wlr_ext_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
                           const char *app_id)
{
    (void)handle;
    wlr_ext_toplevel *entry = data;
    vs_window_set_string(entry->app_id, sizeof(entry->app_id), app_id);
}

static void wlr_ext_identifier(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
                               const char *identifier)
{
    (void)handle;
    wlr_ext_toplevel *entry = data;
    vs_window_set_string(entry->identifier, sizeof(entry->identifier), identifier);
}

static const struct ext_foreign_toplevel_handle_v1_listener wlr_ext_handle_listener = {
    .closed = wlr_ext_closed,
    .done = wlr_ext_done,
    .title = wlr_ext_title,
    .app_id = wlr_ext_app_id,
    .identifier = wlr_ext_identifier,
};

static void wlr_ext_toplevel_added(void *data, struct ext_foreign_toplevel_list_v1 *list,
                                   struct ext_foreign_toplevel_handle_v1 *handle)
{
    (void)list;
    wlr_ext_toplevel *entry = calloc(1, sizeof(*entry));
    if (!entry) return;
    entry->state = data;
    entry->handle = handle;
    ext_foreign_toplevel_handle_v1_add_listener(handle, &wlr_ext_handle_listener, entry);
}

static void wlr_ext_finished(void *data, struct ext_foreign_toplevel_list_v1 *list)
{
    (void)data;
    (void)list;
}

static const struct ext_foreign_toplevel_list_v1_listener wlr_ext_list_listener = {
    .toplevel = wlr_ext_toplevel_added,
    .finished = wlr_ext_finished,
};

/* ---------------------------------------------------------------- registry */

static void wlr_output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
                                int32_t physical_width, int32_t physical_height, int32_t subpixel,
                                const char *make, const char *model, int32_t transform)
{
    (void)data; (void)output; (void)x; (void)y; (void)physical_width; (void)physical_height;
    (void)subpixel; (void)make; (void)model; (void)transform;
}

static void wlr_output_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width,
                            int32_t height, int32_t refresh)
{
    (void)data; (void)output; (void)flags; (void)width; (void)height; (void)refresh;
}

static void wlr_output_done(void *data, struct wl_output *output)
{
    (void)data;
    (void)output;
}

static void wlr_output_scale(void *data, struct wl_output *output, int32_t factor)
{
    (void)data;
    (void)output;
    (void)factor;
}

static void wlr_output_name(void *data, struct wl_output *output, const char *name)
{
    wlr_state *state = data;
    for (size_t i = 0; i < state->output_count; i++) {
        if (state->outputs[i] != output) continue;
        vs_window_set_string(state->output_names[i], VS_WINDOW_OUTPUT_MAX, name);
        return;
    }
}

static void wlr_output_description(void *data, struct wl_output *output, const char *description)
{
    (void)data;
    (void)output;
    (void)description;
}

static const struct wl_output_listener wlr_output_listener = {
    .geometry = wlr_output_geometry,
    .mode = wlr_output_mode,
    .done = wlr_output_done,
    .scale = wlr_output_scale,
    .name = wlr_output_name,
    .description = wlr_output_description,
};

static void wlr_registry_global(void *data, struct wl_registry *registry, uint32_t name,
                                const char *interface, uint32_t version)
{
    wlr_state *state = data;
    if (strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
        uint32_t bind_version = version < WLR_MANAGER_VERSION ? version : WLR_MANAGER_VERSION;
        state->manager = wl_registry_bind(registry, name,
                                          &zwlr_foreign_toplevel_manager_v1_interface, bind_version);
        zwlr_foreign_toplevel_manager_v1_add_listener(state->manager, &wlr_manager_listener, state);
    } else if (strcmp(interface, ext_foreign_toplevel_list_v1_interface.name) == 0) {
        state->ext_list = wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, 1);
        ext_foreign_toplevel_list_v1_add_listener(state->ext_list, &wlr_ext_list_listener, state);
    } else if (strcmp(interface, wl_seat_interface.name) == 0 && !state->seat) {
        state->seat_name = name;
        state->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0 && version >= 4) {
        struct wl_output **outputs =
            realloc(state->outputs, (state->output_count + 1) * sizeof(*outputs));
        if (!outputs) return;
        state->outputs = outputs;
        char (*names)[VS_WINDOW_OUTPUT_MAX] =
            realloc(state->output_names, (state->output_count + 1) * VS_WINDOW_OUTPUT_MAX);
        if (!names) return;
        state->output_names = names;
        state->outputs[state->output_count] = wl_registry_bind(registry, name, &wl_output_interface, 4);
        state->output_names[state->output_count][0] = '\0';
        wl_output_add_listener(state->outputs[state->output_count], &wlr_output_listener, state);
        state->output_count++;
    }
}

static void wlr_registry_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener wlr_registry_listener = {
    .global = wlr_registry_global,
    .global_remove = wlr_registry_remove,
};

/* ------------------------------------------------------------------ vtable */

/** Pump the connection without blocking. */
static int wlr_pump(wlr_state *state)
{
    wl_display_flush(state->display);
    while (wl_display_prepare_read(state->display) != 0) {
        if (wl_display_dispatch_pending(state->display) < 0) return VS_ERR_BACKEND;
    }
    struct pollfd descriptor = { .fd = wl_display_get_fd(state->display), .events = POLLIN };
    int ready = poll(&descriptor, 1, 0);
    if (ready > 0 && (descriptor.revents & POLLIN)) {
        if (wl_display_read_events(state->display) < 0) return VS_ERR_BACKEND;
    } else {
        wl_display_cancel_read(state->display);
    }
    if (wl_display_dispatch_pending(state->display) < 0) return VS_ERR_BACKEND;
    return VS_OK;
}

static int wlr_sway_snapshot(wlr_state *state, sway_container **containers, size_t *count,
                             int32_t *current_workspace)
{
    *containers = NULL;
    *count = 0;
    *current_workspace = -1;
    if (!state->has_sway) return VS_OK;
    if (sway_ipc_containers(&state->sway, containers, count) != VS_OK) return VS_ERR_BACKEND;
    sway_ipc_current_workspace(&state->sway, current_workspace);
    return VS_OK;
}

static int wlr_list(vs_window_system *self, vs_window_info **windows_out, size_t *count_out)
{
    wlr_state *state = self->impl;
    if (wl_display_roundtrip(state->display) < 0) return VS_ERR_BACKEND;

    sway_container *containers = NULL;
    size_t container_count = 0;
    int32_t current_workspace = -1;
    wlr_sway_snapshot(state, &containers, &container_count, &current_workspace);

    vs_window_vec vec = { 0 };
    for (wlr_toplevel *toplevel = state->toplevels; toplevel; toplevel = toplevel->next) {
        if (!toplevel->announced || toplevel->closed) continue;
        vs_window_info info;
        wlr_fill(state, toplevel, &info, containers, container_count, current_workspace);
        info.stacking_index = (uint32_t)vec.count;
        /* The protocol announces toplevels in creation order and never
         * restacks them, so this is an ordering, not a stacking order. */
        info.stacking_valid = false;
        if (!vs_window_vec_push(&vec, &info)) {
            free(containers);
            vs_window_vec_free(&vec);
            return VS_ERR_NO_MEM;
        }
    }
    free(containers);
    *windows_out = vec.items;
    *count_out = vec.count;
    return VS_OK;
}

static int wlr_activate(vs_window_system *self, vs_window_id id)
{
    wlr_state *state = self->impl;
    wlr_toplevel *toplevel = wlr_find(state, id);
    if (!toplevel) return VS_ERR_NOT_FOUND;
    if (!state->seat) return VS_ERR_UNSUPPORTED;
    if (toplevel->flags & VS_WINDOW_MINIMIZED) {
        zwlr_foreign_toplevel_handle_v1_unset_minimized(toplevel->handle);
    }
    zwlr_foreign_toplevel_handle_v1_activate(toplevel->handle, state->seat);
    if (wl_display_roundtrip(state->display) < 0) return VS_ERR_BACKEND;
    return VS_OK;
}

static int wlr_close(vs_window_system *self, vs_window_id id)
{
    wlr_state *state = self->impl;
    wlr_toplevel *toplevel = wlr_find(state, id);
    if (!toplevel) return VS_ERR_NOT_FOUND;
    zwlr_foreign_toplevel_handle_v1_close(toplevel->handle);
    /* A round trip, not a flush: the caller is entitled to know the compositor
     * has the request before the process that sent it goes away. A flush alone
     * loses the close often enough to be seen in the sway suite. */
    if (wl_display_roundtrip(state->display) < 0) return VS_ERR_BACKEND;
    return VS_OK;
}

static int wlr_set_minimized(vs_window_system *self, vs_window_id id, bool minimized)
{
    wlr_state *state = self->impl;
    wlr_toplevel *toplevel = wlr_find(state, id);
    if (!toplevel) return VS_ERR_NOT_FOUND;
    if (minimized) zwlr_foreign_toplevel_handle_v1_set_minimized(toplevel->handle);
    else zwlr_foreign_toplevel_handle_v1_unset_minimized(toplevel->handle);
    if (wl_display_roundtrip(state->display) < 0) return VS_ERR_BACKEND;
    return VS_OK;
}

/** Resolve a toplevel to the sway container id, which is what sway commands
 *  address. Returns 0 when the match is missing or ambiguous. */
static int64_t wlr_con_id(wlr_state *state, const wlr_toplevel *toplevel)
{
    sway_container *containers = NULL;
    size_t count = 0;
    if (sway_ipc_containers(&state->sway, &containers, &count) != VS_OK) return 0;
    int64_t con_id = 0;
    size_t matches = 0;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(containers[i].app_id, toplevel->app_id) != 0) continue;
        if (strcmp(containers[i].title, toplevel->title) != 0) continue;
        con_id = containers[i].con_id;
        matches++;
    }
    free(containers);
    return matches == 1 ? con_id : 0;
}

static int wlr_geometry(vs_window_system *self, vs_window_id id, vs_rect *frame_out)
{
    wlr_state *state = self->impl;
    if (!frame_out) return VS_ERR_INVALID;
    if (!state->has_sway) return VS_ERR_UNSUPPORTED;
    wlr_toplevel *toplevel = wlr_find(state, id);
    if (!toplevel) return VS_ERR_NOT_FOUND;

    sway_container *containers = NULL;
    size_t count = 0;
    if (sway_ipc_containers(&state->sway, &containers, &count) != VS_OK) return VS_ERR_BACKEND;
    int result = VS_ERR_NOT_FOUND;
    size_t matches = 0;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(containers[i].app_id, toplevel->app_id) != 0) continue;
        if (strcmp(containers[i].title, toplevel->title) != 0) continue;
        *frame_out = containers[i].frame;
        matches++;
    }
    if (matches == 1) result = VS_OK;
    free(containers);
    return result;
}

static int wlr_move_resize(vs_window_system *self, vs_window_id id, vs_rect frame, int32_t tolerance)
{
    wlr_state *state = self->impl;
    if (!state->has_sway) return VS_ERR_UNSUPPORTED;
    if (frame.width <= 0 || frame.height <= 0) return VS_ERR_INVALID;
    wlr_toplevel *toplevel = wlr_find(state, id);
    if (!toplevel) return VS_ERR_NOT_FOUND;

    int64_t con_id = wlr_con_id(state, toplevel);
    if (con_id == 0) return VS_ERR_NOT_FOUND;

    /* Three separate runs, not one comma-separated list. Only floating
     * containers obey an absolute position, and sway commits a command list as
     * one transaction: `floating enable` in the same list as `resize set`
     * re-applies its own default float size afterwards and the resize is lost
     * (measured on sway 1.9: 800x600 requested, 700x500 applied). A criteria
     * selector is also only accepted once per list, so repeating it is a parse
     * error. */
    char command[160];
    snprintf(command, sizeof(command), "[con_id=%lld] floating enable", (long long)con_id);
    int result = sway_ipc_run(&state->sway, command);
    if (result != VS_OK) return result;

    /* `floating enable` starts its own transaction that gives the window sway's
     * default float size, and that transaction can land *after* a resize sent
     * immediately behind it (measured on sway 1.9: 800x600 requested, 700x500
     * afterwards). Re-issue until the window settles where it was asked to go,
     * exactly as the macOS WindowLayoutService re-applies a frame. */
    for (int attempt = 0; attempt < 4; attempt++) {
        snprintf(command, sizeof(command), "[con_id=%lld] resize set %d %d", (long long)con_id,
                 frame.width, frame.height);
        result = sway_ipc_run(&state->sway, command);
        if (result != VS_OK) return result;

        snprintf(command, sizeof(command), "[con_id=%lld] move position %d %d", (long long)con_id,
                 frame.x, frame.y);
        result = sway_ipc_run(&state->sway, command);
        if (result != VS_OK) return result;
        if (tolerance < 0) return VS_OK;

        for (int poll_attempt = 0; poll_attempt < 8; poll_attempt++) {
            struct timespec pause = { .tv_sec = 0, .tv_nsec = 25 * 1000 * 1000 };
            nanosleep(&pause, NULL);
            vs_rect actual;
            if (wlr_geometry(self, id, &actual) == VS_OK &&
                vs_rect_close(actual, frame, tolerance)) {
                return VS_OK;
            }
        }
    }
    return VS_ERR_NOT_APPLIED;
}

static int wlr_current_workspace(vs_window_system *self, int32_t *workspace_out)
{
    wlr_state *state = self->impl;
    if (!workspace_out) return VS_ERR_INVALID;
    if (!state->has_sway) return VS_ERR_UNSUPPORTED;
    return sway_ipc_current_workspace(&state->sway, workspace_out);
}

static int wlr_set_workspace(vs_window_system *self, int32_t workspace)
{
    wlr_state *state = self->impl;
    if (!state->has_sway) return VS_ERR_UNSUPPORTED;
    if (workspace < 0) return VS_ERR_INVALID;
    char command[64];
    snprintf(command, sizeof(command), "workspace number %d", workspace);
    int result = sway_ipc_run(&state->sway, command);
    if (result != VS_OK) return result;
    int32_t actual = -1;
    if (sway_ipc_current_workspace(&state->sway, &actual) == VS_OK && actual == workspace) return VS_OK;
    return VS_ERR_NOT_APPLIED;
}

static int wlr_set_event_callback(vs_window_system *self, vs_window_event_cb callback, void *user_data)
{
    wlr_state *state = self->impl;
    state->callback = callback;
    state->callback_data = user_data;
    /* Anything queued while nobody was listening belongs to the snapshot the
     * caller has already taken. */
    state->pending_count = 0;
    return VS_OK;
}

static int wlr_event_fd(vs_window_system *self)
{
    wlr_state *state = self->impl;
    return wl_display_get_fd(state->display);
}

static int wlr_dispatch(vs_window_system *self)
{
    wlr_state *state = self->impl;
    int result = wlr_pump(state);
    if (result != VS_OK) {
        if (state->callback) {
            vs_window_event event = { .type = VS_WINDOW_EVENT_BACKEND_LOST, .workspace = -1 };
            state->callback(&event, state->callback_data);
        }
        return result;
    }

    size_t delivered = state->pending_count;
    for (size_t i = 0; i < state->pending_count; i++) {
        if (!state->callback) break;
        wlr_pending_event *pending = &state->pending[i];
        vs_window_event event = {
            .type = pending->type,
            .id = pending->id,
            .info = pending->has_info ? &pending->info : NULL,
            .workspace = pending->workspace,
        };
        state->callback(&event, state->callback_data);
    }
    state->pending_count = 0;
    return (int)delivered;
}

static void wlr_state_free(wlr_state *state)
{
    if (state) {
        while (state->toplevels) {
            wlr_toplevel *next = state->toplevels->next;
            zwlr_foreign_toplevel_handle_v1_destroy(state->toplevels->handle);
            free(state->toplevels);
            state->toplevels = next;
        }
        if (state->manager) zwlr_foreign_toplevel_manager_v1_destroy(state->manager);
        if (state->ext_list) ext_foreign_toplevel_list_v1_destroy(state->ext_list);
        if (state->seat) wl_seat_destroy(state->seat);
        for (size_t i = 0; i < state->output_count; i++) wl_output_destroy(state->outputs[i]);
        free(state->outputs);
        free(state->output_names);
        if (state->registry) wl_registry_destroy(state->registry);
        if (state->display) wl_display_disconnect(state->display);
        if (state->has_sway) sway_ipc_close(&state->sway);
        free(state->pending);
        free(state);
    }
}

static void wlr_destroy(vs_window_system *self)
{
    wlr_state_free(self->impl);
    free(self);
}

vs_window_system *vs_window_backend_wlr_create(int *result_out)
{
    if (result_out) *result_out = VS_ERR_NO_BACKEND;
    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    if ((!wayland_display || !*wayland_display) && !getenv("WAYLAND_SOCKET")) return NULL;

    wlr_state *state = calloc(1, sizeof(*state));
    if (!state) {
        if (result_out) *result_out = VS_ERR_NO_MEM;
        return NULL;
    }
    state->next_id = 1;
    state->sway.fd = -1;

    state->display = wl_display_connect(NULL);
    if (!state->display) {
        free(state);
        if (result_out) *result_out = VS_ERR_BACKEND;
        return NULL;
    }

    state->registry = wl_display_get_registry(state->display);
    wl_registry_add_listener(state->registry, &wlr_registry_listener, state);
    wl_display_roundtrip(state->display);
    /* Second round trip: the manager announces its existing toplevels only
     * after we have bound it, and output names arrive on their own listener. */
    wl_display_roundtrip(state->display);

    if (!state->manager) {
        vs_window_log("wlr: compositor advertises no %s",
                      zwlr_foreign_toplevel_manager_v1_interface.name);
        wl_display_disconnect(state->display);
        free(state->outputs);
        free(state->output_names);
        free(state);
        if (result_out) *result_out = VS_ERR_NO_BACKEND;
        return NULL;
    }
    wl_display_roundtrip(state->display);

    state->has_sway = sway_ipc_open(&state->sway, NULL) == VS_OK;
    if (state->has_sway) vs_window_log("wlr: sway IPC available, move/resize enabled");

    vs_window_system *system = calloc(1, sizeof(*system));
    if (!system) {
        wlr_state_free(state);
        if (result_out) *result_out = VS_ERR_NO_MEM;
        return NULL;
    }

    system->name = "wlr";
    system->impl = state;
    system->capabilities = VS_WINDOW_CAN_LIST | VS_WINDOW_CAN_ACTIVATE | VS_WINDOW_CAN_CLOSE |
                           VS_WINDOW_CAN_MINIMIZE | VS_WINDOW_HAS_LIVE_EVENTS;
    if (state->has_sway) {
        system->capabilities |= VS_WINDOW_CAN_MOVE_RESIZE | VS_WINDOW_CAN_WORKSPACE_SWITCH;
    }
    system->list = wlr_list;
    system->free_list = vs_window_free_list_default;
    system->activate = wlr_activate;
    system->close = wlr_close;
    system->set_minimized = wlr_set_minimized;
    system->move_resize = wlr_move_resize;
    system->geometry = wlr_geometry;
    system->current_workspace = wlr_current_workspace;
    system->set_workspace = wlr_set_workspace;
    system->set_event_callback = wlr_set_event_callback;
    system->event_fd = wlr_event_fd;
    system->dispatch = wlr_dispatch;
    system->destroy = wlr_destroy;

    if (result_out) *result_out = VS_OK;
    return system;
}
