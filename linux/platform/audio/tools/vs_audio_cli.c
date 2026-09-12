/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * vs-audio: the CLI harness for the audio backend.
 *
 * PLAN.md § 5: backends are developed and tested locally with their CLI
 * harnesses, and the Swift wrappers are exercised on CI. This is that harness.
 * Everything the Qt shell's mixer will do is reachable from here, so a
 * behaviour can be reproduced and a bug reported without a desktop session.
 */

#include "vorssaint_platform.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig) { (void)sig; stop_requested = 1; }

static void usage(FILE *out)
{
    fprintf(out,
        "usage: vs-audio [--backend NAME] [--json] <command>\n"
        "\n"
        "  devices                  list sinks and sources\n"
        "  streams                  list application streams\n"
        "  watch                    print change events until interrupted\n"
        "  set-volume <id> <0..1.5> set a linear volume (1.0 = 100 %%)\n"
        "  set-mute <id> on|off     mute one node\n"
        "  route <stream> <sink>    send a stream to a sink (sink 0 clears)\n"
        "  set-default <sink>       make a sink the default output\n"
        "  set-default-source <src> make a source the default input\n"
        "  mute-inputs on|off       mute every input, or restore\n"
        "  caps                     print the backend and its capabilities\n"
        "\n"
        "  --backend NAME  force a backend: pipewire, libpulse\n"
        "  --json          machine-readable output\n"
        "\n"
        "ids are the first column of devices/streams: PipeWire object.serial\n"
        "numbers, or tagged PulseAudio indices in the fallback.\n");
}

static const char *kind_name(vs_audio_node_kind kind)
{
    switch (kind) {
    case VS_AUDIO_NODE_SINK: return "sink";
    case VS_AUDIO_NODE_SOURCE: return "source";
    case VS_AUDIO_NODE_STREAM_OUTPUT: return "playback";
    case VS_AUDIO_NODE_STREAM_INPUT: return "capture";
    }
    return "?";
}

static void print_json_string(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') printf("\\%c", *s);
        else if ((unsigned char)*s < 0x20) printf("\\u%04x", (unsigned char)*s);
        else putchar(*s);
    }
    putchar('"');
}

static void print_node_json(const vs_audio_node *n, bool last)
{
    printf("  {\"id\":%" PRIu32 ",\"kind\":\"%s\"", n->id, kind_name(n->kind));
    printf(",\"name\":"); print_json_string(n->name);
    printf(",\"description\":"); print_json_string(n->description);
    printf(",\"app_name\":"); print_json_string(n->app_name);
    printf(",\"icon_name\":"); print_json_string(n->icon_name);
    printf(",\"media_name\":"); print_json_string(n->media_name);
    printf(",\"app_id\":"); print_json_string(n->app_id);
    printf(",\"transport\":"); print_json_string(n->transport);
    printf(",\"pid\":%" PRId32, n->pid);
    printf(",\"volume\":%.4f,\"cubic\":%.4f,\"percent\":%.1f",
           (double)n->volume, (double)vs_audio_linear_to_cubic(n->volume),
           (double)n->volume * 100.0);
    printf(",\"mute\":%s", (n->flags & VS_AUDIO_NODE_MUTED) ? "true" : "false");
    printf(",\"has_volume\":%s", (n->flags & VS_AUDIO_NODE_HAS_VOLUME) ? "true" : "false");
    printf(",\"is_default\":%s", (n->flags & VS_AUDIO_NODE_IS_DEFAULT) ? "true" : "false");
    printf(",\"active\":%s", (n->flags & VS_AUDIO_NODE_ACTIVE) ? "true" : "false");
    printf(",\"target_id\":%" PRIu32 ",\"effective_id\":%" PRIu32,
           n->target_id, n->effective_id);
    printf("}%s\n", last ? "" : ",");
}

static void print_nodes_json(const vs_audio_node *nodes, size_t count)
{
    printf("[\n");
    for (size_t i = 0; i < count; i++) print_node_json(&nodes[i], i + 1 == count);
    printf("]\n");
}

static void print_devices(const vs_audio_node *nodes, size_t count)
{
    printf("%-8s %-8s %-6s %-5s %-7s %-9s %s\n",
           "ID", "KIND", "VOL%", "MUTE", "DEFAULT", "TRANSPORT", "NAME");
    for (size_t i = 0; i < count; i++) {
        const vs_audio_node *n = &nodes[i];
        printf("%-8" PRIu32 " %-8s %-6.1f %-5s %-7s %-9s %s\n",
               n->id, kind_name(n->kind), (double)n->volume * 100.0,
               (n->flags & VS_AUDIO_NODE_MUTED) ? "yes" : "no",
               (n->flags & VS_AUDIO_NODE_IS_DEFAULT) ? "*" : "",
               n->transport[0] ? n->transport : "-",
               n->description[0] ? n->description : n->name);
    }
}

