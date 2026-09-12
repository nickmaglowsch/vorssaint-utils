/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * Hyprland backend, over its two IPC sockets:
 *
 *   $XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock
 *       request/response; one request per connection, the reply is the whole
 *       stream up to EOF. `j/`-prefixed commands answer in JSON.
 *   $XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket2.sock
 *       a line stream of `EVENT>>data` notifications, held open.
 *
 * Hyprland needs a DRM device, so it cannot run in this project's CI or
 * container. This backend is written against the documented protocol and
 * exercised by tests/fake_hyprland.py, which replays recorded-format fixtures
 * over the same two sockets. It is UNVERIFIED against a live Hyprland; see
 * docs/linux-port/WINDOW_BACKENDS.md.
 */

#include "vs_window_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char request_path[108];
    char event_path[108];
    int event_fd;
    char event_buffer[8192];
    size_t event_used;

    vs_window_event_cb callback;
    void *callback_data;
} hypr_state;

/* ------------------------------------------------------------ transport */

static int hypr_connect(const char *path)
{
    struct sockaddr_un address = { 0 };
    address.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(address.sun_path)) return -1;
    strcpy(address.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/** One request, one response, one connection. Caller frees `*reply_out`. */
static int hypr_request(hypr_state *state, const char *command, char **reply_out)
{
    if (reply_out) *reply_out = NULL;
    int fd = hypr_connect(state->request_path);
    if (fd < 0) return VS_ERR_BACKEND;

    size_t length = strlen(command);
    const char *cursor = command;
    while (length > 0) {
        ssize_t written = write(fd, cursor, length);
        if (written < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return VS_ERR_BACKEND;
        }
        cursor += written;
        length -= (size_t)written;
    }
    shutdown(fd, SHUT_WR);

    size_t capacity = 4096;
    size_t used = 0;
    char *buffer = malloc(capacity);
    if (!buffer) {
        close(fd);
        return VS_ERR_NO_MEM;
    }
    for (;;) {
        if (used + 1 >= capacity) {
            size_t next = capacity * 2;
            char *grown = realloc(buffer, next);
            if (!grown) {
                free(buffer);
                close(fd);
                return VS_ERR_NO_MEM;
            }
            buffer = grown;
            capacity = next;
        }
        ssize_t got = read(fd, buffer + used, capacity - used - 1);
        if (got < 0) {
            if (errno == EINTR) continue;
            free(buffer);
            close(fd);
            return VS_ERR_BACKEND;
        }
        if (got == 0) break;
        used += (size_t)got;
    }
    buffer[used] = '\0';
    close(fd);

    if (reply_out) *reply_out = buffer;
    else free(buffer);
    return VS_OK;
}

/** Hyprland answers a dispatch with "ok" and anything else is the reason. */
static int hypr_dispatch_command(hypr_state *state, const char *command)
{
    char *reply = NULL;
    int result = hypr_request(state, command, &reply);
    if (result != VS_OK) return result;
    bool ok = reply && strncmp(reply, "ok", 2) == 0;
    if (!ok) vs_window_log("hyprland: %s refused: %s", command, reply ? reply : "(no reply)");
    free(reply);
    return ok ? VS_OK : VS_ERR_BACKEND;
}

/* ------------------------------------------------------------ JSON model */

static const char *hypr_string(json_object *object, const char *key)
{
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value)) return NULL;
    if (!json_object_is_type(value, json_type_string)) return NULL;
    return json_object_get_string(value);
}

static int64_t hypr_int(json_object *object, const char *key, int64_t fallback)
{
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value)) return fallback;
    if (json_object_is_type(value, json_type_null)) return fallback;
    return json_object_get_int64(value);
}

static bool hypr_bool(json_object *object, const char *key)
{
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value)) return false;
    return json_object_get_boolean(value);
}

static void hypr_pair(json_object *object, const char *key, int32_t *first, int32_t *second)
{
    json_object *array = NULL;
    if (!json_object_object_get_ex(object, key, &array)) return;
    if (!json_object_is_type(array, json_type_array)) return;
    if (json_object_array_length(array) < 2) return;
    *first = (int32_t)json_object_get_int64(json_object_array_get_idx(array, 0));
    *second = (int32_t)json_object_get_int64(json_object_array_get_idx(array, 1));
}

/** Hyprland writes window addresses as "0x55f0..."; its event stream writes the
 *  same value without the prefix. */
static vs_window_id hypr_parse_address(const char *text)
{
    if (!text) return 0;
    while (*text == ' ') text++;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
    return (vs_window_id)strtoull(text, NULL, 16);
}

