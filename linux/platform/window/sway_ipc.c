/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 */

#include "sway_ipc.h"
#include "vs_window_internal.h"

#include <errno.h>
#include <json-c/json.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static const char sway_magic[6] = { 'i', '3', '-', 'i', 'p', 'c' };

typedef struct {
    char magic[6];
    uint32_t length;
    uint32_t type;
} __attribute__((packed)) sway_header;

int sway_ipc_open(sway_ipc *ipc, const char *socket_path)
{
    ipc->fd = -1;
    if (!socket_path) socket_path = getenv("SWAYSOCK");
    if (!socket_path || !*socket_path) return VS_ERR_NO_BACKEND;

    struct sockaddr_un address = { 0 };
    address.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof(address.sun_path)) return VS_ERR_INVALID;
    strcpy(address.sun_path, socket_path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return VS_ERR_BACKEND;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return VS_ERR_BACKEND;
    }
    ipc->fd = fd;
    return VS_OK;
}

void sway_ipc_close(sway_ipc *ipc)
{
    if (ipc->fd >= 0) close(ipc->fd);
    ipc->fd = -1;
}

static bool sway_write_all(int fd, const void *buffer, size_t length)
{
    const char *bytes = buffer;
    while (length > 0) {
        ssize_t written = write(fd, bytes, length);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        bytes += written;
        length -= (size_t)written;
    }
    return true;
}

static bool sway_read_all(int fd, void *buffer, size_t length)
{
    char *bytes = buffer;
    while (length > 0) {
        ssize_t got = read(fd, bytes, length);
        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (got == 0) return false;
        bytes += got;
        length -= (size_t)got;
    }
    return true;
}

int sway_ipc_request(sway_ipc *ipc, uint32_t type, const char *payload, char **payload_out)
{
    if (ipc->fd < 0) return VS_ERR_BACKEND;
    if (payload_out) *payload_out = NULL;

    size_t payload_length = payload ? strlen(payload) : 0;
    sway_header header;
    memcpy(header.magic, sway_magic, sizeof(sway_magic));
    header.length = (uint32_t)payload_length;
    header.type = type;

    if (!sway_write_all(ipc->fd, &header, sizeof(header))) return VS_ERR_BACKEND;
    if (payload_length && !sway_write_all(ipc->fd, payload, payload_length)) return VS_ERR_BACKEND;

    sway_header reply;
    if (!sway_read_all(ipc->fd, &reply, sizeof(reply))) return VS_ERR_BACKEND;
    if (memcmp(reply.magic, sway_magic, sizeof(sway_magic)) != 0) return VS_ERR_BACKEND;

    char *body = malloc((size_t)reply.length + 1);
    if (!body) return VS_ERR_NO_MEM;
    if (reply.length && !sway_read_all(ipc->fd, body, reply.length)) {
        free(body);
        return VS_ERR_BACKEND;
    }
    body[reply.length] = '\0';

    if (payload_out) *payload_out = body;
    else free(body);
    return VS_OK;
}

/* ------------------------------------------------------------------ tree */

typedef struct {
    sway_container *items;
    size_t count;
    size_t capacity;
} sway_vec;

static bool sway_vec_push(sway_vec *vec, const sway_container *container)
{
    if (vec->count == vec->capacity) {
        size_t capacity = vec->capacity ? vec->capacity * 2 : 16;
        sway_container *items = realloc(vec->items, capacity * sizeof(*items));
        if (!items) return false;
        vec->items = items;
        vec->capacity = capacity;
    }
    vec->items[vec->count++] = *container;
    return true;
}

static const char *sway_string(json_object *object, const char *key)
{
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value)) return NULL;
    if (!json_object_is_type(value, json_type_string)) return NULL;
    return json_object_get_string(value);
}

static bool sway_bool(json_object *object, const char *key)
{
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value)) return false;
    return json_object_get_boolean(value);
}

static int64_t sway_int(json_object *object, const char *key, int64_t fallback)
{
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value)) return fallback;
    if (json_object_is_type(value, json_type_null)) return fallback;
    return json_object_get_int64(value);
}

static void sway_rect(json_object *object, const char *key, vs_rect *out)
{
    json_object *rect = NULL;
    if (!json_object_object_get_ex(object, key, &rect)) return;
    out->x = (int32_t)sway_int(rect, "x", 0);
    out->y = (int32_t)sway_int(rect, "y", 0);
    out->width = (int32_t)sway_int(rect, "width", 0);
    out->height = (int32_t)sway_int(rect, "height", 0);
}

