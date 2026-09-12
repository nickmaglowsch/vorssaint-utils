/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * KWin backend.
 *
 * KWin exposes no foreign-toplevel protocol and no window API of its own on
 * D-Bus; the supported route is its scripting engine. The engine can call out
 * over D-Bus (`callDBus`) but cannot own a bus name, so the two directions are
 * asymmetric:
 *
 *   requests   we write kwin/vorssaint-window.js with a VORSSAINT_COMMAND
 *              object prepended and load it through
 *              org.kde.KWin /Scripting loadScript + Script<n>.run, then
 *              unload it. One command, one short-lived script.
 *   answers    the script calls org.vorssaint.KWinBridge, which this file
 *              owns, with a JSON payload under the request's token.
 *   events     a second, persistent copy of the same script stays loaded and
 *              pushes windowAdded / windowRemoved / windowActivated /
 *              currentDesktopChanged into the same sink.
 *
 * Window ids are KWin `internalId` UUIDs; the portable id is their FNV-1a
 * hash, and this file keeps the reverse map for the duration of the session.
 *
 * KWin needs a real Plasma session, which this project's CI and container do
 * not have. This backend is exercised by tests/fake_kwin.py, which owns
 * org.kde.KWin, executes the generated script's VORSSAINT_COMMAND against a
 * fixture window set and answers on the sink exactly as the script would. It
 * is UNVERIFIED against a live KWin; see docs/linux-port/WINDOW_BACKENDS.md.
 */

#include "bridge_client.h"

#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define KWIN_SERVICE "org.kde.KWin"
#define KWIN_SCRIPTING_PATH "/Scripting"
#define KWIN_SCRIPTING_INTERFACE "org.kde.kwin.Scripting"
#define KWIN_SCRIPT_INTERFACE "org.kde.kwin.Script"

#define KWIN_SINK_SERVICE "org.vorssaint.KWinBridge"
#define KWIN_SINK_PATH "/org/vorssaint/KWinBridge"
#define KWIN_SINK_INTERFACE "org.vorssaint.KWinBridge"

/* A KWin script round trip is a file write, a script load and two D-Bus hops.
 * Two seconds is long enough for a loaded session and short enough that the
 * switcher gives up rather than hanging. */
#define KWIN_TIMEOUT_USEC (2 * 1000 * 1000)

typedef struct {
    vs_window_id id;
    char uuid[64];
} kwin_id_entry;

typedef struct {
    sd_bus *bus;
    sd_bus_slot *sink_slot;
    char *script_source;
    char script_directory[256];
    unsigned token_counter;
    int event_script_id;

    /* Answer to the request currently in flight. */
    char pending_token[32];
    bool answered;
    bool answer_ok;
    char *answer_json;
    char answer_message[256];

    kwin_id_entry *ids;
    size_t id_count;
    size_t id_capacity;

    vs_window_event_cb callback;
    void *callback_data;
    int emitted;
} kwin_state;

/* ------------------------------------------------------------- id mapping */

static vs_window_id kwin_hash(const char *uuid)
{
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char *byte = (const unsigned char *)uuid; *byte; byte++) {
        hash ^= *byte;
        hash *= 1099511628211ull;
    }
    return hash ? hash : 1;
}

static vs_window_id kwin_remember(kwin_state *state, const char *uuid)
{
    vs_window_id id = kwin_hash(uuid);
    for (size_t i = 0; i < state->id_count; i++) {
        if (state->ids[i].id != id) continue;
        if (strcmp(state->ids[i].uuid, uuid) != 0) {
            vs_window_log("kwin: id hash collision between %s and %s", state->ids[i].uuid, uuid);
        }
        return id;
    }
    if (state->id_count == state->id_capacity) {
        size_t capacity = state->id_capacity ? state->id_capacity * 2 : 32;
        kwin_id_entry *ids = realloc(state->ids, capacity * sizeof(*ids));
        if (!ids) return id;
        state->ids = ids;
        state->id_capacity = capacity;
    }
    state->ids[state->id_count].id = id;
    vs_window_set_string(state->ids[state->id_count].uuid, sizeof(state->ids[0].uuid), uuid);
    state->id_count++;
    return id;
}