static void hypr_format_address(vs_window_id id, char *out, size_t capacity)
{
    snprintf(out, capacity, "0x%llx", (unsigned long long)id);
}

/** Monitor id → name, so a window can report its output like every other
 *  backend does. */
typedef struct {
    int64_t id;
    char name[VS_WINDOW_OUTPUT_MAX];
} hypr_monitor;

static int hypr_monitors(hypr_state *state, hypr_monitor **out, size_t *count_out)
{
    *out = NULL;
    *count_out = 0;
    char *body = NULL;
    if (hypr_request(state, "j/monitors", &body) != VS_OK) return VS_ERR_BACKEND;
    json_object *monitors = json_tokener_parse(body);
    free(body);
    if (!monitors || !json_object_is_type(monitors, json_type_array)) {
        if (monitors) json_object_put(monitors);
        return VS_ERR_BACKEND;
    }
    size_t length = json_object_array_length(monitors);
    hypr_monitor *items = length ? calloc(length, sizeof(*items)) : NULL;
    if (length && !items) {
        json_object_put(monitors);
        return VS_ERR_NO_MEM;
    }
    for (size_t i = 0; i < length; i++) {
        json_object *monitor = json_object_array_get_idx(monitors, i);
        items[i].id = hypr_int(monitor, "id", -1);
        vs_window_set_string(items[i].name, sizeof(items[i].name), hypr_string(monitor, "name"));
    }
    json_object_put(monitors);
    *out = items;
    *count_out = length;
    return VS_OK;
}

static int32_t hypr_active_workspace(hypr_state *state)
{
    char *body = NULL;
    if (hypr_request(state, "j/activeworkspace", &body) != VS_OK) return -1;
    json_object *workspace = json_tokener_parse(body);
    free(body);
    if (!workspace) return -1;
    int32_t id = (int32_t)hypr_int(workspace, "id", -1);
    json_object_put(workspace);
    return id;
}

static void hypr_fill(json_object *client, const hypr_monitor *monitors, size_t monitor_count,
                      int32_t current_workspace, vs_window_info *info)
{
    vs_window_info_init(info);
    info->id = hypr_parse_address(hypr_string(client, "address"));
    vs_window_set_string(info->app_id, sizeof(info->app_id), hypr_string(client, "class"));
    vs_window_set_string(info->app_name, sizeof(info->app_name), hypr_string(client, "class"));
    vs_window_set_string(info->title, sizeof(info->title), hypr_string(client, "title"));
    info->pid = (int32_t)hypr_int(client, "pid", -1);
    if (info->pid > 0) info->flags |= VS_WINDOW_HAS_PID;

    int32_t x = 0, y = 0, width = 0, height = 0;
    hypr_pair(client, "at", &x, &y);
    hypr_pair(client, "size", &width, &height);
    info->frame = (vs_rect){ .x = x, .y = y, .width = width, .height = height };
    if (width > 0 && height > 0) info->flags |= VS_WINDOW_HAS_GEOMETRY;

    json_object *workspace = NULL;
    if (json_object_object_get_ex(client, "workspace", &workspace)) {
        info->workspace = (int32_t)hypr_int(workspace, "id", -1);
    }
    if (current_workspace < 0 || info->workspace == current_workspace) {
        info->flags |= VS_WINDOW_ON_CURRENT_WORKSPACE;
    }

    if (hypr_bool(client, "hidden") || !hypr_bool(client, "mapped")) {
        info->flags |= VS_WINDOW_MINIMIZED;
    }
    if (hypr_int(client, "fullscreen", 0) != 0) info->flags |= VS_WINDOW_FULLSCREEN;
    /* focusHistoryID 0 is the focused window; Hyprland renumbers the rest on
     * every focus change. */
    if (hypr_int(client, "focusHistoryID", -1) == 0) info->flags |= VS_WINDOW_FOCUSED;
    if (!(info->flags & VS_WINDOW_MINIMIZED) && (info->flags & VS_WINDOW_ON_CURRENT_WORKSPACE)) {
        info->flags |= VS_WINDOW_ON_SCREEN;
    }

    int64_t monitor_id = hypr_int(client, "monitor", -1);
    for (size_t i = 0; i < monitor_count; i++) {
        if (monitors[i].id != monitor_id) continue;
        vs_window_set_string(info->output, sizeof(info->output), monitors[i].name);
        break;
    }
}

/* ------------------------------------------------------------------ vtable */

