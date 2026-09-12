/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * `vs-window`: the CLI harness for the window backends. Everything the Swift
 * side will do through the vtable can be done from a shell here, which is what
 * the ctest suites and later QA drive.
 */

#include "vorssaint_platform.h"

#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void print_usage(void)
{
    fprintf(stderr,
            "usage: vs-window [--backend NAME] COMMAND [ARGS]\n"
            "\n"
            "  backends            list the backends compiled in\n"
            "  probe               name the backend chosen for this session and its capabilities\n"
            "  list                one window per line, tab separated\n"
            "  watch [SECONDS]     print events as they arrive (default 5)\n"
            "  activate ID\n"
            "  close ID\n"
            "  minimize ID [0|1]\n"
            "  move ID X Y W H [TOLERANCE]\n"
            "  geometry ID\n"
            "  workspace [INDEX]   read, or switch to, the current workspace\n");
}

static const struct {
    uint32_t bit;
    const char *name;
} capability_names[] = {
    { VS_WINDOW_CAN_LIST, "can_list" },
    { VS_WINDOW_CAN_ACTIVATE, "can_activate" },
    { VS_WINDOW_CAN_CLOSE, "can_close" },
    { VS_WINDOW_CAN_MINIMIZE, "can_minimize" },
    { VS_WINDOW_CAN_MOVE_RESIZE, "can_move_resize" },
    { VS_WINDOW_CAN_WORKSPACE_SWITCH, "can_workspace_switch" },
    { VS_WINDOW_HAS_LIVE_EVENTS, "has_live_events" },
    { VS_WINDOW_HAS_PREVIEWS, "has_previews" },
};

static const struct {
    uint32_t bit;
    const char *name;
} flag_names[] = {
    { VS_WINDOW_MINIMIZED, "minimized" },
    { VS_WINDOW_FOCUSED, "focused" },
    { VS_WINDOW_FULLSCREEN, "fullscreen" },
    { VS_WINDOW_MAXIMIZED, "maximized" },
    { VS_WINDOW_ON_CURRENT_WORKSPACE, "on-current-workspace" },
    { VS_WINDOW_HAS_GEOMETRY, "has-geometry" },
    { VS_WINDOW_HAS_PID, "has-pid" },
    { VS_WINDOW_ON_SCREEN, "on-screen" },
};

static void print_flags(uint32_t flags)
{
    bool first = true;
    for (size_t i = 0; i < sizeof(flag_names) / sizeof(flag_names[0]); i++) {
        if (!(flags & flag_names[i].bit)) continue;
        printf("%s%s", first ? "" : ",", flag_names[i].name);
        first = false;
    }
    if (first) printf("-");
}

static void print_window(const vs_window_info *window)
{
    printf("%" PRIu64 "\t%s\t%s\t%d\t%d,%d %dx%d\t", window->id,
           window->app_id[0] ? window->app_id : "-", window->title[0] ? window->title : "-",
           window->pid, window->frame.x, window->frame.y, window->frame.width,
           window->frame.height);
    print_flags(window->flags);
    printf("\t%d\t%s\t%u%s\n", window->workspace, window->output[0] ? window->output : "-",
           window->stacking_index, window->stacking_valid ? "" : "?");
}

static const char *event_name(vs_window_event_type type)
{
    switch (type) {
    case VS_WINDOW_EVENT_ADDED: return "added";
    case VS_WINDOW_EVENT_REMOVED: return "removed";
    case VS_WINDOW_EVENT_CHANGED: return "changed";
    case VS_WINDOW_EVENT_ACTIVATED: return "activated";
    case VS_WINDOW_EVENT_WORKSPACE_CHANGED: return "workspace";
    case VS_WINDOW_EVENT_BACKEND_LOST: return "backend-lost";
    default: return "?";
    }
}

static void on_event(const vs_window_event *event, void *user_data)
{
    (void)user_data;
    printf("event\t%s\t%" PRIu64 "\t%s\t%d\n", event_name(event->type), event->id,
           event->info && event->info->title[0] ? event->info->title : "-", event->workspace);
    fflush(stdout);
}

/** Window ids are only as stable as the backend makes them: X11 ids are XIDs
 *  and outlive any process, but a foreign-toplevel handle is numbered per
 *  connection, so an id printed by one `vs-window list` means nothing to the
 *  next invocation. `@name` resolves an app_id or title inside this process
 *  instead, which is what the shell tests address windows by. */
static bool resolve_id(vs_window_system *system, const char *argument, vs_window_id *id_out)
{
    if (argument[0] != '@') {
        *id_out = strtoull(argument, NULL, 0);
        return true;
    }
    const char *wanted = argument + 1;

    vs_window_info *windows = NULL;
    size_t count = 0;
    int result = system->list(system, &windows, &count);
    if (result != VS_OK) {
        fprintf(stderr, "vs-window: list: %s\n", vs_result_string(result));
        return false;
    }
    size_t matches = 0;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(windows[i].app_id, wanted) != 0 && strcmp(windows[i].title, wanted) != 0) continue;
        *id_out = windows[i].id;
        matches++;
    }
    system->free_list(system, windows, count);
    if (matches == 1) return true;
    fprintf(stderr, "vs-window: %zu windows match '%s'\n", matches, wanted);
    return false;
}