static const char *kwin_uuid(kwin_state *state, vs_window_id id)
{
    for (size_t i = 0; i < state->id_count; i++) {
        if (state->ids[i].id == id) return state->ids[i].uuid;
    }
    return NULL;
}

/* ------------------------------------------------------------------- sink */

static void kwin_emit(kwin_state *state, vs_window_event_type type, vs_window_id id,
                      int32_t workspace)
{
    state->emitted++;
    if (!state->callback) return;
    vs_window_event event = { .type = type, .id = id, .info = NULL, .workspace = workspace };
    state->callback(&event, state->callback_data);
}

static int kwin_sink_result(sd_bus_message *message, void *user_data, sd_bus_error *error)
{
    (void)error;
    kwin_state *state = user_data;
    const char *token = NULL;
    int ok = 0;
    const char *json = NULL;
    const char *reason = NULL;
    int rc = sd_bus_message_read(message, "sbss", &token, &ok, &json, &reason);
    if (rc < 0) return sd_bus_reply_method_return(message, "");

    if (token && strcmp(token, state->pending_token) == 0) {
        state->answered = true;
        state->answer_ok = ok != 0;
        free(state->answer_json);
        state->answer_json = json ? strdup(json) : NULL;
        vs_window_set_string(state->answer_message, sizeof(state->answer_message), reason);
    } else {
        vs_window_log("kwin: stale answer for token %s", token ? token : "(none)");
    }
    return sd_bus_reply_method_return(message, "");
}

static int kwin_sink_event(sd_bus_message *message, void *user_data, sd_bus_error *error)
{
    (void)error;
    kwin_state *state = user_data;
    const char *type = NULL;
    const char *uuid = NULL;
    int32_t workspace = -1;
    if (sd_bus_message_read(message, "ssi", &type, &uuid, &workspace) < 0) {
        return sd_bus_reply_method_return(message, "");
    }

    vs_window_id id = (uuid && *uuid) ? kwin_remember(state, uuid) : 0;
    if (strcmp(type, "added") == 0) kwin_emit(state, VS_WINDOW_EVENT_ADDED, id, -1);
    else if (strcmp(type, "removed") == 0) kwin_emit(state, VS_WINDOW_EVENT_REMOVED, id, -1);
    else if (strcmp(type, "activated") == 0) kwin_emit(state, VS_WINDOW_EVENT_ACTIVATED, id, -1);
    else if (strcmp(type, "changed") == 0) kwin_emit(state, VS_WINDOW_EVENT_CHANGED, id, -1);
    else if (strcmp(type, "workspace") == 0) {
        kwin_emit(state, VS_WINDOW_EVENT_WORKSPACE_CHANGED, 0, workspace);
    }
    return sd_bus_reply_method_return(message, "");
}

static const sd_bus_vtable kwin_sink_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Result", "sbss", "", kwin_sink_result, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Event", "ssi", "", kwin_sink_event, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

/* ----------------------------------------------------------- script loading */

