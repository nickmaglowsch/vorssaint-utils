/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * The audio backend against a real audio server.
 *
 * run-live-test.sh brings up a private stack (its own dbus-daemon, pipewire,
 * wireplumber and pipewire-pulse, with `support.null-audio-sink` nodes standing
 * in for hardware) and runs one case from this binary inside it. Exit 77 means
 * the stack could not be built here, which ctest reports as "not run"; a
 * machine without PipeWire cannot look green by accident.
 *
 * Nothing here asserts on its own bookkeeping. Every claim is checked against
 * what the server says: a volume is read back from the node, a route is checked
 * against where the links landed, a default against the metadata.
 */

#include "vorssaint_platform.h"

#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The nodes run-stack.sh creates. */
#define SINK_A "vs-sink-a"
#define SINK_B "vs-sink-b"
#define SOURCE_A "vs-source-a"
/* The application.name test-stream.sh gives its pw-play. */
#define TEST_APP "VsTestPlayer"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define CHECK_OK(call) do { \
    int rc_ = (call); \
    if (rc_ != VS_OK) { \
        fprintf(stderr, "FAIL %s:%d: %s -> %s\n", __FILE__, __LINE__, #call, \
                vs_result_string(rc_)); \
        failures++; \
    } \
} while (0)

#define REQUIRE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FATAL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
        return; \
    } \
} while (0)

/* --- helpers --------------------------------------------------------------- */

static bool find_named(vs_audio_system *audio, uint32_t mask, const char *name,
                       vs_audio_node *out)
{
    vs_audio_node *nodes = NULL;
    size_t count = 0;
    if (audio->list(audio, mask, &nodes, &count) != VS_OK) return false;
    bool found = false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(nodes[i].name, name) != 0) continue;
        *out = nodes[i];
        found = true;
        break;
    }
    audio->free_list(audio, nodes, count);
    return found;
}

/** Finds the test stream by its application.name, whatever its node.name is. */
static bool find_test_stream(vs_audio_system *audio, vs_audio_node *out)
{
    vs_audio_node *nodes = NULL;
    size_t count = 0;
    if (audio->list(audio, VS_AUDIO_NODE_ANY_STREAM, &nodes, &count) != VS_OK)
        return false;
    bool found = false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(nodes[i].app_name, TEST_APP) != 0) continue;
        *out = nodes[i];
        found = true;
        break;
    }
    audio->free_list(audio, nodes, count);
    return found;
}

/**
 * Waits for the test stream to appear *and be connected*.
 *
 * Two separate waits in one, because they finish at different times: the
 * stream is another process starting up, and its node global arrives a moment
 * before WirePlumber has built the links that carry its audio to a sink. A
 * test that stopped at the node would read `effective_id` as 0 and blame the
 * backend for the race -- which is what it did before this waited for the
 * links too.
 */
static bool await_test_stream(vs_audio_system *audio, vs_audio_node *out,
                              unsigned timeout_ms)
{
    for (unsigned waited = 0; waited <= timeout_ms; waited += 200) {
        audio->dispatch(audio);
        if (find_test_stream(audio, out) && out->effective_id != 0) return true;
        struct timespec nap = { .tv_sec = 0, .tv_nsec = 200L * 1000000L };
        nanosleep(&nap, NULL);
    }
    return false;
}

/** The sink the metadata currently calls the default, by id. */
static vs_audio_id default_sink_id(vs_audio_system *audio)
{
    vs_audio_node *sinks = NULL;
    size_t count = 0;
    if (audio->list(audio, VS_AUDIO_NODE_SINK, &sinks, &count) != VS_OK) return 0;
    vs_audio_id id = 0;
    for (size_t i = 0; i < count; i++)
        if (sinks[i].flags & VS_AUDIO_NODE_IS_DEFAULT) { id = sinks[i].id; break; }
    audio->free_list(audio, sinks, count);
    return id;
}

static vs_audio_system *open_backend(const char *preferred)
{
    int result = VS_ERR_NO_BACKEND;
    vs_audio_system *audio = vs_audio_system_create(preferred, &result);
    if (!audio)
        fprintf(stderr, "cannot open %s backend: %s\n",
                preferred ? preferred : "default", vs_result_string(result));
    return audio;
}

