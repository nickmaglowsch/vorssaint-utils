/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * Client side of org.vorssaint.WindowBridge, and the GNOME backend that is
 * nothing but that client. See bridge_client.h for the interface.
 */

#include "bridge_client.h"

#include <errno.h>
#include <stdarg.h>
#include <time.h>

/* One second is generous for a Shell extension answering on the session bus and
 * short enough that the switcher never hangs behind a wedged one. */
#define BRIDGE_TIMEOUT_USEC (1000 * 1000)

int vs_bridge_read_windows(sd_bus_message *message, vs_window_info **windows_out, size_t *count_out)
{
    int rc = sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY,
                                            "(" VS_BRIDGE_WINDOW_SIGNATURE ")");
    if (rc < 0) return VS_ERR_BACKEND;

    vs_window_vec vec = { 0 };
    for (;;) {
        uint64_t id = 0;
        const char *app_id = NULL;
        const char *app_name = NULL;
        const char *title = NULL;
        const char *output = NULL;
        int32_t pid = -1, x = 0, y = 0, width = 0, height = 0, workspace = -1;
        uint32_t flags = 0;

        rc = sd_bus_message_read(message, "(" VS_BRIDGE_WINDOW_SIGNATURE ")", &id, &app_id,
                                 &app_name, &title, &pid, &x, &y, &width, &height, &flags,
                                 &workspace, &output);
        if (rc == 0) break;
        if (rc < 0) {
            vs_window_vec_free(&vec);
            return VS_ERR_BACKEND;
        }

        vs_window_info info;
        vs_window_info_init(&info);
        info.id = id;
        vs_window_set_string(info.app_id, sizeof(info.app_id), app_id);
        vs_window_set_string(info.app_name, sizeof(info.app_name), app_name);
        vs_window_set_string(info.title, sizeof(info.title), title);
        vs_window_set_string(info.output, sizeof(info.output), output);
        info.pid = pid;
        info.frame = (vs_rect){ .x = x, .y = y, .width = width, .height = height };
        info.flags = flags;
        info.workspace = workspace;
        info.stacking_index = (uint32_t)vec.count;
        /* Both bridges enumerate in the compositor's own stacking order. */
        info.stacking_valid = true;
        if (!vs_window_vec_push(&vec, &info)) {
            vs_window_vec_free(&vec);
            return VS_ERR_NO_MEM;
        }
    }
    sd_bus_message_exit_container(message);

    *windows_out = vec.items;
    *count_out = vec.count;
    return VS_OK;
}

bool vs_bridge_name_has_owner(sd_bus *bus, const char *service)
{
    sd_bus_message *reply = NULL;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int has_owner = 0;
    int rc = sd_bus_call_method(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                "org.freedesktop.DBus", "NameHasOwner", &error, &reply, "s",
                                service);
    if (rc >= 0) sd_bus_message_read(reply, "b", &has_owner);
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    return rc >= 0 && has_owner != 0;
}

/* --------------------------------------------------------- GNOME backend */

typedef struct {
    sd_bus *bus;
    vs_window_event_cb callback;
    void *callback_data;
    int emitted;
} gnome_state;

static int gnome_call(gnome_state *state, const char *method, sd_bus_message **reply_out,
                      const char *types, ...)
{
    sd_bus_message *call = NULL;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int rc = sd_bus_message_new_method_call(state->bus, &call, VS_BRIDGE_GNOME_SERVICE,
                                            VS_BRIDGE_PATH, VS_BRIDGE_INTERFACE, method);
    if (rc < 0) return VS_ERR_BACKEND;
    if (types && *types) {
        va_list args;
        va_start(args, types);
        rc = sd_bus_message_appendv(call, types, args);
        va_end(args);
        if (rc < 0) {
            sd_bus_message_unref(call);
            return VS_ERR_BACKEND;
        }
    }

    sd_bus_message *reply = NULL;
    rc = sd_bus_call(state->bus, call, BRIDGE_TIMEOUT_USEC, &error, &reply);
    sd_bus_message_unref(call);
    if (rc < 0) {
        int result = VS_ERR_BACKEND;
        if (sd_bus_error_has_name(&error, SD_BUS_ERROR_UNKNOWN_METHOD)) result = VS_ERR_UNSUPPORTED;
        else if (sd_bus_error_has_name(&error, SD_BUS_ERROR_INVALID_ARGS)) result = VS_ERR_NOT_FOUND;
        else if (sd_bus_error_has_name(&error, SD_BUS_ERROR_NO_REPLY) ||
                 sd_bus_error_has_name(&error, SD_BUS_ERROR_TIMEOUT)) result = VS_ERR_TIMEOUT;
        vs_window_log("gnome: %s failed: %s", method, error.message ? error.message : strerror(-rc));
        sd_bus_error_free(&error);
        return result;
    }
    sd_bus_error_free(&error);
    if (reply_out) *reply_out = reply;
    else sd_bus_message_unref(reply);
    return VS_OK;
}