static int kwin_write_script(kwin_state *state, const char *prologue, char *path_out,
                             size_t path_capacity)
{
    snprintf(path_out, path_capacity, "%s/vorssaint-window-%u.js", state->script_directory,
             state->token_counter);
    int fd = open(path_out, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return VS_ERR_BACKEND;

    bool ok = true;
    if (prologue) {
        size_t length = strlen(prologue);
        ok = write(fd, prologue, length) == (ssize_t)length;
    }
    if (ok) {
        size_t length = strlen(state->script_source);
        ok = write(fd, state->script_source, length) == (ssize_t)length;
    }
    close(fd);
    return ok ? VS_OK : VS_ERR_BACKEND;
}

/* Every call out to KWin is asynchronous, and waiting means running the bus
 * rather than blocking on it: the script answers by calling *us* back, and a
 * blocking sd_bus_call queues incoming method calls instead of serving them.
 * One synchronous call here deadlocks the whole exchange. */

typedef struct {
    bool done;
    bool failed;
    int value;
} kwin_reply;

static int kwin_pump_until(kwin_state *state, const bool *flag, uint64_t timeout_usec)
{
    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);
    while (!*flag) {
        int rc = sd_bus_process(state->bus, NULL);
        if (rc < 0) return VS_ERR_BACKEND;
        if (rc > 0) continue;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t elapsed = (uint64_t)(now.tv_sec - started.tv_sec) * 1000000ull +
                           (uint64_t)((now.tv_nsec - started.tv_nsec) / 1000);
        if (elapsed >= timeout_usec) return VS_ERR_TIMEOUT;
        sd_bus_wait(state->bus, timeout_usec - elapsed);
    }
    return VS_OK;
}

static int kwin_on_int_reply(sd_bus_message *message, void *user_data, sd_bus_error *error)
{
    (void)error;
    kwin_reply *reply = user_data;
    reply->done = true;
    if (sd_bus_message_is_method_error(message, NULL)) {
        const sd_bus_error *failure = sd_bus_message_get_error(message);
        vs_window_log("kwin: call failed: %s", failure && failure->message ? failure->message : "?");
        reply->failed = true;
        return 0;
    }
    if (sd_bus_message_read(message, "i", &reply->value) < 0) reply->failed = true;
    return 0;
}

static int kwin_on_void_reply(sd_bus_message *message, void *user_data, sd_bus_error *error)
{
    (void)error;
    kwin_reply *reply = user_data;
    reply->done = true;
    if (sd_bus_message_is_method_error(message, NULL)) {
        const sd_bus_error *failure = sd_bus_message_get_error(message);
        vs_window_log("kwin: call failed: %s", failure && failure->message ? failure->message : "?");
        reply->failed = true;
    }
    return 0;
}

static int kwin_load_and_run(kwin_state *state, const char *path, const char *plugin_name,
                             int *script_id_out)
{
    kwin_reply load = { 0 };
    int rc = sd_bus_call_method_async(state->bus, NULL, KWIN_SERVICE, KWIN_SCRIPTING_PATH,
                                      KWIN_SCRIPTING_INTERFACE, "loadScript", kwin_on_int_reply,
                                      &load, "ss", path, plugin_name);
    if (rc < 0) return VS_ERR_BACKEND;
    int result = kwin_pump_until(state, &load.done, KWIN_TIMEOUT_USEC);
    if (result != VS_OK) return result;
    if (load.failed || load.value < 0) return VS_ERR_BACKEND;

    char object[64];
    snprintf(object, sizeof(object), "%s/Script%d", KWIN_SCRIPTING_PATH, load.value);
    kwin_reply run = { 0 };
    rc = sd_bus_call_method_async(state->bus, NULL, KWIN_SERVICE, object, KWIN_SCRIPT_INTERFACE,
                                  "run", kwin_on_void_reply, &run, "");
    if (rc < 0) return VS_ERR_BACKEND;
    result = kwin_pump_until(state, &run.done, KWIN_TIMEOUT_USEC);
    if (result != VS_OK) return result;
    if (run.failed) return VS_ERR_BACKEND;

    if (script_id_out) *script_id_out = load.value;
    return VS_OK;
}

static void kwin_unload(kwin_state *state, const char *plugin_name)
{
    kwin_reply unload = { 0 };
    int rc = sd_bus_call_method_async(state->bus, NULL, KWIN_SERVICE, KWIN_SCRIPTING_PATH,
                                      KWIN_SCRIPTING_INTERFACE, "unloadScript", kwin_on_void_reply,
                                      &unload, "s", plugin_name);
    if (rc < 0) return;
    kwin_pump_until(state, &unload.done, KWIN_TIMEOUT_USEC);
}