/* --- 1. registry ----------------------------------------------------------- */

static void case_registry(void)
{
    vs_audio_system *audio = open_backend("pipewire");
    REQUIRE(audio);

    /* Both sinks and the source the stack created are enumerated, with the
     * `node.description` the props carried, not just the node name. */
    vs_audio_node sink_a, sink_b, source_a;
    CHECK(find_named(audio, VS_AUDIO_NODE_SINK, SINK_A, &sink_a));
    CHECK(find_named(audio, VS_AUDIO_NODE_SINK, SINK_B, &sink_b));
    CHECK(find_named(audio, VS_AUDIO_NODE_SOURCE, SOURCE_A, &source_a));
    CHECK(strcmp(sink_a.description, "Vorssaint Test Sink A") == 0);
    CHECK(sink_a.id != 0 && sink_b.id != 0 && sink_a.id != sink_b.id);
    CHECK(sink_a.flags & VS_AUDIO_NODE_HAS_VOLUME);

    /* The default sink comes from the `default.audio.sink` metadata; the stack
     * sets it to sink A. Exactly one sink is the default. */
    vs_audio_node *sinks = NULL;
    size_t count = 0;
    CHECK_OK(audio->list(audio, VS_AUDIO_NODE_SINK, &sinks, &count));
    int defaults = 0;
    for (size_t i = 0; i < count; i++)
        if (sinks[i].flags & VS_AUDIO_NODE_IS_DEFAULT) defaults++;
    CHECK(defaults == 1);
    audio->free_list(audio, sinks, count);

    /* A stream carries the application properties a mixer row needs. pid and
     * media.name are the interesting ones: they are absent from the registry's
     * filtered global props and only arrive on the node's own info event, so
     * this is the check that the node is bound and not merely noticed. */
    vs_audio_node stream;
    REQUIRE(await_test_stream(audio, &stream, 10000));
    CHECK(stream.kind == VS_AUDIO_NODE_STREAM_OUTPUT);
    CHECK(strcmp(stream.app_name, TEST_APP) == 0);
    CHECK(strcmp(stream.media_name, "VsTestTone") == 0);
    CHECK(strcmp(stream.icon_name, "audio-x-generic") == 0);
    CHECK(stream.pid > 0);
    CHECK(stream.flags & VS_AUDIO_NODE_HAS_PID);
    /* Unrouted, it follows the default and its audio lands on that sink.
     * Compared against whichever sink the metadata calls the default rather
     * than against sink A by name: which one WirePlumber settles on is its
     * choice, and the claim being checked is "an unpinned stream lands on the
     * default", not "it lands on sink A". */
    CHECK(stream.target_id == 0);
    CHECK(stream.effective_id == default_sink_id(audio));

    /* The kind mask selects: asking for devices returns no streams. */
    vs_audio_node *devices = NULL;
    CHECK_OK(audio->list(audio, VS_AUDIO_NODE_ANY_DEVICE, &devices, &count));
    for (size_t i = 0; i < count; i++)
        CHECK((devices[i].kind & VS_AUDIO_NODE_ANY_STREAM) == 0);
    audio->free_list(audio, devices, count);

    /* An empty mask is a caller error, not an empty answer. */
    vs_audio_node *none = (void *)1;
    size_t none_count = 99;
    CHECK(audio->list(audio, 0, &none, &none_count) == VS_ERR_INVALID);
    CHECK(none == NULL && none_count == 0);

    audio->destroy(audio);
}

/* --- 1. events ------------------------------------------------------------- */

struct event_log {
    int changed;
    int sink_disconnected;
    int default_changed;
    int total;
    vs_audio_id last_disconnected;
    char last_disconnected_name[VS_AUDIO_NAME_MAX];
    /* Set if the callback ever runs outside a dispatch call. */
    bool outside_dispatch;
    bool in_dispatch;
};