static void sway_walk(json_object *node, const char *output, int32_t workspace, sway_vec *vec)
{
    const char *type = sway_string(node, "type");
    if (type && strcmp(type, "output") == 0) output = sway_string(node, "name");
    if (type && strcmp(type, "workspace") == 0) workspace = (int32_t)sway_int(node, "num", -1);

    /* sway reports `pid` only on views; split containers carry a JSON null. */
    json_object *pid_value = NULL;
    bool is_view = json_object_object_get_ex(node, "pid", &pid_value) &&
                   !json_object_is_type(pid_value, json_type_null);

    if (is_view) {
        sway_container container = { 0 };
        container.con_id = sway_int(node, "id", 0);
        container.pid = (int32_t)sway_int(node, "pid", -1);
        container.workspace = workspace;
        container.focused = sway_bool(node, "focused");
        container.visible = sway_bool(node, "visible");
        json_object *fullscreen = NULL;
        if (json_object_object_get_ex(node, "fullscreen_mode", &fullscreen)) {
            container.fullscreen = json_object_get_int(fullscreen) != 0;
        }
        const char *app_id = sway_string(node, "app_id");
        if (!app_id) {
            json_object *properties = NULL;
            if (json_object_object_get_ex(node, "window_properties", &properties)) {
                app_id = sway_string(properties, "class");
            }
        }
        vs_window_set_string(container.app_id, sizeof(container.app_id), app_id);
        vs_window_set_string(container.title, sizeof(container.title), sway_string(node, "name"));
        vs_window_set_string(container.output, sizeof(container.output), output);
        sway_rect(node, "rect", &container.frame);
        sway_vec_push(vec, &container);
    }

    const char *lists[] = { "nodes", "floating_nodes" };
    for (size_t i = 0; i < 2; i++) {
        json_object *children = NULL;
        if (!json_object_object_get_ex(node, lists[i], &children)) continue;
        size_t length = json_object_array_length(children);
        for (size_t j = 0; j < length; j++) {
            sway_walk(json_object_array_get_idx(children, j), output, workspace, vec);
        }
    }
}

int sway_ipc_containers(sway_ipc *ipc, sway_container **out, size_t *count_out)
{
    char *body = NULL;
    int result = sway_ipc_request(ipc, SWAY_IPC_GET_TREE, NULL, &body);
    if (result != VS_OK) return result;

    json_object *tree = json_tokener_parse(body);
    free(body);
    if (!tree) return VS_ERR_BACKEND;

    sway_vec vec = { 0 };
    sway_walk(tree, NULL, -1, &vec);
    json_object_put(tree);

    *out = vec.items;
    *count_out = vec.count;
    return VS_OK;
}

int sway_ipc_current_workspace(sway_ipc *ipc, int32_t *workspace_out)
{
    char *body = NULL;
    int result = sway_ipc_request(ipc, SWAY_IPC_GET_WORKSPACES, NULL, &body);
    if (result != VS_OK) return result;

    json_object *workspaces = json_tokener_parse(body);
    free(body);
    if (!workspaces || !json_object_is_type(workspaces, json_type_array)) {
        if (workspaces) json_object_put(workspaces);
        return VS_ERR_BACKEND;
    }

    int found = VS_ERR_NOT_FOUND;
    size_t length = json_object_array_length(workspaces);
    for (size_t i = 0; i < length; i++) {
        json_object *workspace = json_object_array_get_idx(workspaces, i);
        if (!sway_bool(workspace, "focused")) continue;
        *workspace_out = (int32_t)sway_int(workspace, "num", -1);
        found = VS_OK;
        break;
    }
    json_object_put(workspaces);
    return found;
}

int sway_ipc_run(sway_ipc *ipc, const char *command)
{
    char *body = NULL;
    int result = sway_ipc_request(ipc, SWAY_IPC_RUN_COMMAND, command, &body);
    if (result != VS_OK) return result;

    json_object *results = json_tokener_parse(body);
    free(body);
    if (!results || !json_object_is_type(results, json_type_array)) {
        if (results) json_object_put(results);
        return VS_ERR_BACKEND;
    }

    int outcome = VS_OK;
    size_t length = json_object_array_length(results);
    for (size_t i = 0; i < length; i++) {
        json_object *entry = json_object_array_get_idx(results, i);
        if (sway_bool(entry, "success")) continue;
        const char *error = sway_string(entry, "error");
        vs_window_log("sway: command refused: %s", error ? error : "(no reason given)");
        outcome = VS_ERR_BACKEND;
    }
    json_object_put(results);
    return outcome;
}