static void print_streams(const vs_audio_node *nodes, size_t count)
{
    printf("%-8s %-8s %-6s %-5s %-6s %-8s %-9s %-20s %-8s %s\n",
           "ID", "KIND", "VOL%", "MUTE", "LIVE", "TARGET", "EFFECTIVE", "APP", "PID",
           "MEDIA");
    for (size_t i = 0; i < count; i++) {
        const vs_audio_node *n = &nodes[i];
        printf("%-8" PRIu32 " %-8s %-6.1f %-5s %-6s %-8" PRIu32 " %-9" PRIu32
               " %-20s %-8" PRId32 " %s\n",
               n->id, kind_name(n->kind), (double)n->volume * 100.0,
               (n->flags & VS_AUDIO_NODE_MUTED) ? "yes" : "no",
               (n->flags & VS_AUDIO_NODE_ACTIVE) ? "yes" : "no",
               n->target_id, n->effective_id,
               n->app_name[0] ? n->app_name : n->name, n->pid, n->media_name);
    }
}

static const char *event_name(vs_audio_event_type type)
{
    switch (type) {
    case VS_AUDIO_EVENT_CHANGED: return "changed";
    case VS_AUDIO_EVENT_DEFAULT_SINK_CHANGED: return "default-sink-changed";
    case VS_AUDIO_EVENT_DEFAULT_SOURCE_CHANGED: return "default-source-changed";
    case VS_AUDIO_EVENT_DEFAULT_SINK_DISCONNECTED: return "default-sink-disconnected";
    }
    return "?";
}

static void on_event(const vs_audio_event *event, void *user_data)
{
    (void)user_data;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    /* Flushed every line: `watch` is piped into the test scripts, and a
     * block-buffered stdout would hand them the events only at exit. */
    printf("%ld.%03ld %s id=%" PRIu32 " name=%s\n", (long)ts.tv_sec,
           ts.tv_nsec / 1000000, event_name(event->type), event->id, event->name);
    fflush(stdout);
}

static void print_caps(const vs_audio_system *audio, bool json)
{
    static const struct { const char *name; uint32_t bit; } bits[] = {
        { "has_pipewire", VS_AUDIO_HAS_PIPEWIRE },
        { "has_pulse_fallback", VS_AUDIO_HAS_PULSE_FALLBACK },
        { "can_route_per_stream", VS_AUDIO_CAN_ROUTE_PER_STREAM },
        { "can_boost_over_100", VS_AUDIO_CAN_BOOST_OVER_100 },
        { "has_events", VS_AUDIO_HAS_EVENTS },
    };
    size_t n = sizeof(bits) / sizeof(bits[0]);
    if (json) {
        printf("{\"backend\":\"%s\"", audio->name);
        for (size_t i = 0; i < n; i++)
            printf(",\"%s\":%s", bits[i].name,
                   (audio->capabilities & bits[i].bit) ? "true" : "false");
        printf(",\"max_volume\":%.2f}\n", (double)VS_AUDIO_MAX_VOLUME);
        return;
    }
    printf("backend: %s\n", audio->name);
    for (size_t i = 0; i < n; i++)
        printf("%-22s %s\n", bits[i].name,
               (audio->capabilities & bits[i].bit) ? "yes" : "no");
    printf("%-22s %.0f%%\n", "max_volume", (double)VS_AUDIO_MAX_VOLUME * 100.0);
}

static int fail(const char *what, int rc)
{
    fprintf(stderr, "vs-audio: %s: %s\n", what, vs_result_string(rc));
    return 1;
}

static bool parse_onoff(const char *s, bool *out)
{
    if (!strcmp(s, "on") || !strcmp(s, "true") || !strcmp(s, "1")) { *out = true; return true; }
    if (!strcmp(s, "off") || !strcmp(s, "false") || !strcmp(s, "0")) { *out = false; return true; }
    return false;
}

static bool parse_id(const char *s, vs_audio_id *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || !end || *end != '\0' || v > UINT32_MAX) return false;
    *out = (vs_audio_id)v;
    return true;
}

/** `watch`: poll the backend's fd where it has one, tick where it does not. */
static void run_watch(vs_audio_system *audio)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    audio->set_event_callback(audio, on_event, NULL);
    int fd = audio->event_fd(audio);
    fprintf(stderr, "vs-audio: watching on %s (event_fd=%d); interrupt to stop\n",
            audio->name, fd);
    fflush(stderr);

    while (!stop_requested) {
        if (fd >= 0) {
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            /* A timeout even with a pollable fd: the debounce timer lives in
             * the backend's own loop, and on the libpulse side there is no fd
             * at all, so the same loop serves both. */
            if (poll(&pfd, 1, 100) < 0 && errno != EINTR) break;
        } else {
            struct timespec nap = { .tv_sec = 0, .tv_nsec = 50L * 1000000L };
            nanosleep(&nap, NULL);
        }
        int rc = audio->dispatch(audio);
        if (rc < 0) {
            fprintf(stderr, "vs-audio: dispatch: %s\n", vs_result_string(rc));
            break;
        }
    }
}