static void log_event(const vs_audio_event *event, void *user_data)
{
    struct event_log *log = user_data;
    if (!log->in_dispatch) log->outside_dispatch = true;
    log->total++;
    switch (event->type) {
    case VS_AUDIO_EVENT_CHANGED: log->changed++; break;
    case VS_AUDIO_EVENT_DEFAULT_SINK_CHANGED:
    case VS_AUDIO_EVENT_DEFAULT_SOURCE_CHANGED: log->default_changed++; break;
    case VS_AUDIO_EVENT_DEFAULT_SINK_DISCONNECTED:
        log->sink_disconnected++;
        log->last_disconnected = event->id;
        snprintf(log->last_disconnected_name, sizeof(log->last_disconnected_name),
                 "%s", event->name);
        break;
    }
}

/** dispatch, with the flag that proves the callback ran inside it. */
static int guarded_dispatch(vs_audio_system *audio, struct event_log *log)
{
    log->in_dispatch = true;
    int rc = audio->dispatch(audio);
    log->in_dispatch = false;
    return rc;
}

/** Pumps for up to `timeout_ms`, stopping early once `want` events arrived. */
static void pump(vs_audio_system *audio, struct event_log *log, unsigned timeout_ms,
                 int want)
{
    int fd = audio->event_fd(audio);
    for (unsigned waited = 0; waited < timeout_ms; waited += 50) {
        if (fd >= 0) {
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            poll(&pfd, 1, 50);
        } else {
            struct timespec nap = { .tv_sec = 0, .tv_nsec = 50L * 1000000L };
            nanosleep(&nap, NULL);
        }
        guarded_dispatch(audio, log);
        if (want > 0 && log->total >= want) return;
    }
}

static void case_events(void)
{
    vs_audio_system *audio = open_backend("pipewire");
    REQUIRE(audio);
    CHECK(audio->capabilities & VS_AUDIO_HAS_EVENTS);
    /* The PipeWire backend has a real pollable descriptor, so a Qt event loop
     * can wait on it rather than tick. */
    CHECK(audio->event_fd(audio) >= 0);

    struct event_log log = { 0 };
    CHECK_OK(audio->set_event_callback(audio, log_event, &log));

    vs_audio_node stream;
    REQUIRE(await_test_stream(audio, &stream, 10000));

    /* A param change: setting a volume changes the graph, so a CHANGED follows.
     * It is debounced, so one write does not produce a burst. */
    log.total = log.changed = 0;
    CHECK_OK(audio->set_volume(audio, stream.id, 0.4f));
    pump(audio, &log, 2000, 1);
    CHECK(log.changed >= 1);
    CHECK(log.changed <= 3);

    /* The callback never ran outside a dispatch, including during the blocking
     * set_volume above, which pumps the connection internally. That is the
     * header's threading rule and the reason for the event queue. */
    CHECK(!log.outside_dispatch);

    /* A node arriving is a change. Its removal, when it is the default sink, is
     * the headphone-disconnect event -- checked in case_default_sink, which can
     * afford to destroy a sink. */
    audio->destroy(audio);
}

/* --- 2. volume and mute ---------------------------------------------------- */