/** Run one command in KWin and wait for the script's answer. `*json_out` is the
 *  parsed payload, owned by the caller. */
static int kwin_command(kwin_state *state, const char *verb, const char *arguments_json,
                        json_object **json_out)
{
    if (json_out) *json_out = NULL;

    char token[32];
    snprintf(token, sizeof(token), "t%u", ++state->token_counter);
    vs_window_set_string(state->pending_token, sizeof(state->pending_token), token);
    state->answered = false;
    state->answer_ok = false;
    free(state->answer_json);
    state->answer_json = NULL;
    state->answer_message[0] = '\0';

    char prologue[1024];
    snprintf(prologue, sizeof(prologue),
             "var VORSSAINT_COMMAND = {\"token\":\"%s\",\"verb\":\"%s\",\"args\":%s};\n", token,
             verb, arguments_json ? arguments_json : "[]");

    char path[512];
    int result = kwin_write_script(state, prologue, path, sizeof(path));
    if (result != VS_OK) return result;

    char plugin_name[64];
    snprintf(plugin_name, sizeof(plugin_name), "vorssaint-window-%s", token);
    result = kwin_load_and_run(state, path, plugin_name, NULL);
    if (result != VS_OK) {
        unlink(path);
        return result;
    }

    kwin_pump_until(state, &state->answered, KWIN_TIMEOUT_USEC);
    kwin_unload(state, plugin_name);
    unlink(path);

    if (!state->answered) {
        vs_window_log("kwin: %s timed out waiting for the script", verb);
        return VS_ERR_TIMEOUT;
    }
    if (!state->answer_ok) {
        vs_window_log("kwin: %s refused: %s", verb, state->answer_message);
        return strstr(state->answer_message, "no such") ? VS_ERR_NOT_FOUND : VS_ERR_BACKEND;
    }
    if (json_out && state->answer_json) *json_out = json_tokener_parse(state->answer_json);
    return VS_OK;
}

/* ------------------------------------------------------------------ vtable */

static const char *kwin_json_string(json_object *object, const char *key)
{
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value)) return NULL;
    if (!json_object_is_type(value, json_type_string)) return NULL;
    return json_object_get_string(value);
}

static int64_t kwin_json_int(json_object *object, const char *key, int64_t fallback)
{
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value)) return fallback;
    if (json_object_is_type(value, json_type_null)) return fallback;
    return json_object_get_int64(value);
}

static int kwin_list(vs_window_system *self, vs_window_info **windows_out, size_t *count_out)
{
    kwin_state *state = self->impl;
    json_object *payload = NULL;
    int result = kwin_command(state, "list", NULL, &payload);
    if (result != VS_OK) return result;
    if (!payload || !json_object_is_type(payload, json_type_array)) {
        if (payload) json_object_put(payload);
        return VS_ERR_BACKEND;
    }

    vs_window_vec vec = { 0 };
    size_t length = json_object_array_length(payload);
    for (size_t i = 0; i < length; i++) {
        json_object *entry = json_object_array_get_idx(payload, i);
        const char *uuid = kwin_json_string(entry, "id");
        if (!uuid) continue;

        vs_window_info info;
        vs_window_info_init(&info);
        info.id = kwin_remember(state, uuid);
        vs_window_set_string(info.app_id, sizeof(info.app_id), kwin_json_string(entry, "app_id"));
        vs_window_set_string(info.app_name, sizeof(info.app_name),
                             kwin_json_string(entry, "app_name"));
        vs_window_set_string(info.title, sizeof(info.title), kwin_json_string(entry, "title"));
        vs_window_set_string(info.output, sizeof(info.output), kwin_json_string(entry, "output"));
        info.pid = (int32_t)kwin_json_int(entry, "pid", -1);
        info.frame = (vs_rect){
            .x = (int32_t)kwin_json_int(entry, "x", 0),
            .y = (int32_t)kwin_json_int(entry, "y", 0),
            .width = (int32_t)kwin_json_int(entry, "width", 0),
            .height = (int32_t)kwin_json_int(entry, "height", 0),
        };
        info.flags = (uint32_t)kwin_json_int(entry, "flags", 0);
        info.workspace = (int32_t)kwin_json_int(entry, "workspace", -1);
        info.stacking_index = (uint32_t)vec.count;
        /* `workspace.windowList()` is KWin's stacking order, bottom first. */
        info.stacking_valid = true;
        if (!vs_window_vec_push(&vec, &info)) {
            json_object_put(payload);
            vs_window_vec_free(&vec);
            return VS_ERR_NO_MEM;
        }
    }
    json_object_put(payload);

    *windows_out = vec.items;
    *count_out = vec.count;
    return VS_OK;
}