int main(int argc, char **argv)
{
    bool json = false;
    const char *backend = NULL;
    int arg = 1;
    for (; arg < argc && argv[arg][0] == '-'; arg++) {
        if (!strcmp(argv[arg], "--json")) {
            json = true;
        } else if (!strcmp(argv[arg], "--backend") && arg + 1 < argc) {
            backend = argv[++arg];
        } else if (!strcmp(argv[arg], "-h") || !strcmp(argv[arg], "--help")) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "vs-audio: unknown option %s\n", argv[arg]);
            return 2;
        }
    }
    if (arg >= argc) { usage(stderr); return 2; }
    const char *command = argv[arg++];
    int rest = argc - arg;

    int result = VS_ERR_NO_BACKEND;
    vs_audio_system *audio = vs_audio_system_create(backend, &result);
    if (!audio) {
        fprintf(stderr, "vs-audio: no audio server answered: %s\n",
                vs_result_string(result));
        return 1;
    }

    int status = 0;
    if (!strcmp(command, "devices") || !strcmp(command, "streams")) {
        uint32_t mask = !strcmp(command, "devices") ? VS_AUDIO_NODE_ANY_DEVICE
                                                    : VS_AUDIO_NODE_ANY_STREAM;
        vs_audio_node *nodes = NULL;
        size_t count = 0;
        int rc = audio->list(audio, mask, &nodes, &count);
        if (rc != VS_OK) {
            status = fail(command, rc);
        } else if (json) {
            print_nodes_json(nodes, count);
        } else if (mask == VS_AUDIO_NODE_ANY_DEVICE) {
            print_devices(nodes, count);
        } else {
            print_streams(nodes, count);
        }
        audio->free_list(audio, nodes, count);
    } else if (!strcmp(command, "caps")) {
        print_caps(audio, json);
    } else if (!strcmp(command, "watch")) {
        run_watch(audio);
    } else if (!strcmp(command, "set-volume")) {
        vs_audio_id id;
        if (rest != 2 || !parse_id(argv[arg], &id)) { usage(stderr); status = 2; }
        else {
            float v = strtof(argv[arg + 1], NULL);
            int rc = audio->set_volume(audio, id, v);
            if (rc != VS_OK) status = fail("set-volume", rc);
            else printf("set %" PRIu32 " to %.1f%% (linear %.4f, cubic %.4f)\n",
                        id, (double)v * 100.0, (double)v,
                        (double)vs_audio_linear_to_cubic(v));
        }
    } else if (!strcmp(command, "set-mute")) {
        vs_audio_id id;
        bool on;
        if (rest != 2 || !parse_id(argv[arg], &id) || !parse_onoff(argv[arg + 1], &on)) {
            usage(stderr);
            status = 2;
        } else {
            int rc = audio->set_mute(audio, id, on);
            if (rc != VS_OK) status = fail("set-mute", rc);
            else printf("%" PRIu32 " mute=%s\n", id, on ? "on" : "off");
        }
    } else if (!strcmp(command, "route")) {
        vs_audio_id stream, sink;
        if (rest != 2 || !parse_id(argv[arg], &stream) || !parse_id(argv[arg + 1], &sink)) {
            usage(stderr);
            status = 2;
        } else {
            int rc = audio->route_stream(audio, stream, sink);
            if (rc != VS_OK) status = fail("route", rc);
            else if (sink == 0) printf("stream %" PRIu32 " follows the default\n", stream);
            else printf("stream %" PRIu32 " routed to sink %" PRIu32 "\n", stream, sink);
        }
    } else if (!strcmp(command, "set-default")) {
        vs_audio_id sink;
        if (rest != 1 || !parse_id(argv[arg], &sink)) { usage(stderr); status = 2; }
        else {
            int rc = audio->set_default_sink(audio, sink);
            if (rc != VS_OK) status = fail("set-default", rc);
            else printf("default sink is now %" PRIu32 "\n", sink);
        }
    } else if (!strcmp(command, "set-default-source")) {
        vs_audio_id source;
        if (rest != 1 || !parse_id(argv[arg], &source)) { usage(stderr); status = 2; }
        else {
            int rc = audio->set_default_source(audio, source);
            if (rc != VS_OK) status = fail("set-default-source", rc);
            else printf("default source is now %" PRIu32 "\n", source);
        }
    } else if (!strcmp(command, "mute-inputs")) {
        bool on;
        if (rest != 1 || !parse_onoff(argv[arg], &on)) { usage(stderr); status = 2; }
        else {
            size_t changed = 0;
            int rc = audio->mute_all_inputs(audio, on, &changed);
            if (rc != VS_OK) status = fail("mute-inputs", rc);
            else printf("inputs %s (%zu changed)\n", on ? "muted" : "restored", changed);
        }
    } else {
        fprintf(stderr, "vs-audio: unknown command %s\n", command);
        usage(stderr);
        status = 2;
    }

    audio->destroy(audio);
    return status;
}