static void case_volume(void)
{
    vs_audio_system *audio = open_backend("pipewire");
    REQUIRE(audio);
    CHECK(audio->capabilities & VS_AUDIO_CAN_BOOST_OVER_100);

    vs_audio_node stream;
    REQUIRE(await_test_stream(audio, &stream, 10000));

    /* Half volume, read back from the node rather than from what we sent. */
    CHECK_OK(audio->set_volume(audio, stream.id, 0.5f));
    REQUIRE(find_test_stream(audio, &stream));
    CHECK(stream.volume > 0.499f && stream.volume < 0.501f);
    /* The same loudness is 0.7937 on a wpctl slider; the conversion is the one
     * place that mapping lives. */
    CHECK(vs_audio_linear_to_cubic(stream.volume) > 0.79f);
    CHECK(vs_audio_linear_to_cubic(stream.volume) < 0.80f);

    /* Above unity: the point of the feature. PipeWire does not clamp, so 150 %
     * comes back as 150 % and not as 100 %. */
    CHECK_OK(audio->set_volume(audio, stream.id, VS_AUDIO_MAX_VOLUME));
    REQUIRE(find_test_stream(audio, &stream));
    CHECK(stream.volume > 1.499f && stream.volume < 1.501f);

    /* Over the ceiling is clamped to it, not refused: a slider dragged to the
     * end has to work. */
    CHECK_OK(audio->set_volume(audio, stream.id, 4.0f));
    REQUIRE(find_test_stream(audio, &stream));
    CHECK(stream.volume > 1.499f && stream.volume < 1.501f);

    /* Back to unity. */
    CHECK_OK(audio->set_volume(audio, stream.id, 1.0f));
    REQUIRE(find_test_stream(audio, &stream));
    CHECK(stream.volume > 0.999f && stream.volume < 1.001f);

    /* Mute is separate from volume: muting must not lose the level, because
     * unmuting has to put the slider back where it was. */
    CHECK_OK(audio->set_volume(audio, stream.id, 0.7f));
    CHECK_OK(audio->set_mute(audio, stream.id, true));
    REQUIRE(find_test_stream(audio, &stream));
    CHECK(stream.flags & VS_AUDIO_NODE_MUTED);
    CHECK(stream.volume > 0.699f && stream.volume < 0.701f);
    CHECK_OK(audio->set_mute(audio, stream.id, false));
    REQUIRE(find_test_stream(audio, &stream));
    CHECK(!(stream.flags & VS_AUDIO_NODE_MUTED));
    CHECK(stream.volume > 0.699f && stream.volume < 0.701f);

    /* Muting what is already muted is a no-op, not an error. */
    CHECK_OK(audio->set_mute(audio, stream.id, false));

    /* Device volumes work the same way as stream volumes. */
    vs_audio_node sink;
    REQUIRE(find_named(audio, VS_AUDIO_NODE_SINK, SINK_B, &sink));
    CHECK_OK(audio->set_volume(audio, sink.id, 0.25f));
    REQUIRE(find_named(audio, VS_AUDIO_NODE_SINK, SINK_B, &sink));
    CHECK(sink.volume > 0.249f && sink.volume < 0.251f);
    CHECK_OK(audio->set_volume(audio, sink.id, 1.0f));

    /* An id nothing answers to is NOT_FOUND, and 0 is never valid. */
    CHECK(audio->set_volume(audio, 999999, 0.5f) == VS_ERR_NOT_FOUND);
    CHECK(audio->set_volume(audio, 0, 0.5f) == VS_ERR_INVALID);
    CHECK(audio->set_mute(audio, 999999, true) == VS_ERR_NOT_FOUND);

    audio->destroy(audio);
}

/* --- 3. routing ------------------------------------------------------------ */

static void case_routing(void)
{
    vs_audio_system *audio = open_backend("pipewire");
    REQUIRE(audio);
    CHECK(audio->capabilities & VS_AUDIO_CAN_ROUTE_PER_STREAM);

    vs_audio_node sink_a, sink_b, stream;
    REQUIRE(find_named(audio, VS_AUDIO_NODE_SINK, SINK_A, &sink_a));
    REQUIRE(find_named(audio, VS_AUDIO_NODE_SINK, SINK_B, &sink_b));
    REQUIRE(await_test_stream(audio, &stream, 10000));

    /* It starts on the default, whichever that is. */
    CHECK(stream.effective_id == default_sink_id(audio));

    /* Pin it to the other sink. route_stream returns only once the links have
     * actually moved, so the listing right after it must already agree -- that
     * is the read-back, and `effective_id` is read from the Link objects, not
     * from the request. */
    CHECK_OK(audio->route_stream(audio, stream.id, sink_b.id));
    REQUIRE(find_test_stream(audio, &stream));
    CHECK(stream.target_id == sink_b.id);
    CHECK(stream.effective_id == sink_b.id);

    /* And back. */
    CHECK_OK(audio->route_stream(audio, stream.id, sink_a.id));
    REQUIRE(find_test_stream(audio, &stream));
    CHECK(stream.target_id == sink_a.id);
    CHECK(stream.effective_id == sink_a.id);

    /* Clearing the pin: the stream follows the default again. target_id goes
     * back to 0 while the audio keeps arriving somewhere. */
    CHECK_OK(audio->route_stream(audio, stream.id, 0));
    REQUIRE(find_test_stream(audio, &stream));
    CHECK(stream.target_id == 0);
    CHECK(stream.effective_id != 0);

    /* Routing to something that is not a sink, or that does not exist, is
     * refused rather than silently ignored. */
    vs_audio_node source;
    REQUIRE(find_named(audio, VS_AUDIO_NODE_SOURCE, SOURCE_A, &source));
    CHECK(audio->route_stream(audio, stream.id, source.id) == VS_ERR_NOT_FOUND);
    CHECK(audio->route_stream(audio, stream.id, 999999) == VS_ERR_NOT_FOUND);
    /* A sink is not a stream. */
    CHECK(audio->route_stream(audio, sink_a.id, sink_b.id) == VS_ERR_NOT_FOUND);

    audio->destroy(audio);
}