static int kwin_window_command(vs_window_system *self, const char *verb, vs_window_id id,
                               const char *extra_json, json_object **json_out)
{
    kwin_state *state = self->impl;
    const char *uuid = kwin_uuid(state, id);
    if (!uuid) return VS_ERR_NOT_FOUND;
    char arguments[256];
    snprintf(arguments, sizeof(arguments), "[\"%s\"%s%s]", uuid, extra_json ? "," : "",
             extra_json ? extra_json : "");
    return kwin_command(state, verb, arguments, json_out);
}

static int kwin_activate(vs_window_system *self, vs_window_id id)
{
    return kwin_window_command(self, "activate", id, NULL, NULL);
}

static int kwin_close(vs_window_system *self, vs_window_id id)
{
    return kwin_window_command(self, "close", id, NULL, NULL);
}

static int kwin_set_minimized(vs_window_system *self, vs_window_id id, bool minimized)
{
    return kwin_window_command(self, "setMinimized", id, minimized ? "true" : "false", NULL);
}

static int kwin_geometry(vs_window_system *self, vs_window_id id, vs_rect *frame_out)
{
    if (!frame_out) return VS_ERR_INVALID;
    json_object *payload = NULL;
    int result = kwin_window_command(self, "geometry", id, NULL, &payload);
    if (result != VS_OK) return result;
    if (!payload) return VS_ERR_BACKEND;
    *frame_out = (vs_rect){
        .x = (int32_t)kwin_json_int(payload, "x", 0),
        .y = (int32_t)kwin_json_int(payload, "y", 0),
        .width = (int32_t)kwin_json_int(payload, "width", 0),
        .height = (int32_t)kwin_json_int(payload, "height", 0),
    };
    json_object_put(payload);
    return VS_OK;
}

static int kwin_move_resize(vs_window_system *self, vs_window_id id, vs_rect frame, int32_t tolerance)
{
    if (frame.width <= 0 || frame.height <= 0) return VS_ERR_INVALID;
    char arguments[128];
    snprintf(arguments, sizeof(arguments), "%d,%d,%d,%d", frame.x, frame.y, frame.width,
             frame.height);
    json_object *payload = NULL;
    int result = kwin_window_command(self, "moveResize", id, arguments, &payload);
    if (result != VS_OK) return result;

    vs_rect applied = frame;
    if (payload) {
        applied = (vs_rect){
            .x = (int32_t)kwin_json_int(payload, "x", 0),
            .y = (int32_t)kwin_json_int(payload, "y", 0),
            .width = (int32_t)kwin_json_int(payload, "width", 0),
            .height = (int32_t)kwin_json_int(payload, "height", 0),
        };
        json_object_put(payload);
    }
    if (tolerance < 0) return VS_OK;
    /* The script already read the frame back inside KWin; confirm it again from
     * here so a later layout pass cannot hide a refusal. */
    if (vs_rect_close(applied, frame, tolerance)) return VS_OK;
    vs_rect actual;
    if (kwin_geometry(self, id, &actual) == VS_OK && vs_rect_close(actual, frame, tolerance)) {
        return VS_OK;
    }
    return VS_ERR_NOT_APPLIED;
}

