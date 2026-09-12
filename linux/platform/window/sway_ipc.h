/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * Minimal i3/sway IPC client. The foreign-toplevel protocol carries no
 * geometry, no pid and no workspace, and no Wayland protocol lets one client
 * move another client's window; on sway the IPC socket supplies all four.
 */

#ifndef VS_SWAY_IPC_H
#define VS_SWAY_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vorssaint_platform.h"

#define SWAY_IPC_RUN_COMMAND 0
#define SWAY_IPC_GET_WORKSPACES 1
#define SWAY_IPC_GET_TREE 4

typedef struct {
    int fd;
} sway_ipc;

/** Connect to `$SWAYSOCK`, or to `socket_path` when it is non-NULL. Returns
 *  VS_OK or a negative `vs_result`. */
int sway_ipc_open(sway_ipc *ipc, const char *socket_path);
void sway_ipc_close(sway_ipc *ipc);

/** Round-trip one message. `*payload_out` is malloc'd and NUL-terminated. */
int sway_ipc_request(sway_ipc *ipc, uint32_t type, const char *payload, char **payload_out);

/** One container from `get_tree`, flattened to what the window layer needs. */
typedef struct {
    int64_t con_id;
    int32_t pid;
    char app_id[VS_WINDOW_APP_ID_MAX];
    char title[VS_WINDOW_TITLE_MAX];
    char output[VS_WINDOW_OUTPUT_MAX];
    int32_t workspace;
    vs_rect frame;
    bool focused;
    bool visible;
    bool fullscreen;
} sway_container;

/** Every view in the tree, in the tree's own order. Caller frees `*out`. */
int sway_ipc_containers(sway_ipc *ipc, sway_container **out, size_t *count_out);

/** Index of the focused workspace, or a negative `vs_result`. */
int sway_ipc_current_workspace(sway_ipc *ipc, int32_t *workspace_out);

/** Run a sway command; returns VS_ERR_BACKEND when any sub-command failed. */
int sway_ipc_run(sway_ipc *ipc, const char *command);

#endif /* VS_SWAY_IPC_H */