/* --- 4. default sink and headphone disconnect ------------------------------ */

/** Runs a command inside the stack. Returns its exit status. */
static int stack_exec(const char *command)
{
    const char *scripts = getenv("VS_AUDIO_SCRIPTS");
    if (!scripts) return -1;
    char line[1024];
    snprintf(line, sizeof(line), "%s/run-stack.sh exec %s", scripts, command);
    return system(line);
}

static void case_default_sink(void)
{
    vs_audio_system *audio = open_backend("pipewire");
    REQUIRE(audio);

    struct event_log log = { 0 };
    CHECK_OK(audio->set_event_callback(audio, log_event, &log));

    vs_audio_node sink_a, sink_b;
    REQUIRE(find_named(audio, VS_AUDIO_NODE_SINK, SINK_A, &sink_a));
    REQUIRE(find_named(audio, VS_AUDIO_NODE_SINK, SINK_B, &sink_b));
    CHECK(sink_a.flags & VS_AUDIO_NODE_IS_DEFAULT);

    /* Switch the default and read it back from the metadata. */
    CHECK_OK(audio->set_default_sink(audio, sink_b.id));
    REQUIRE(find_named(audio, VS_AUDIO_NODE_SINK, SINK_B, &sink_b));
    REQUIRE(find_named(audio, VS_AUDIO_NODE_SINK, SINK_A, &sink_a));
    CHECK(sink_b.flags & VS_AUDIO_NODE_IS_DEFAULT);
    CHECK(!(sink_a.flags & VS_AUDIO_NODE_IS_DEFAULT));

    /* The change is announced, and only from dispatch. */
    pump(audio, &log, 2000, 0);
    CHECK(log.default_changed >= 1);
    CHECK(!log.outside_dispatch);

    /* And back, so the disconnect below removes a sink that is the default. */
    CHECK_OK(audio->set_default_sink(audio, sink_a.id));
    REQUIRE(find_named(audio, VS_AUDIO_NODE_SINK, SINK_A, &sink_a));
    CHECK(sink_a.flags & VS_AUDIO_NODE_IS_DEFAULT);

    /* A sink that does not exist, and a source asked to be a sink. */
    CHECK(audio->set_default_sink(audio, 999999) == VS_ERR_NOT_FOUND);
    vs_audio_node source;
    REQUIRE(find_named(audio, VS_AUDIO_NODE_SOURCE, SOURCE_A, &source));
    CHECK(audio->set_default_sink(audio, source.id) == VS_ERR_NOT_FOUND);

    /* Headphone disconnect: destroy the sink that is currently the default.
     * A null sink going away is exactly the shape of a USB DAC being pulled or
     * a Bluetooth headset dropping -- the node vanishes while it is the
     * default -- and it is the event the macOS "lower the volume when
     * headphones disconnect" behaviour hangs on. */
    vs_audio_id expected = sink_a.id;
    char command[256];
    snprintf(command, sizeof(command), "pw-cli destroy %s > /dev/null 2>&1", SINK_A);
    REQUIRE(stack_exec(command) == 0);

    log.sink_disconnected = 0;
    pump(audio, &log, 5000, 0);
    CHECK(log.sink_disconnected == 1);
    CHECK(log.last_disconnected == expected);
    CHECK(strcmp(log.last_disconnected_name, SINK_A) == 0);
    CHECK(!log.outside_dispatch);

    /* And it really is gone from the listing, not merely announced. */
    vs_audio_node gone;
    CHECK(!find_named(audio, VS_AUDIO_NODE_SINK, SINK_A, &gone));

    audio->destroy(audio);
}