static int hypr_list(vs_window_system *self, vs_window_info **windows_out, size_t *count_out)
{
    hypr_state *state = self->impl;
    char *body = NULL;
    int result = hypr_request(state, "j/clients", &body);
    if (result != VS_OK) return result;

    json_object *clients = json_tokener_parse(body);
    free(body);
    if (!clients || !json_object_is_type(clients, json_type_array)) {
        if (clients) json_object_put(clients);
        return VS_ERR_BACKEND;
    }

    hypr_monitor *monitors = NULL;
    size_t monitor_count = 0;
    hypr_monitors(state, &monitors, &monitor_count);
    int32_t current_workspace = hypr_active_workspace(state);

    vs_window_vec vec = { 0 };
    size_t length = json_object_array_length(clients);
    for (size_t i = 0; i < length; i++) {
        vs_window_info info;
        hypr_fill(json_object_array_get_idx(clients, i), monitors, monitor_count, current_workspace,
                  &info);
        if (info.id == 0) continue;
        info.stacking_index = (uint32_t)vec.count;
        /* `j/clients` is documented as unordered; only focusHistoryID carries
         * a usable recency order, which WP-C3 reads instead. */
        info.stacking_valid = false;
        if (!vs_window_vec_push(&vec, &info)) {
            json_object_put(clients);
            free(monitors);
            vs_window_vec_free(&vec);
            return VS_ERR_NO_MEM;
        }
    }
    json_object_put(clients);
    free(monitors);

    *windows_out = vec.items;
    *count_out = vec.count;
    return VS_OK;
}

static int hypr_find(vs_window_system *self, vs_window_id id, vs_window_info *out)
{
    vs_window_info *windows = NULL;
    size_t count = 0;
    int result = hypr_list(self, &windows, &count);
    if (result != VS_OK) return result;
    result = VS_ERR_NOT_FOUND;
    for (size_t i = 0; i < count; i++) {
        if (windows[i].id != id) continue;
        if (out) *out = windows[i];
        result = VS_OK;
        break;
    }
    free(windows);
    return result;
}

static int hypr_activate(vs_window_system *self, vs_window_id id)
{
    hypr_state *state = self->impl;
    int result = hypr_find(self, id, NULL);
    if (result != VS_OK) return result;
    char address[32];
    hypr_format_address(id, address, sizeof(address));
    char command[96];
    snprintf(command, sizeof(command), "dispatch focuswindow address:%s", address);
    return hypr_dispatch_command(state, command);
}

static int hypr_close(vs_window_system *self, vs_window_id id)
{
    hypr_state *state = self->impl;
    int result = hypr_find(self, id, NULL);
    if (result != VS_OK) return result;
    char address[32];
    hypr_format_address(id, address, sizeof(address));
    char command[96];
    snprintf(command, sizeof(command), "dispatch closewindow address:%s", address);
    return hypr_dispatch_command(state, command);
}

static int hypr_set_minimized(vs_window_system *self, vs_window_id id, bool minimized)
{
    (void)self;
    (void)id;
    (void)minimized;
    /* Hyprland has no minimized state. The usual substitute, moving the window
     * to a special workspace, loses the window's own workspace and is not a
     * restore the compositor will undo on its own, so this backend declines
     * rather than pretending. The switcher reports Hyprland's `hidden` windows
     * as minimized but cannot put one there. */
    return VS_ERR_UNSUPPORTED;
}

static int hypr_geometry(vs_window_system *self, vs_window_id id, vs_rect *frame_out)
{
    if (!frame_out) return VS_ERR_INVALID;
    vs_window_info info;
    int result = hypr_find(self, id, &info);
    if (result != VS_OK) return result;
    if (!(info.flags & VS_WINDOW_HAS_GEOMETRY)) return VS_ERR_UNSUPPORTED;
    *frame_out = info.frame;
    return VS_OK;
}