static int gnome_list(vs_window_system *self, vs_window_info **windows_out, size_t *count_out)
{
    gnome_state *state = self->impl;
    sd_bus_message *reply = NULL;
    int result = gnome_call(state, "List", &reply, NULL);
    if (result != VS_OK) return result;
    result = vs_bridge_read_windows(reply, windows_out, count_out);
    sd_bus_message_unref(reply);
    return result;
}

static int gnome_activate(vs_window_system *self, vs_window_id id)
{
    return gnome_call(self->impl, "Activate", NULL, "t", id);
}

static int gnome_close(vs_window_system *self, vs_window_id id)
{
    return gnome_call(self->impl, "Close", NULL, "t", id);
}

static int gnome_set_minimized(vs_window_system *self, vs_window_id id, bool minimized)
{
    return gnome_call(self->impl, "SetMinimized", NULL, "tb", id, minimized ? 1 : 0);
}

static int gnome_geometry(vs_window_system *self, vs_window_id id, vs_rect *frame_out)
{
    if (!frame_out) return VS_ERR_INVALID;
    sd_bus_message *reply = NULL;
    int result = gnome_call(self->impl, "Geometry", &reply, "t", id);
    if (result != VS_OK) return result;
    int32_t x = 0, y = 0, width = 0, height = 0;
    int rc = sd_bus_message_read(reply, "(iiii)", &x, &y, &width, &height);
    sd_bus_message_unref(reply);
    if (rc < 0) return VS_ERR_BACKEND;
    *frame_out = (vs_rect){ .x = x, .y = y, .width = width, .height = height };
    return VS_OK;
}

static int gnome_move_resize(vs_window_system *self, vs_window_id id, vs_rect frame,
                             int32_t tolerance)
{
    if (frame.width <= 0 || frame.height <= 0) return VS_ERR_INVALID;
    int result = gnome_call(self->impl, "MoveResize", NULL, "tiiii", id, frame.x, frame.y,
                            frame.width, frame.height);
    if (result != VS_OK) return result;
    if (tolerance < 0) return VS_OK;

    /* Mutter commits a move_resize_frame on its next layout pass, so the first
     * read can still see the old frame. */
    for (int attempt = 0; attempt < 20; attempt++) {
        vs_rect actual;
        if (gnome_geometry(self, id, &actual) == VS_OK && vs_rect_close(actual, frame, tolerance)) {
            return VS_OK;
        }
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 25 * 1000 * 1000 };
        nanosleep(&pause, NULL);
    }
    return VS_ERR_NOT_APPLIED;
}

static int gnome_current_workspace(vs_window_system *self, int32_t *workspace_out)
{
    if (!workspace_out) return VS_ERR_INVALID;
    sd_bus_message *reply = NULL;
    int result = gnome_call(self->impl, "CurrentWorkspace", &reply, NULL);
    if (result != VS_OK) return result;
    int rc = sd_bus_message_read(reply, "i", workspace_out);
    sd_bus_message_unref(reply);
    return rc < 0 ? VS_ERR_BACKEND : VS_OK;
}

static int gnome_set_workspace(vs_window_system *self, int32_t workspace)
{
    if (workspace < 0) return VS_ERR_INVALID;
    int result = gnome_call(self->impl, "SetWorkspace", NULL, "i", workspace);
    if (result != VS_OK) return result;
    int32_t actual = -1;
    if (gnome_current_workspace(self, &actual) == VS_OK && actual == workspace) return VS_OK;
    return VS_ERR_NOT_APPLIED;
}

static void gnome_emit(gnome_state *state, vs_window_event_type type, vs_window_id id,
                       int32_t workspace)
{
    state->emitted++;
    if (!state->callback) return;
    vs_window_event event = { .type = type, .id = id, .info = NULL, .workspace = workspace };
    state->callback(&event, state->callback_data);
}