/* --- 5. mute every input --------------------------------------------------- */

static void case_mute_inputs(void)
{
    vs_audio_system *audio = open_backend("pipewire");
    REQUIRE(audio);

    vs_audio_node source;
    REQUIRE(find_named(audio, VS_AUDIO_NODE_SOURCE, SOURCE_A, &source));
    CHECK_OK(audio->set_mute(audio, source.id, false));

    size_t changed = 0;
    CHECK_OK(audio->mute_all_inputs(audio, true, &changed));
    CHECK(changed == 1);
    REQUIRE(find_named(audio, VS_AUDIO_NODE_SOURCE, SOURCE_A, &source));
    CHECK(source.flags & VS_AUDIO_NODE_MUTED);

    CHECK_OK(audio->mute_all_inputs(audio, false, &changed));
    CHECK(changed == 1);
    REQUIRE(find_named(audio, VS_AUDIO_NODE_SOURCE, SOURCE_A, &source));
    CHECK(!(source.flags & VS_AUDIO_NODE_MUTED));

    /* Restoring when nothing was muted by this feature does nothing at all --
     * deliberately not "unmute everything", which would open a microphone the
     * user closed themselves. */
    CHECK_OK(audio->set_mute(audio, source.id, true));
    CHECK_OK(audio->mute_all_inputs(audio, false, &changed));
    CHECK(changed == 0);
    REQUIRE(find_named(audio, VS_AUDIO_NODE_SOURCE, SOURCE_A, &source));
    CHECK(source.flags & VS_AUDIO_NODE_MUTED);

    /* The promise that matters: a source the user muted before the mic was
     * turned off stays muted when it is turned back on. Here the source is
     * already muted, so muting everything changes nothing, and restoring must
     * leave it alone rather than unmute it. */
    CHECK_OK(audio->mute_all_inputs(audio, true, &changed));
    CHECK(changed == 0);
    CHECK_OK(audio->mute_all_inputs(audio, false, &changed));
    CHECK(changed == 0);
    REQUIRE(find_named(audio, VS_AUDIO_NODE_SOURCE, SOURCE_A, &source));
    CHECK(source.flags & VS_AUDIO_NODE_MUTED);

    /* The record outlives the handle: a second instance -- standing in for the
     * app restarting, or for the CLI, which is a new process every time -- can
     * still undo what the first one did. */
    CHECK_OK(audio->set_mute(audio, source.id, false));
    CHECK_OK(audio->mute_all_inputs(audio, true, &changed));
    CHECK(changed == 1);
    audio->destroy(audio);

    vs_audio_system *second = open_backend("pipewire");
    REQUIRE(second);
    REQUIRE(find_named(second, VS_AUDIO_NODE_SOURCE, SOURCE_A, &source));
    CHECK(source.flags & VS_AUDIO_NODE_MUTED);
    CHECK_OK(second->mute_all_inputs(second, false, &changed));
    CHECK(changed == 1);
    REQUIRE(find_named(second, VS_AUDIO_NODE_SOURCE, SOURCE_A, &source));
    CHECK(!(source.flags & VS_AUDIO_NODE_MUTED));
    second->destroy(second);
}

/* --- 6. the libpulse fallback ---------------------------------------------- */

