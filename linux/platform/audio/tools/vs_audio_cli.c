/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * vs-audio: the CLI harness for the audio backend.
 *
 * PLAN.md § 5: "Backends are developed and tested locally with their CLI
 * harnesses; the Swift wrappers are exercised on CI." This is that harness.
 * Everything the Qt shell's mixer will do is reachable from here, so a
 * behaviour can be reproduced, and a bug reported, without a desktop session.
 */

#include "vorssaint_platform.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested;

static void on_sigint(int sig) { (void)sig; stop_requested = 1; }

static void usage(FILE *out)
{
    fprintf(out,
        "usage: vs-audio [--pulse] [--json] <command>\n"
        "\n"
        "  devices                  list sinks and sources\n"
        "  streams                  list application streams\n"
        "  watch                    print change events until interrupted\n"
        "  set-volume <id> <0..1.5> set a linear volume (1.0 = 100 %%)\n"
        "  set-mute <id> on|off     mute one node\n"
        "  route <stream> <sink>    pin a stream to a sink (sink 0 clears)\n"
        "  set-default <sink>       make a sink the default output\n"
        "  mute-inputs on|off       mute every input, or restore\n"
        "  caps                     print the capability flags\n"
        "\n"
        "  --pulse   use the libpulse fallback even where PipeWire answers\n"
        "  --json    machine-readable output for devices/streams/caps\n"
        "\n"
        "ids are PipeWire object.serial numbers (or tagged PulseAudio indices\n"
        "in the fallback); they are the first column of devices/streams.\n");
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
        else if ((unsigned char)*s < 0x20) printf("\\u%04x", *s);
        else putchar(*s);
    }
    putchar('"');
}

static void print_node_json(const vs_audio_node *n, bool last)
{
    printf("  {\"id\":%" PRIu32 ",\"pw_global_id\":%" PRIu32 ",\"kind\":\"%s\"",
           n->id, n->pw_global_id, kind_name(n->kind));
    printf(",\"name\":"); print_json_string(n->name);
    printf(",\"description\":"); print_json_string(n->description);
    printf(",\"app_name\":"); print_json_string(n->app_name);
    printf(",\"icon_name\":"); print_json_string(n->icon_name);
    printf(",\"media_name\":"); print_json_string(n->media_name);
    printf(",\"pid\":%" PRId32, n->pid);
    printf(",\"volume\":%.4f,\"cubic\":%.4f,\"percent\":%.1f",
           (double)n->volume, (double)vs_audio_linear_to_cubic(n->volume),
           (double)n->volume * 100.0);
    printf(",\"mute\":%s,\"has_volume\":%s,\"is_default\":%s",
           n->mute ? "true" : "false", n->has_volume ? "true" : "false",
           n->is_default ? "true" : "false");
    printf(",\"target_id\":%" PRIu32 ",\"linked_pw_global_id\":%" PRIu32,
           n->target_id, n->linked_pw_global_id);
    printf("}%s\n", last ? "" : ",");
}

static void print_devices(const vs_audio_node *nodes, size_t count, bool json)
{
    if (json) {
        printf("[\n");
        for (size_t i = 0; i < count; i++) print_node_json(&nodes[i], i + 1 == count);
        printf("]\n");
        return;
    }
    printf("%-8s %-8s %-6s %-5s %-7s %s\n",
           "ID", "KIND", "VOL%", "MUTE", "DEFAULT", "NAME");
    for (size_t i = 0; i < count; i++) {
        const vs_audio_node *n = &nodes[i];
        printf("%-8" PRIu32 " %-8s %-6.1f %-5s %-7s %s\n",
               n->id, kind_name(n->kind), (double)n->volume * 100.0,
               n->mute ? "yes" : "no", n->is_default ? "*" : "",
               n->description[0] ? n->description : n->name);
    }
}