static int kwin_current_workspace(vs_window_system *self, int32_t *workspace_out)
{
    if (!workspace_out) return VS_ERR_INVALID;
    json_object *payload = NULL;
    int result = kwin_command(self->impl, "currentWorkspace", NULL, &payload);
    if (result != VS_OK) return result;
    if (!payload) return VS_ERR_BACKEND;
    *workspace_out = (int32_t)json_object_get_int64(payload);
    json_object_put(payload);
    return VS_OK;
}

static int kwin_set_workspace(vs_window_system *self, int32_t workspace)
{
    if (workspace < 0) return VS_ERR_INVALID;
    char arguments[32];
    snprintf(arguments, sizeof(arguments), "[%d]", workspace);
    json_object *payload = NULL;
    int result = kwin_command(self->impl, "setWorkspace", arguments, &payload);
    if (result != VS_OK) return result;
    int32_t actual = payload ? (int32_t)json_object_get_int64(payload) : -1;
    if (payload) json_object_put(payload);
    return actual == workspace ? VS_OK : VS_ERR_NOT_APPLIED;
}

static int kwin_set_event_callback(vs_window_system *self, vs_window_event_cb callback,
                                   void *user_data)
{
    kwin_state *state = self->impl;
    state->callback = callback;
    state->callback_data = user_data;
    return VS_OK;
}

static int kwin_event_fd(vs_window_system *self)
{
    kwin_state *state = self->impl;
    return sd_bus_get_fd(state->bus);
}

static int kwin_dispatch(vs_window_system *self)
{
    kwin_state *state = self->impl;
    state->emitted = 0;
    for (;;) {
        int rc = sd_bus_process(state->bus, NULL);
        if (rc < 0) {
            kwin_emit(state, VS_WINDOW_EVENT_BACKEND_LOST, 0, -1);
            return VS_ERR_BACKEND;
        }
        if (rc == 0) break;
    }
    return state->emitted;
}

static void kwin_destroy(vs_window_system *self)
{
    kwin_state *state = self->impl;
    if (state) {
        if (state->event_script_id >= 0) kwin_unload(state, "vorssaint-window-events");
        if (state->sink_slot) sd_bus_slot_unref(state->sink_slot);
        if (state->bus) {
            sd_bus_release_name(state->bus, KWIN_SINK_SERVICE);
            sd_bus_unref(state->bus);
        }
        free(state->script_source);
        free(state->answer_json);
        free(state->ids);
        free(state);
    }
    free(self);
}

/* ------------------------------------------------------------------ create */

static char *kwin_read_file(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    size_t capacity = 8192;
    size_t used = 0;
    char *buffer = malloc(capacity);
    if (!buffer) {
        close(fd);
        return NULL;
    }
    for (;;) {
        if (used + 1 >= capacity) {
            char *grown = realloc(buffer, capacity * 2);
            if (!grown) {
                free(buffer);
                close(fd);
                return NULL;
            }
            buffer = grown;
            capacity *= 2;
        }
        ssize_t got = read(fd, buffer + used, capacity - used - 1);
        if (got < 0) {
            if (errno == EINTR) continue;
            free(buffer);
            close(fd);
            return NULL;
        }
        if (got == 0) break;
        used += (size_t)got;
    }
    buffer[used] = '\0';
    close(fd);
    return buffer;
}

/** The script ships beside the library. `$VORSSAINT_KWIN_SCRIPT` overrides it
 *  for tests and for running out of a build tree. */
static char *kwin_load_source(void)
{
    const char *override = getenv("VORSSAINT_KWIN_SCRIPT");
    if (override && *override) return kwin_read_file(override);

    static const char *candidates[] = {
        "/usr/share/vorssaint/kwin/vorssaint-window.js",
        "/usr/local/share/vorssaint/kwin/vorssaint-window.js",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        char *source = kwin_read_file(candidates[i]);
        if (source) return source;
    }
    return NULL;
}