static void case_pulse_fallback(void)
{
    /* Against pipewire-pulse here, which is the same protocol a real
     * PulseAudio server speaks. What is being tested is this backend's use of
     * the libpulse API, not which daemon is behind the socket. */
    vs_audio_system *audio = open_backend("libpulse");
    REQUIRE(audio);
    CHECK(strcmp(audio->name, "libpulse") == 0);
    CHECK(audio->capabilities & VS_AUDIO_HAS_PULSE_FALLBACK);
    CHECK(!(audio->capabilities & VS_AUDIO_HAS_PIPEWIRE));
    /* No single pollable descriptor; the caller ticks `dispatch` instead. */
    CHECK(audio->event_fd(audio) == -1);
    CHECK(audio->dispatch(audio) >= 0);

    /* devices */
    vs_audio_node sink_a, sink_b, source_a;
    REQUIRE(find_named(audio, VS_AUDIO_NODE_SINK, SINK_A, &sink_a));
    REQUIRE(find_named(audio, VS_AUDIO_NODE_SINK, SINK_B, &sink_b));
    REQUIRE(find_named(audio, VS_AUDIO_NODE_SOURCE, SOURCE_A, &source_a));
    /* Monitor sources are left out: muting every input must not silence
     * system-audio capture. */
    vs_audio_node monitor;
    CHECK(!find_named(audio, VS_AUDIO_NODE_SOURCE, SINK_A ".monitor", &monitor));

    /* streams, with the application properties a mixer row needs */
    vs_audio_node stream;
    REQUIRE(await_test_stream(audio, &stream, 10000));
    CHECK(stream.pid > 0);
    CHECK(strcmp(stream.media_name, "VsTestTone") == 0);

    /* set-volume, including the boost, read back through the same API. The
     * linear scale is the backend's promise: pa_volume_t is cubic underneath,
     * and pa_sw_volume_from_linear is what keeps the two from being confused. */
    CHECK_OK(audio->set_volume(audio, stream.id, 1.5f));
    REQUIRE(find_test_stream(audio, &stream));
    CHECK(stream.volume > 1.49f && stream.volume < 1.51f);
    CHECK_OK(audio->set_volume(audio, stream.id, 0.5f));
    REQUIRE(find_test_stream(audio, &stream));
    CHECK(stream.volume > 0.49f && stream.volume < 0.51f);
    CHECK_OK(audio->set_volume(audio, stream.id, 1.0f));

    /* set-default, read back from the server info */
    CHECK_OK(audio->set_default_sink(audio, sink_b.id));
    REQUIRE(find_named(audio, VS_AUDIO_NODE_SINK, SINK_B, &sink_b));
    CHECK(sink_b.flags & VS_AUDIO_NODE_IS_DEFAULT);
    CHECK_OK(audio->set_default_sink(audio, sink_a.id));
    REQUIRE(find_named(audio, VS_AUDIO_NODE_SINK, SINK_A, &sink_a));
    CHECK(sink_a.flags & VS_AUDIO_NODE_IS_DEFAULT);

    /* Routing a sink input, verified by re-reading which sink it is on. */
    CHECK_OK(audio->route_stream(audio, stream.id, sink_b.id));
    REQUIRE(find_test_stream(audio, &stream));
    CHECK(stream.target_id == sink_b.id);
    CHECK_OK(audio->route_stream(audio, stream.id, 0));

    /* mute and restore every input, over the same generic bookkeeping */
    size_t changed = 0;
    CHECK_OK(audio->set_mute(audio, source_a.id, false));
    CHECK_OK(audio->mute_all_inputs(audio, true, &changed));
    CHECK(changed == 1);
    REQUIRE(find_named(audio, VS_AUDIO_NODE_SOURCE, SOURCE_A, &source_a));
    CHECK(source_a.flags & VS_AUDIO_NODE_MUTED);
    CHECK_OK(audio->mute_all_inputs(audio, false, &changed));
    REQUIRE(find_named(audio, VS_AUDIO_NODE_SOURCE, SOURCE_A, &source_a));
    CHECK(!(source_a.flags & VS_AUDIO_NODE_MUTED));

    /* Errors keep the same shape as the PipeWire backend's. */
    CHECK(audio->set_volume(audio, 0, 0.5f) == VS_ERR_INVALID);
    /* The fallback's ids carry the object kind in the top byte, so "a sink
     * index nothing answers to" has to be built with a real tag; a bare large
     * number has no kind at all and is rejected as malformed instead. Both
     * answers are right, and the test says which is which. */
    CHECK(audio->set_volume(audio, 999999, 0.5f) == VS_ERR_INVALID);
    vs_audio_id absent_sink = (1u << 24) | 0xfffeu;
    CHECK(audio->set_volume(audio, absent_sink, 0.5f) == VS_ERR_NOT_FOUND);

    audio->destroy(audio);
}