static void print_streams(const vs_audio_node *nodes, size_t count, bool json)
{
    if (json) {
        printf("[\n");
        for (size_t i = 0; i < count; i++) print_node_json(&nodes[i], i + 1 == count);
        printf("]\n");
        return;
    }
    printf("%-8s %-8s %-6s %-5s %-7s %-7s %-22s %-8s %s\n",
           "ID", "KIND", "VOL%", "MUTE", "TARGET", "LINKED", "APP", "PID", "MEDIA");
    for (size_t i = 0; i < count; i++) {
        const vs_audio_node *n = &nodes[i];
        printf("%-8" PRIu32 " %-8s %-6.1f %-5s %-7" PRIu32 " %-7" PRIu32
               " %-22s %-8" PRId32 " %s\n",
               n->id, kind_name(n->kind), (double)n->volume * 100.0,
               n->mute ? "yes" : "no", n->target_id, n->linked_pw_global_id,
               n->app_name[0] ? n->app_name : n->name, n->pid, n->media_name);
    }
}

static const char *event_name(vs_audio_event_kind kind)
{
    switch (kind) {
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
    /* Line-buffered and flushed: `watch` is piped into test scripts, and a
     * block-buffered stdout would hand them the events only at exit. */
    printf("%ld.%03ld %s id=%" PRIu32 " name=%s\n", (long)ts.tv_sec,
           ts.tv_nsec / 1000000, event_name(event->kind), event->id, event->name);
    fflush(stdout);
}

static void print_caps(uint32_t caps, const char *backend, bool json)
{
    struct { const char *name; uint32_t bit; } bits[] = {
        { "has_pipewire", VS_AUDIO_CAP_HAS_PIPEWIRE },
        { "has_pulse_fallback", VS_AUDIO_CAP_HAS_PULSE_FALLBACK },
        { "can_route_per_stream", VS_AUDIO_CAP_CAN_ROUTE_PER_STREAM },
        { "can_boost_over_100", VS_AUDIO_CAP_CAN_BOOST_OVER_100 },
        { "has_events", VS_AUDIO_CAP_HAS_EVENTS },
    };
    if (json) {
        printf("{\"backend\":\"%s\"", backend);
        for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++)
            printf(",\"%s\":%s", bits[i].name, (caps & bits[i].bit) ? "true" : "false");
        printf(",\"max_volume\":%.2f}\n", (double)VS_AUDIO_MAX_VOLUME);
        return;
    }
    printf("backend: %s\n", backend);
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++)
        printf("%-22s %s\n", bits[i].name, (caps & bits[i].bit) ? "yes" : "no");
    printf("%-22s %.0f%%\n", "max_volume", (double)VS_AUDIO_MAX_VOLUME * 100.0);
}

static int fail(const char *what, int rc)
{
    fprintf(stderr, "vs-audio: %s: %s\n", what, strerror(-rc));
    return 1;
}

