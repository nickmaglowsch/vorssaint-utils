/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * Enumeration: what can be captured, and what audio exists to capture with.
 *
 * The ScreenCast portal has no enumeration call at all -- it tells you which
 * *kinds* of source exist (`AvailableSourceTypes`) and then shows its own
 * chooser. So the monitor list comes from the display server itself, through
 * `wl_output`, which is the same list the portal's chooser draws from and the
 * same names xdg-desktop-portal-wlr matches its `output_name` against.
 *
 * There is deliberately no window list here. On a session whose
 * `AvailableSourceTypes` has no WINDOW bit there is nothing to list, and on one
 * that has it the portal, not the app, decides which window is shared -- a list
 * the app drew would be a list the app cannot act on. WP-C1's `vs_window_system`
 * is the window list; a session that can preview windows joins the two through
 * `VS_WINDOW_HAS_PREVIEWS`.
 */

#include <stdlib.h>
#include <string.h>

#include <pipewire/pipewire.h>
#include <wayland-client.h>

#include "vs_capture_internal.h"

/* ------------------------------------------------------------- wl_output */

typedef struct {
    struct wl_output *output;
    uint32_t global_name;
    char name[VS_CAPTURE_NAME_MAX];
    char description[VS_CAPTURE_DESC_MAX];
    int32_t x, y, width, height, refresh_mhz, scale;
    bool have_mode;
    bool have_scale;
} output_entry;

typedef struct {
    output_entry *entries;
    size_t count;
    size_t capacity;
    struct wl_registry *registry;
} output_scan;

static void handle_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
                            int32_t physical_width, int32_t physical_height,
                            int32_t subpixel, const char *make, const char *model,
                            int32_t transform)
{
    (void)output; (void)physical_width; (void)physical_height; (void)subpixel;
    (void)transform;
    output_entry *entry = data;
    entry->x = x;
    entry->y = y;
    /* wl_output version 4 sends a proper description; before that, make and
     * model are all there is, and they are often "Unknown". */
    if (entry->description[0] == '\0' && make && model)
        snprintf(entry->description, sizeof(entry->description), "%s %s", make, model);
}

static void handle_mode(void *data, struct wl_output *output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh)
{
    (void)output;
    output_entry *entry = data;
    if (!(flags & WL_OUTPUT_MODE_CURRENT)) return;
    entry->width = width;
    entry->height = height;
    entry->refresh_mhz = refresh;
    entry->have_mode = true;
}

static void handle_done(void *data, struct wl_output *output)
{
    (void)data; (void)output;
}

static void handle_scale(void *data, struct wl_output *output, int32_t factor)
{
    (void)output;
    output_entry *entry = data;
    entry->scale = factor;
    entry->have_scale = true;
}

static void handle_name(void *data, struct wl_output *output, const char *name)
{
    (void)output;
    output_entry *entry = data;
    if (name) snprintf(entry->name, sizeof(entry->name), "%s", name);
}

static void handle_description(void *data, struct wl_output *output, const char *description)
{
    (void)output;
    output_entry *entry = data;
    if (description)
        snprintf(entry->description, sizeof(entry->description), "%s", description);
}

static const struct wl_output_listener output_listener = {
    .geometry = handle_geometry,
    .mode = handle_mode,
    .done = handle_done,
    .scale = handle_scale,
    .name = handle_name,
    .description = handle_description,
};

static void handle_global(void *data, struct wl_registry *registry, uint32_t name,
                          const char *interface, uint32_t version)
{
    output_scan *scan = data;
    if (strcmp(interface, wl_output_interface.name) != 0) return;
    if (scan->count == scan->capacity) {
        size_t capacity = scan->capacity ? scan->capacity * 2 : 4;
        output_entry *grown = realloc(scan->entries, capacity * sizeof(*grown));
        if (!grown) return;
        scan->entries = grown;
        scan->capacity = capacity;
    }
    output_entry *entry = &scan->entries[scan->count];
    memset(entry, 0, sizeof(*entry));
    entry->global_name = name;
    /* Version 4 is where `name` and `description` arrive; anything older still
     * gives geometry and mode, and the source simply has no connector name. */
    uint32_t bind_version = version < 4 ? version : 4;
    entry->output = wl_registry_bind(registry, name, &wl_output_interface, bind_version);
    if (!entry->output) return;
    wl_output_add_listener(entry->output, &output_listener, entry);
    scan->count++;
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

static int enumerate_monitors(vs_capture_source **sources_out, size_t *count_out)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        VS_LOG("no Wayland display: monitors cannot be enumerated here");
        return VS_ERR_UNSUPPORTED;
    }
    output_scan scan = { 0 };
    scan.registry = wl_display_get_registry(display);
    wl_registry_add_listener(scan.registry, &registry_listener, &scan);
    /* One roundtrip brings the globals in, the second the per-output events
     * the binds in the first one triggered. */
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    vs_capture_source *sources = NULL;
    if (scan.count) {
        sources = calloc(scan.count, sizeof(*sources));
        if (!sources) {
            for (size_t i = 0; i < scan.count; i++) wl_output_destroy(scan.entries[i].output);
            free(scan.entries);
            wl_registry_destroy(scan.registry);
            wl_display_disconnect(display);
            return VS_ERR_NO_MEM;
        }
    }
    for (size_t i = 0; i < scan.count; i++) {
        output_entry *entry = &scan.entries[i];
        vs_capture_source *source = &sources[i];
        source->id = entry->global_name;
        source->type = VS_CAPTURE_SOURCE_MONITOR;
        snprintf(source->name, sizeof(source->name), "%s", entry->name);
        snprintf(source->description, sizeof(source->description), "%s", entry->description);
        source->bounds.x = entry->x;
        source->bounds.y = entry->y;
        source->bounds.width = entry->width;
        source->bounds.height = entry->height;
        source->bounds_valid = entry->have_mode;
        /* -1, not 0: an output that never sent a mode has no refresh rate, and
         * 0 Hz would read as one. Same for the scale. */
        source->refresh_mhz = entry->have_mode ? entry->refresh_mhz : -1;
        source->scale = entry->have_scale ? entry->scale : -1.0;
        wl_output_destroy(entry->output);
    }
    free(scan.entries);
    wl_registry_destroy(scan.registry);
    wl_display_disconnect(display);

    *sources_out = sources;
    *count_out = scan.count;
    return VS_OK;
}