static int fail(const char *what, int result)
{
    fprintf(stderr, "vs-window: %s: %s\n", what, vs_result_string(result));
    return 1;
}

int main(int argc, char **argv)
{
    const char *backend = NULL;
    int index = 1;
    while (index < argc && strncmp(argv[index], "--", 2) == 0) {
        if (strcmp(argv[index], "--backend") == 0 && index + 1 < argc) {
            backend = argv[++index];
            index++;
            continue;
        }
        print_usage();
        return 2;
    }
    if (index >= argc) {
        print_usage();
        return 2;
    }
    const char *command = argv[index++];

    if (strcmp(command, "backends") == 0) {
        for (const char *const *name = vs_window_backend_names(); *name; name++) printf("%s\n", *name);
        return 0;
    }

    int result = VS_OK;
    vs_window_system *system = vs_window_system_create(backend, &result);
    if (!system) return fail("no backend", result);

    int status = 0;
    if (strcmp(command, "probe") == 0) {
        printf("backend\t%s\n", system->name);
        for (size_t i = 0; i < sizeof(capability_names) / sizeof(capability_names[0]); i++) {
            printf("%s\t%s\n", capability_names[i].name,
                   (system->capabilities & capability_names[i].bit) ? "yes" : "no");
        }
    } else if (strcmp(command, "list") == 0) {
        vs_window_info *windows = NULL;
        size_t count = 0;
        result = system->list(system, &windows, &count);
        if (result != VS_OK) status = fail("list", result);
        else {
            for (size_t i = 0; i < count; i++) print_window(&windows[i]);
            system->free_list(system, windows, count);
        }
    } else if (strcmp(command, "watch") == 0) {
        double seconds = index < argc ? atof(argv[index]) : 5.0;
        system->set_event_callback(system, on_event, NULL);
        int fd = system->event_fd(system);
        struct timespec started;
        clock_gettime(CLOCK_MONOTONIC, &started);
        for (;;) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (double)(now.tv_sec - started.tv_sec) +
                             (double)(now.tv_nsec - started.tv_nsec) / 1e9;
            if (elapsed >= seconds) break;
            if (fd >= 0) {
                struct pollfd descriptor = { .fd = fd, .events = POLLIN };
                poll(&descriptor, 1, 100);
            } else {
                struct timespec pause = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
                nanosleep(&pause, NULL);
            }
            if (system->dispatch(system) < 0) break;
        }
    } else if (strcmp(command, "activate") == 0 && index < argc) {
        vs_window_id id = 0;
        if (!resolve_id(system, argv[index], &id)) status = 1;
        else if ((result = system->activate(system, id)) != VS_OK) status = fail("activate", result);
    } else if (strcmp(command, "close") == 0 && index < argc) {
        vs_window_id id = 0;
        if (!resolve_id(system, argv[index], &id)) status = 1;
        else if ((result = system->close(system, id)) != VS_OK) status = fail("close", result);
    } else if (strcmp(command, "minimize") == 0 && index < argc) {
        vs_window_id id = 0;
        if (!resolve_id(system, argv[index++], &id)) status = 1;
        else {
            bool minimized = index < argc ? atoi(argv[index]) != 0 : true;
            result = system->set_minimized(system, id, minimized);
            if (result != VS_OK) status = fail("minimize", result);
        }
    } else if (strcmp(command, "geometry") == 0 && index < argc) {
        vs_window_id id = 0;
        vs_rect frame;
        if (!resolve_id(system, argv[index], &id)) status = 1;
        else if ((result = system->geometry(system, id, &frame)) != VS_OK) {
            status = fail("geometry", result);
        } else {
            printf("%d,%d %dx%d\n", frame.x, frame.y, frame.width, frame.height);
        }
    } else if (strcmp(command, "move") == 0 && index + 4 < argc) {
        vs_window_id id = 0;
        if (!resolve_id(system, argv[index++], &id)) {
            system->destroy(system);
            return 1;
        }
        vs_rect frame = { .x = atoi(argv[index]), .y = atoi(argv[index + 1]),
                          .width = atoi(argv[index + 2]), .height = atoi(argv[index + 3]) };
        index += 4;
        int32_t tolerance = index < argc ? (int32_t)atoi(argv[index]) : 2;
        result = system->move_resize(system, id, frame, tolerance);
        if (result != VS_OK) status = fail("move", result);
        else {
            vs_rect actual;
            if (system->geometry(system, id, &actual) == VS_OK) {
                printf("%d,%d %dx%d\n", actual.x, actual.y, actual.width, actual.height);
            }
        }
    } else if (strcmp(command, "workspace") == 0) {
        if (index < argc) {
            result = system->set_workspace(system, atoi(argv[index]));
            if (result != VS_OK) status = fail("workspace", result);
        }
        int32_t workspace = -1;
        result = system->current_workspace(system, &workspace);
        if (result != VS_OK) status = fail("workspace", result);
        else printf("%d\n", workspace);
    } else {
        print_usage();
        status = 2;
    }

    system->destroy(system);
    return status;
}