static int hypr_move_resize(vs_window_system *self, vs_window_id id, vs_rect frame, int32_t tolerance)
{
    hypr_state *state = self->impl;
    if (frame.width <= 0 || frame.height <= 0) return VS_ERR_INVALID;
    int result = hypr_find(self, id, NULL);
    if (result != VS_OK) return result;

    char address[32];
    hypr_format_address(id, address, sizeof(address));

    /* Only floating windows take an absolute position; a tiled window keeps its
     * layout geometry and the read-back below reports that honestly. */
    char command[160];
    snprintf(command, sizeof(command), "dispatch setfloating address:%s", address);
    hypr_dispatch_command(state, command);

    snprintf(command, sizeof(command), "dispatch resizewindowpixel exact %d %d,address:%s",
             frame.width, frame.height, address);
    result = hypr_dispatch_command(state, command);
    if (result != VS_OK) return result;

    snprintf(command, sizeof(command), "dispatch movewindowpixel exact %d %d,address:%s", frame.x,
             frame.y, address);
    result = hypr_dispatch_command(state, command);
    if (result != VS_OK) return result;
    if (tolerance < 0) return VS_OK;

    for (int attempt = 0; attempt < 20; attempt++) {
        vs_rect actual;
        if (hypr_geometry(self, id, &actual) == VS_OK && vs_rect_close(actual, frame, tolerance)) {
            return VS_OK;
        }
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 25 * 1000 * 1000 };
        nanosleep(&pause, NULL);
    }
    return VS_ERR_NOT_APPLIED;
}

static int hypr_current_workspace(vs_window_system *self, int32_t *workspace_out)
{
    hypr_state *state = self->impl;
    if (!workspace_out) return VS_ERR_INVALID;
    int32_t workspace = hypr_active_workspace(state);
    if (workspace < 0) return VS_ERR_BACKEND;
    *workspace_out = workspace;
    return VS_OK;
}

static int hypr_set_workspace(vs_window_system *self, int32_t workspace)
{
    hypr_state *state = self->impl;
    if (workspace < 0) return VS_ERR_INVALID;
    char command[64];
    snprintf(command, sizeof(command), "dispatch workspace %d", workspace);
    int result = hypr_dispatch_command(state, command);
    if (result != VS_OK) return result;
    return hypr_active_workspace(state) == workspace ? VS_OK : VS_ERR_NOT_APPLIED;
}

/* --------------------------------------------------------------- events */

static void hypr_emit(hypr_state *state, vs_window_event_type type, vs_window_id id,
                      int32_t workspace)
{
    if (!state->callback) return;
    vs_window_event event = { .type = type, .id = id, .info = NULL, .workspace = workspace };
    state->callback(&event, state->callback_data);
}

/** `EVENT>>arg,arg,...`; the documented set this backend acts on. */
static int hypr_handle_line(hypr_state *state, char *line)
{
    char *separator = strstr(line, ">>");
    if (!separator) return 0;
    *separator = '\0';
    const char *name = line;
    char *data = separator + 2;

    if (strcmp(name, "openwindow") == 0) {
        char *comma = strchr(data, ',');
        if (comma) *comma = '\0';
        hypr_emit(state, VS_WINDOW_EVENT_ADDED, hypr_parse_address(data), -1);
        return 1;
    }
    if (strcmp(name, "closewindow") == 0) {
        hypr_emit(state, VS_WINDOW_EVENT_REMOVED, hypr_parse_address(data), -1);
        return 1;
    }
    if (strcmp(name, "activewindowv2") == 0) {
        hypr_emit(state, VS_WINDOW_EVENT_ACTIVATED, hypr_parse_address(data), -1);
        return 1;
    }
    if (strcmp(name, "windowtitlev2") == 0 || strcmp(name, "movewindowv2") == 0 ||
        strcmp(name, "movewindow") == 0 || strcmp(name, "windowtitle") == 0 ||
        strcmp(name, "fullscreen") == 0) {
        char *comma = strchr(data, ',');
        if (comma) *comma = '\0';
        hypr_emit(state, VS_WINDOW_EVENT_CHANGED, hypr_parse_address(data), -1);
        return 1;
    }
    if (strcmp(name, "workspace") == 0 || strcmp(name, "workspacev2") == 0) {
        /* `workspacev2` leads with the numeric id, `workspace` with the name. */
        hypr_emit(state, VS_WINDOW_EVENT_WORKSPACE_CHANGED, 0, (int32_t)strtol(data, NULL, 10));
        return 1;
    }
    return 0;
}