int vs_capture_enumerate_sources(vs_capture_engine *engine, uint32_t types,
                                 vs_capture_source **sources_out, size_t *count_out)
{
    if (!engine || !sources_out || !count_out) return VS_ERR_INVALID;
    *sources_out = NULL;
    *count_out = 0;
    uint32_t wanted = types ? types : engine->source_types;

    if (wanted & VS_CAPTURE_SOURCE_WINDOW) {
        /* Say it out loud rather than returning an empty list that reads as
         * "no windows are open". The two are not the same answer. */
        if (!(engine->source_types & VS_CAPTURE_SOURCE_WINDOW))
            VS_LOG("window sources requested but AvailableSourceTypes=%u has no "
                   "WINDOW bit; none will be listed", engine->source_types);
        else
            VS_LOG("window sources are chosen by the portal, not listed by the app");
    }
    if (!(wanted & VS_CAPTURE_SOURCE_MONITOR)) return VS_OK;
    if (!(engine->source_types & VS_CAPTURE_SOURCE_MONITOR)) {
        VS_LOG("AvailableSourceTypes=%u has no MONITOR bit", engine->source_types);
        return VS_OK;
    }
    return enumerate_monitors(sources_out, count_out);
}

void vs_capture_free_sources(vs_capture_source *sources, size_t count)
{
    (void)count;
    free(sources);
}

/* ----------------------------------------------------------- audio probe */

typedef struct {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct spa_hook registry_hook;
    struct spa_hook core_hook;
    int sync_seq;
    bool have_sink;
    bool have_source;
} audio_probe;

static void probe_global(void *data, uint32_t id, uint32_t permissions, const char *type,
                         uint32_t version, const struct spa_dict *props)
{
    (void)id; (void)permissions; (void)version;
    audio_probe *probe = data;
    if (!props || !type || strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;
    const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!media_class) return;
    /* An Audio/Sink always carries a monitor, which is what system-audio
     * capture records; an Audio/Source is a microphone or a line in. */
    if (strcmp(media_class, "Audio/Sink") == 0) probe->have_sink = true;
    else if (strcmp(media_class, "Audio/Source") == 0) probe->have_source = true;
    else if (strcmp(media_class, "Audio/Source/Virtual") == 0) probe->have_source = true;
}

static const struct pw_registry_events probe_registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = probe_global,
};

static void probe_done(void *data, uint32_t id, int seq)
{
    audio_probe *probe = data;
    if (id == PW_ID_CORE && seq == probe->sync_seq) pw_main_loop_quit(probe->loop);
}

static void probe_error(void *data, uint32_t id, int seq, int res, const char *message)
{
    (void)id; (void)seq; (void)res;
    audio_probe *probe = data;
    VS_LOG("PipeWire probe error: %s", message ? message : "?");
    pw_main_loop_quit(probe->loop);
}

static const struct pw_core_events probe_core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = probe_done,
    .error = probe_error,
};

/* One registry sweep of the user's own PipeWire daemon -- never the portal's
 * restricted fd, which exposes only the screencast node (WP-02, section 5.4). */
uint32_t vs_capture_probe_audio(void)
{
    audio_probe probe = { 0 };
    uint32_t capabilities = 0;
    probe.loop = pw_main_loop_new(NULL);
    if (!probe.loop) return 0;
    probe.context = pw_context_new(pw_main_loop_get_loop(probe.loop), NULL, 0);
    if (!probe.context) goto out;
    probe.core = pw_context_connect(probe.context, NULL, 0);
    if (!probe.core) {
        VS_LOG("no PipeWire daemon: no audio capture on this session");
        goto out;
    }
    pw_core_add_listener(probe.core, &probe.core_hook, &probe_core_events, &probe);
    probe.registry = pw_core_get_registry(probe.core, PW_VERSION_REGISTRY, 0);
    if (!probe.registry) goto out;
    pw_registry_add_listener(probe.registry, &probe.registry_hook,
                             &probe_registry_events, &probe);
    probe.sync_seq = pw_core_sync(probe.core, PW_ID_CORE, 0);
    pw_main_loop_run(probe.loop);

    if (probe.have_sink) capabilities |= VS_CAPTURE_HAS_AUDIO_MONITOR;
    if (probe.have_source) capabilities |= VS_CAPTURE_HAS_MICROPHONE;

out:
    if (probe.registry) pw_proxy_destroy((struct pw_proxy *)probe.registry);
    if (probe.core) pw_core_disconnect(probe.core);
    if (probe.context) pw_context_destroy(probe.context);
    pw_main_loop_destroy(probe.loop);
    return capabilities;
}