vs_window_system *vs_window_backend_kwin_create(int *result_out)
{
    if (result_out) *result_out = VS_ERR_NO_BACKEND;

    sd_bus *bus = NULL;
    if (sd_bus_open_user(&bus) < 0) return NULL;
    if (!vs_bridge_name_has_owner(bus, KWIN_SERVICE)) {
        sd_bus_unref(bus);
        return NULL;
    }

    char *source = kwin_load_source();
    if (!source) {
        vs_window_log("kwin: %s is on the bus but vorssaint-window.js is not installed",
                      KWIN_SERVICE);
        sd_bus_unref(bus);
        if (result_out) *result_out = VS_ERR_NO_BACKEND;
        return NULL;
    }

    kwin_state *state = calloc(1, sizeof(*state));
    if (!state) {
        free(source);
        sd_bus_unref(bus);
        if (result_out) *result_out = VS_ERR_NO_MEM;
        return NULL;
    }
    state->bus = bus;
    state->script_source = source;
    state->event_script_id = -1;

    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime) runtime = "/tmp";
    snprintf(state->script_directory, sizeof(state->script_directory), "%s/vorssaint", runtime);
    mkdir(state->script_directory, 0700);

    int rc = sd_bus_add_object_vtable(bus, &state->sink_slot, KWIN_SINK_PATH, KWIN_SINK_INTERFACE,
                                      kwin_sink_vtable, state);
    if (rc < 0) {
        vs_window_log("kwin: cannot export %s: %s", KWIN_SINK_PATH, strerror(-rc));
        free(state->script_source);
        sd_bus_unref(bus);
        free(state);
        if (result_out) *result_out = VS_ERR_BACKEND;
        return NULL;
    }
    rc = sd_bus_request_name(bus, KWIN_SINK_SERVICE, 0);
    if (rc < 0) {
        vs_window_log("kwin: cannot own %s: %s", KWIN_SINK_SERVICE, strerror(-rc));
        sd_bus_slot_unref(state->sink_slot);
        state->sink_slot = NULL;
        free(state->script_source);
        sd_bus_unref(bus);
        free(state);
        if (result_out) *result_out = VS_ERR_BACKEND;
        return NULL;
    }

    vs_window_system *system = calloc(1, sizeof(*system));
    if (!system) {
        sd_bus_slot_unref(state->sink_slot);
        sd_bus_release_name(bus, KWIN_SINK_SERVICE);
        free(state->script_source);
        sd_bus_unref(bus);
        free(state);
        if (result_out) *result_out = VS_ERR_NO_MEM;
        return NULL;
    }

    system->name = "kwin";
    system->impl = state;
    system->capabilities = VS_WINDOW_CAN_LIST | VS_WINDOW_CAN_ACTIVATE | VS_WINDOW_CAN_CLOSE |
                           VS_WINDOW_CAN_MINIMIZE | VS_WINDOW_CAN_MOVE_RESIZE |
                           VS_WINDOW_CAN_WORKSPACE_SWITCH;
    system->list = kwin_list;
    system->free_list = vs_window_free_list_default;
    system->activate = kwin_activate;
    system->close = kwin_close;
    system->set_minimized = kwin_set_minimized;
    system->move_resize = kwin_move_resize;
    system->geometry = kwin_geometry;
    system->current_workspace = kwin_current_workspace;
    system->set_workspace = kwin_set_workspace;
    system->set_event_callback = kwin_set_event_callback;
    system->event_fd = kwin_event_fd;
    system->dispatch = kwin_dispatch;
    system->destroy = kwin_destroy;

    /* The persistent half: one long-lived copy of the same script pushing
     * events. Without it the backend still answers every request, so a failure
     * costs live events, not the feature. */
    state->token_counter++;
    char path[512];
    if (kwin_write_script(state, NULL, path, sizeof(path)) == VS_OK) {
        if (kwin_load_and_run(state, path, "vorssaint-window-events", &state->event_script_id) ==
            VS_OK) {
            system->capabilities |= VS_WINDOW_HAS_LIVE_EVENTS;
        }
    }

    if (result_out) *result_out = VS_OK;
    return system;
}