static bool parse_onoff(const char *s, bool *out)
{
    if (strcmp(s, "on") == 0 || strcmp(s, "true") == 0 || strcmp(s, "1") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(s, "off") == 0 || strcmp(s, "false") == 0 || strcmp(s, "0") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_id(const char *s, uint32_t *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || !end || *end != '\0' || v > UINT32_MAX) return false;
    *out = (uint32_t)v;
    return true;
}

int main(int argc, char **argv)
{
    bool json = false;
    bool use_pulse = false;
    int arg = 1;
    for (; arg < argc && argv[arg][0] == '-'; arg++) {
        if (strcmp(argv[arg], "--json") == 0) json = true;
        else if (strcmp(argv[arg], "--pulse") == 0) use_pulse = true;
        else if (strcmp(argv[arg], "-h") == 0 || strcmp(argv[arg], "--help") == 0) {
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

    vs_audio_options options = {
        .force_pulse = use_pulse,
        .on_event = strcmp(command, "watch") == 0 ? on_event : NULL,
    };
    vs_audio *audio = vs_audio_open(&options);
    if (!audio) {
        fprintf(stderr, "vs-audio: no audio server answered: %s\n", strerror(errno));
        return 1;
    }

    int status = 0;
    vs_audio_node *nodes = NULL;
    size_t count = 0;

    if (strcmp(command, "devices") == 0) {
        int rc = vs_audio_list_nodes(audio, VS_AUDIO_NODE_ANY_DEVICE, &nodes, &count);
        if (rc != 0) status = fail("devices", rc);
        else print_devices(nodes, count, json);
    } else if (strcmp(command, "streams") == 0) {
        int rc = vs_audio_list_nodes(audio, VS_AUDIO_NODE_ANY_STREAM, &nodes, &count);
        if (rc != 0) status = fail("streams", rc);
        else print_streams(nodes, count, json);
    } else if (strcmp(command, "caps") == 0) {
        print_caps(vs_audio_capabilities(audio), vs_audio_backend_name(audio), json);
    } else if (strcmp(command, "watch") == 0) {
        signal(SIGINT, on_sigint);
        signal(SIGTERM, on_sigint);
        fprintf(stderr, "vs-audio: watching on %s; interrupt to stop\n",
                vs_audio_backend_name(audio));
        while (!stop_requested) {
            struct timespec nap = { .tv_sec = 0, .tv_nsec = 100000000L };
            nanosleep(&nap, NULL);
        }
    } else if (strcmp(command, "set-volume") == 0) {
        uint32_t id;
        if (rest != 2 || !parse_id(argv[arg], &id)) { usage(stderr); status = 2; }
        else {
            float v = strtof(argv[arg + 1], NULL);
            int rc = vs_audio_set_volume(audio, id, v);
            if (rc != 0) status = fail("set-volume", rc);
            else printf("set %" PRIu32 " to %.1f%% (linear %.4f, cubic %.4f)\n",
                        id, (double)v * 100.0, (double)v,
                        (double)vs_audio_linear_to_cubic(v));
        }
    } else if (strcmp(command, "set-mute") == 0) {
        uint32_t id;
        bool on;
        if (rest != 2 || !parse_id(argv[arg], &id) || !parse_onoff(argv[arg + 1], &on)) {
            usage(stderr);
            status = 2;
        } else {
            int rc = vs_audio_set_mute(audio, id, on);
            if (rc != 0) status = fail("set-mute", rc);
            else printf("%" PRIu32 " mute=%s\n", id, on ? "on" : "off");
        }
    } else if (strcmp(command, "route") == 0) {
        uint32_t stream, sink;
        if (rest != 2 || !parse_id(argv[arg], &stream) || !parse_id(argv[arg + 1], &sink)) {
            usage(stderr);
            status = 2;
        } else {
            int rc = vs_audio_route_stream(audio, stream, sink);
            if (rc != 0) status = fail("route", rc);
            else if (sink == 0) printf("stream %" PRIu32 " follows the default\n", stream);
            else printf("stream %" PRIu32 " routed to sink %" PRIu32 "\n", stream, sink);
        }
    } else if (strcmp(command, "set-default") == 0) {
        uint32_t sink;
        if (rest != 1 || !parse_id(argv[arg], &sink)) { usage(stderr); status = 2; }
        else {
            int rc = vs_audio_set_default_sink(audio, sink);
            if (rc != 0) status = fail("set-default", rc);
            else printf("default sink is now %" PRIu32 "\n", sink);
        }
    } else if (strcmp(command, "mute-inputs") == 0) {
        bool on;
        if (rest != 1 || !parse_onoff(argv[arg], &on)) { usage(stderr); status = 2; }
        else {
            size_t affected = 0;
            int rc = vs_audio_mute_all_inputs(audio, on, &affected);
            if (rc != 0) status = fail("mute-inputs", rc);
            else printf("inputs %s (%zu changed)\n", on ? "muted" : "restored", affected);
        }
    } else {
        fprintf(stderr, "vs-audio: unknown command %s\n", command);
        usage(stderr);
        status = 2;
    }

    vs_audio_free_nodes(nodes, count);
    vs_audio_close(audio);
    return status;
}