/* --- 7. capabilities -------------------------------------------------------- */

static void case_capabilities(void)
{
    /* With PipeWire running, the automatic choice is PipeWire, and it does not
     * fall through to the fallback. */
    vs_audio_system *audio = open_backend(NULL);
    REQUIRE(audio);
    CHECK(strcmp(audio->name, "pipewire") == 0);

    uint32_t caps = audio->capabilities;
    CHECK(caps & VS_AUDIO_HAS_PIPEWIRE);
    CHECK(!(caps & VS_AUDIO_HAS_PULSE_FALLBACK));
    CHECK(caps & VS_AUDIO_CAN_ROUTE_PER_STREAM);
    CHECK(caps & VS_AUDIO_CAN_BOOST_OVER_100);
    CHECK(caps & VS_AUDIO_HAS_EVENTS);
    /* The two backend bits are mutually exclusive: a caller reading them has
     * to be able to tell which one it got. */
    CHECK(((caps & VS_AUDIO_HAS_PIPEWIRE) != 0)
          != ((caps & VS_AUDIO_HAS_PULSE_FALLBACK) != 0));
    audio->destroy(audio);

    /* Forcing the fallback flips exactly those two bits and leaves the rest. */
    vs_audio_system *pulse = open_backend("libpulse");
    REQUIRE(pulse);
    uint32_t pulse_caps = pulse->capabilities;
    CHECK(pulse_caps & VS_AUDIO_HAS_PULSE_FALLBACK);
    CHECK(!(pulse_caps & VS_AUDIO_HAS_PIPEWIRE));
    CHECK(pulse_caps & VS_AUDIO_CAN_ROUTE_PER_STREAM);
    CHECK(pulse_caps & VS_AUDIO_CAN_BOOST_OVER_100);
    CHECK(pulse_caps & VS_AUDIO_HAS_EVENTS);
    pulse->destroy(pulse);

    /* PipeWire absence is detected by the connection failing, nothing else.
     * Pointing PIPEWIRE_REMOTE at a name no daemon answers to is that
     * failure, and the fallback then takes over -- which is the whole of
     * requirement 6's detection path, exercised rather than asserted. */
    setenv("PIPEWIRE_REMOTE", "vs-no-such-pipewire", 1);
    int result = VS_OK;
    vs_audio_system *none = vs_audio_system_create("pipewire", &result);
    CHECK(none == NULL);
    CHECK(result == VS_ERR_NO_BACKEND);

    vs_audio_system *fell_back = vs_audio_system_create(NULL, &result);
    REQUIRE(fell_back);
    CHECK(strcmp(fell_back->name, "libpulse") == 0);
    CHECK(fell_back->capabilities & VS_AUDIO_HAS_PULSE_FALLBACK);
    fell_back->destroy(fell_back);
    unsetenv("PIPEWIRE_REMOTE");
}

/* --- dispatch --------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: test_audio_live <case>\n");
        return 2;
    }
    const char *name = argv[1];
    static const struct { const char *name; void (*run)(void); } cases[] = {
        { "registry", case_registry },
        { "events", case_events },
        { "volume", case_volume },
        { "routing", case_routing },
        { "default_sink", case_default_sink },
        { "mute_inputs", case_mute_inputs },
        { "pulse_fallback", case_pulse_fallback },
        { "capabilities", case_capabilities },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (strcmp(cases[i].name, name) != 0) continue;
        cases[i].run();
        if (failures) {
            fprintf(stderr, "%s: %d check(s) failed\n", name, failures);
            return 1;
        }
        printf("%s: all checks passed\n", name);
        return 0;
    }
    fprintf(stderr, "unknown case %s\n", name);
    return 2;
}