static int hypr_dispatch_events(vs_window_system *self)
{
    hypr_state *state = self->impl;
    if (state->event_fd < 0) return VS_ERR_UNSUPPORTED;

    int emitted = 0;
    for (;;) {
        struct pollfd descriptor = { .fd = state->event_fd, .events = POLLIN };
        int ready = poll(&descriptor, 1, 0);
        if (ready <= 0) break;
        if (descriptor.revents & (POLLHUP | POLLERR)) {
            close(state->event_fd);
            state->event_fd = -1;
            hypr_emit(state, VS_WINDOW_EVENT_BACKEND_LOST, 0, -1);
            return emitted ? emitted : VS_ERR_BACKEND;
        }
        if (state->event_used + 1 >= sizeof(state->event_buffer)) {
            /* A line longer than the buffer is not one of the documented
             * events; drop what we have rather than wedging. */
            state->event_used = 0;
        }
        ssize_t got = read(state->event_fd, state->event_buffer + state->event_used,
                           sizeof(state->event_buffer) - state->event_used - 1);
        if (got < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return VS_ERR_BACKEND;
        }
        if (got == 0) {
            close(state->event_fd);
            state->event_fd = -1;
            hypr_emit(state, VS_WINDOW_EVENT_BACKEND_LOST, 0, -1);
            return emitted ? emitted : VS_ERR_BACKEND;
        }
        state->event_used += (size_t)got;
        state->event_buffer[state->event_used] = '\0';

        char *start = state->event_buffer;
        char *newline;
        while ((newline = strchr(start, '\n')) != NULL) {
            *newline = '\0';
            emitted += hypr_handle_line(state, start);
            start = newline + 1;
        }
        size_t remaining = state->event_used - (size_t)(start - state->event_buffer);
        memmove(state->event_buffer, start, remaining);
        state->event_used = remaining;
    }
    return emitted;
}

static int hypr_set_event_callback(vs_window_system *self, vs_window_event_cb callback,
                                   void *user_data)
{
    hypr_state *state = self->impl;
    state->callback = callback;
    state->callback_data = user_data;
    return VS_OK;
}

static int hypr_event_fd(vs_window_system *self)
{
    hypr_state *state = self->impl;
    return state->event_fd;
}

static void hypr_destroy(vs_window_system *self)
{
    hypr_state *state = self->impl;
    if (state) {
        if (state->event_fd >= 0) close(state->event_fd);
        free(state);
    }
    free(self);
}

vs_window_system *vs_window_backend_hyprland_create(int *result_out)
{
    if (result_out) *result_out = VS_ERR_NO_BACKEND;
    const char *signature = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!signature || !*signature) return NULL;
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime) runtime = "/tmp";

    hypr_state *state = calloc(1, sizeof(*state));
    if (!state) {
        if (result_out) *result_out = VS_ERR_NO_MEM;
        return NULL;
    }
    state->event_fd = -1;
    snprintf(state->request_path, sizeof(state->request_path), "%s/hypr/%s/.socket.sock", runtime,
             signature);
    snprintf(state->event_path, sizeof(state->event_path), "%s/hypr/%s/.socket2.sock", runtime,
             signature);

    char *reply = NULL;
    if (hypr_request(state, "j/version", &reply) != VS_OK) {
        vs_window_log("hyprland: no answer on %s", state->request_path);
        free(state);
        if (result_out) *result_out = VS_ERR_NO_BACKEND;
        return NULL;
    }
    free(reply);

    state->event_fd = hypr_connect(state->event_path);
    if (state->event_fd >= 0) {
        int flags = fcntl(state->event_fd, F_GETFL, 0);
        if (flags >= 0) fcntl(state->event_fd, F_SETFL, flags | O_NONBLOCK);
    } else {
        vs_window_log("hyprland: no event socket at %s", state->event_path);
    }

    vs_window_system *system = calloc(1, sizeof(*system));
    if (!system) {
        if (state->event_fd >= 0) close(state->event_fd);
        free(state);
        if (result_out) *result_out = VS_ERR_NO_MEM;
        return NULL;
    }

    system->name = "hyprland";
    system->impl = state;
    system->capabilities = VS_WINDOW_CAN_LIST | VS_WINDOW_CAN_ACTIVATE | VS_WINDOW_CAN_CLOSE |
                           VS_WINDOW_CAN_MOVE_RESIZE | VS_WINDOW_CAN_WORKSPACE_SWITCH;
    if (state->event_fd >= 0) system->capabilities |= VS_WINDOW_HAS_LIVE_EVENTS;
    system->list = hypr_list;
    system->free_list = vs_window_free_list_default;
    system->activate = hypr_activate;
    system->close = hypr_close;
    system->set_minimized = hypr_set_minimized;
    system->move_resize = hypr_move_resize;
    system->geometry = hypr_geometry;
    system->current_workspace = hypr_current_workspace;
    system->set_workspace = hypr_set_workspace;
    system->set_event_callback = hypr_set_event_callback;
    system->event_fd = hypr_event_fd;
    system->dispatch = hypr_dispatch_events;
    system->destroy = hypr_destroy;

    if (result_out) *result_out = VS_OK;
    return system;
}