static int gnome_on_signal(sd_bus_message *message, void *user_data, sd_bus_error *error)
{
    (void)error;
    gnome_state *state = user_data;
    const char *member = sd_bus_message_get_member(message);
    if (!member) return 0;

    if (strcmp(member, "WorkspaceChanged") == 0) {
        int32_t workspace = -1;
        if (sd_bus_message_read(message, "i", &workspace) >= 0) {
            gnome_emit(state, VS_WINDOW_EVENT_WORKSPACE_CHANGED, 0, workspace);
        }
        return 0;
    }

    vs_window_event_type type;
    if (strcmp(member, "WindowAdded") == 0) type = VS_WINDOW_EVENT_ADDED;
    else if (strcmp(member, "WindowRemoved") == 0) type = VS_WINDOW_EVENT_REMOVED;
    else if (strcmp(member, "WindowActivated") == 0) type = VS_WINDOW_EVENT_ACTIVATED;
    else if (strcmp(member, "WindowChanged") == 0) type = VS_WINDOW_EVENT_CHANGED;
    else return 0;

    uint64_t id = 0;
    if (sd_bus_message_read(message, "t", &id) >= 0) gnome_emit(state, type, id, -1);
    return 0;
}

static int gnome_set_event_callback(vs_window_system *self, vs_window_event_cb callback,
                                    void *user_data)
{
    gnome_state *state = self->impl;
    state->callback = callback;
    state->callback_data = user_data;
    return VS_OK;
}

static int gnome_event_fd(vs_window_system *self)
{
    gnome_state *state = self->impl;
    return sd_bus_get_fd(state->bus);
}

static int gnome_dispatch(vs_window_system *self)
{
    gnome_state *state = self->impl;
    state->emitted = 0;
    for (;;) {
        int rc = sd_bus_process(state->bus, NULL);
        if (rc < 0) {
            gnome_emit(state, VS_WINDOW_EVENT_BACKEND_LOST, 0, -1);
            return VS_ERR_BACKEND;
        }
        if (rc == 0) break;
    }
    return state->emitted;
}

static void gnome_destroy(vs_window_system *self)
{
    gnome_state *state = self->impl;
    if (state) {
        if (state->bus) sd_bus_unref(state->bus);
        free(state);
    }
    free(self);
}

vs_window_system *vs_window_backend_gnome_create(int *result_out)
{
    if (result_out) *result_out = VS_ERR_NO_BACKEND;

    sd_bus *bus = NULL;
    if (sd_bus_open_user(&bus) < 0) return NULL;

    if (!vs_bridge_name_has_owner(bus, VS_BRIDGE_GNOME_SERVICE)) {
        vs_window_log("gnome: %s has no owner; the vorssaint-bridge extension is not running",
                      VS_BRIDGE_GNOME_SERVICE);
        sd_bus_unref(bus);
        return NULL;
    }

    gnome_state *state = calloc(1, sizeof(*state));
    if (!state) {
        sd_bus_unref(bus);
        if (result_out) *result_out = VS_ERR_NO_MEM;
        return NULL;
    }
    state->bus = bus;

    int rc = sd_bus_match_signal(bus, NULL, VS_BRIDGE_GNOME_SERVICE, VS_BRIDGE_PATH,
                                 VS_BRIDGE_INTERFACE, NULL, gnome_on_signal, state);
    if (rc < 0) vs_window_log("gnome: cannot watch bridge signals: %s", strerror(-rc));

    vs_window_system *system = calloc(1, sizeof(*system));
    if (!system) {
        sd_bus_unref(bus);
        free(state);
        if (result_out) *result_out = VS_ERR_NO_MEM;
        return NULL;
    }

    system->name = "gnome";
    system->impl = state;
    system->capabilities = VS_WINDOW_CAN_LIST | VS_WINDOW_CAN_ACTIVATE | VS_WINDOW_CAN_CLOSE |
                           VS_WINDOW_CAN_MINIMIZE | VS_WINDOW_CAN_MOVE_RESIZE |
                           VS_WINDOW_CAN_WORKSPACE_SWITCH |
                           (rc >= 0 ? VS_WINDOW_HAS_LIVE_EVENTS : 0u);
    system->list = gnome_list;
    system->free_list = vs_window_free_list_default;
    system->activate = gnome_activate;
    system->close = gnome_close;
    system->set_minimized = gnome_set_minimized;
    system->move_resize = gnome_move_resize;
    system->geometry = gnome_geometry;
    system->current_workspace = gnome_current_workspace;
    system->set_workspace = gnome_set_workspace;
    system->set_event_callback = gnome_set_event_callback;
    system->event_fd = gnome_event_fd;
    system->dispatch = gnome_dispatch;
    system->destroy = gnome_destroy;

    if (result_out) *result_out = VS_OK;
    return system;
}
