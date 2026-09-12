/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * The parts of the audio backend that need no audio server: the volume scale,
 * the clamping, and the event queue that keeps the header's promise that the
 * callback runs only inside `dispatch`.
 *
 * This suite must pass everywhere, which is why it is the one that is never
 * allowed to skip.
 */

#include "vs_audio_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define CHECK_NEAR(a, b) do { \
    double va = (double)(a), vb = (double)(b); \
    if (fabs(va - vb) > 1e-4) { \
        fprintf(stderr, "FAIL %s:%d: %s (%g) != %s (%g)\n", \
                __FILE__, __LINE__, #a, va, #b, vb); \
        failures++; \
    } \
} while (0)

static void test_scale(void)
{
    /* The measured relationship this whole section rests on: `wpctl set-volume
     * ID 0.5` writes a linear channel volume of 0.125, because wpctl's scale is
     * cubic and PipeWire's channelVolumes is linear. */
    CHECK_NEAR(vs_audio_cubic_to_linear(0.5f), 0.125f);
    CHECK_NEAR(vs_audio_linear_to_cubic(0.125f), 0.5f);

    /* Unity is unity in both, which is what makes "100 %" unambiguous. */
    CHECK_NEAR(vs_audio_linear_to_cubic(1.0f), 1.0f);
    CHECK_NEAR(vs_audio_cubic_to_linear(1.0f), 1.0f);

    /* 150 % linear is what a wpctl slider shows as 1.14. */
    CHECK_NEAR(vs_audio_linear_to_cubic(VS_AUDIO_MAX_VOLUME), 1.1447f);

    /* Round trip across the range. */
    for (float v = 0.05f; v <= VS_AUDIO_MAX_VOLUME; v += 0.05f)
        CHECK_NEAR(vs_audio_cubic_to_linear(vs_audio_linear_to_cubic(v)), v);

    /* Silence and nonsense do not become a boost. */
    CHECK_NEAR(vs_audio_linear_to_cubic(0.0f), 0.0f);
    CHECK_NEAR(vs_audio_linear_to_cubic(-1.0f), 0.0f);
    CHECK_NEAR(vs_audio_cubic_to_linear(-1.0f), 0.0f);
    CHECK_NEAR(vs_audio_linear_to_cubic(nanf("")), 0.0f);
}

static void test_clamp(void)
{
    CHECK_NEAR(vs_audio_clamp_volume(0.5f), 0.5f);
    CHECK_NEAR(vs_audio_clamp_volume(-0.1f), 0.0f);
    /* The ceiling is 150 %, not the 200 % the macOS mixer allows; a request
     * above it is clamped, not refused, so a slider dragged to the end works. */
    CHECK_NEAR(vs_audio_clamp_volume(2.0f), VS_AUDIO_MAX_VOLUME);
    CHECK_NEAR(vs_audio_clamp_volume(VS_AUDIO_MAX_VOLUME), VS_AUDIO_MAX_VOLUME);
    CHECK_NEAR(vs_audio_clamp_volume(nanf("")), 0.0f);

    /* The read-back tolerance: below anything audible, above the float
     * round-trip error a cubic conversion introduces. */
    CHECK(vs_audio_volume_equal(0.5f, 0.5f));
    CHECK(vs_audio_volume_equal(0.5f, 0.5004f));
    CHECK(!vs_audio_volume_equal(0.5f, 0.51f));
}

static void test_copy_field(void)
{
    char buf[8];
    vs_audio_copy_field(buf, sizeof(buf), "abc");
    CHECK(strcmp(buf, "abc") == 0);
    /* Truncation, not overflow, and always terminated. */
    vs_audio_copy_field(buf, sizeof(buf), "0123456789");
    CHECK(strcmp(buf, "0123456") == 0);
    /* A property the server did not set is "" and never NULL, so no caller
     * needs a null check. */
    vs_audio_copy_field(buf, sizeof(buf), NULL);
    CHECK(buf[0] == '\0');
}

static void test_node_init(void)
{
    vs_audio_node node;
    memset(&node, 0xff, sizeof(node));
    vs_audio_node_init(&node);
    /* "Unknown is never zero": 0 is a real pid. */
    CHECK(node.pid == -1);
    CHECK(node.flags == 0);
    CHECK(node.id == 0);
    CHECK_NEAR(node.volume, VS_AUDIO_UNITY_VOLUME);
    /* Strings start empty rather than holding whatever was in the slot, so a
     * backend that does not set one hands the UI "" and never a stale name. */
    CHECK(node.name[0] == '\0');
    CHECK(node.app_id[0] == '\0');
    CHECK(node.transport[0] == '\0');
}

struct sink {
    int count;
    vs_audio_event_type last_type;
    vs_audio_id last_id;
    char last_name[VS_AUDIO_NAME_MAX];
};

static void record(const vs_audio_event *event, void *user_data)
{
    struct sink *s = user_data;
    s->count++;
    s->last_type = event->type;
    s->last_id = event->id;
    vs_audio_copy_field(s->last_name, sizeof(s->last_name), event->name);
}

static void test_event_queue(void)
{
    struct vs_audio_events q;
    struct sink sink = { 0 };
    vs_audio_events_init(&q, 80);
    q.callback = record;
    q.user_data = &sink;

    /* Pushing delivers nothing: the callback runs only from a drain, which is
     * what lets a blocking write pump the connection without the caller's
     * callback re-entering the backend mid-write. */
    vs_audio_events_push(&q, VS_AUDIO_EVENT_CHANGED, 0, NULL);
    vs_audio_events_push(&q, VS_AUDIO_EVENT_DEFAULT_SINK_CHANGED, 42, "vs-sink-a");
    CHECK(sink.count == 0);

    CHECK(vs_audio_events_drain(&q) == 2);
    CHECK(sink.count == 2);
    CHECK(sink.last_type == VS_AUDIO_EVENT_DEFAULT_SINK_CHANGED);
    CHECK(sink.last_id == 42);
    CHECK(strcmp(sink.last_name, "vs-sink-a") == 0);

    /* Drained is drained. */
    CHECK(vs_audio_events_drain(&q) == 0);

    /* The ring grows past its first capacity without losing or reordering
     * anything -- realloc moves the buffer, and a wrapped tail has to move with
     * it. Pushed in order, they must come back in order. */
    for (uint32_t i = 1; i <= 40; i++)
        vs_audio_events_push(&q, VS_AUDIO_EVENT_DEFAULT_SINK_CHANGED, i, NULL);
    sink.count = 0;
    CHECK(vs_audio_events_drain(&q) == 40);
    CHECK(sink.last_id == 40);

    /* Interleaving a drain with pushes exercises a head that is not 0 when the
     * ring next grows. */
    for (uint32_t i = 1; i <= 10; i++)
        vs_audio_events_push(&q, VS_AUDIO_EVENT_CHANGED, i, NULL);
    q.callback = NULL;
    CHECK(vs_audio_events_drain(&q) == 10);
    q.callback = record;
    for (uint32_t i = 1; i <= 100; i++)
        vs_audio_events_push(&q, VS_AUDIO_EVENT_DEFAULT_SOURCE_CHANGED, i, NULL);
    sink.count = 0;
    CHECK(vs_audio_events_drain(&q) == 100);
    CHECK(sink.last_id == 100);

    vs_audio_events_dispose(&q);
}

static void test_event_queue_overflow(void)
{
    struct vs_audio_events q;
    struct sink sink = { 0 };
    vs_audio_events_init(&q, 80);
    q.callback = record;
    q.user_data = &sink;

    /* A caller that stopped dispatching must not be able to make this grow
     * without bound. Past the cap the detail is dropped, and the next drain
     * says so with a CHANGED -- which is the right instruction anyway, because
     * the cure for "you missed some events" is "re-read the graph". */
    for (uint32_t i = 0; i < 5000; i++)
        vs_audio_events_push(&q, VS_AUDIO_EVENT_CHANGED, i, NULL);
    CHECK(q.count <= 256);
    CHECK(q.overflowed);

    int delivered = vs_audio_events_drain(&q);
    CHECK(delivered > 0);
    CHECK(delivered <= 257);
    CHECK(!q.overflowed);
    /* The synthesised CHANGED is appended, so it is the last thing seen. */
    CHECK(sink.last_type == VS_AUDIO_EVENT_CHANGED);
    CHECK(vs_audio_events_drain(&q) == 0);

    vs_audio_events_dispose(&q);
}

static void test_backend_names(void)
{
    const char *const *names = vs_audio_backend_names();
    CHECK(names != NULL);
    bool saw_pipewire = false, saw_pulse = false;
    for (size_t i = 0; names[i]; i++) {
        if (strcmp(names[i], "pipewire") == 0) saw_pipewire = true;
        if (strcmp(names[i], "libpulse") == 0) saw_pulse = true;
    }
    CHECK(saw_pipewire);
    CHECK(saw_pulse);

    /* An unknown name is refused rather than quietly served by a default. */
    int result = VS_OK;
    vs_audio_system *system = vs_audio_system_create("coreaudio", &result);
    CHECK(system == NULL);
    CHECK(result == VS_ERR_NO_BACKEND);
}

static void test_result_strings(void)
{
    /* Every enumerator has its own wording; a caller that prints one must not
     * get "unknown error" for a result this header defines. */
    const int results[] = { VS_OK, VS_ERR_UNSUPPORTED, VS_ERR_NOT_FOUND,
                            VS_ERR_BACKEND, VS_ERR_TIMEOUT, VS_ERR_NO_MEM,
                            VS_ERR_INVALID, VS_ERR_NO_BACKEND, VS_ERR_NOT_APPLIED };
    for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); i++)
        CHECK(strcmp(vs_result_string(results[i]), "unknown error") != 0);
    CHECK(strcmp(vs_result_string(-999), "unknown error") == 0);
}

int main(void)
{
    test_scale();
    test_clamp();
    test_copy_field();
    test_node_init();
    test_event_queue();
    test_event_queue_overflow();
    test_backend_names();
    test_result_strings();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_audio_support: all checks passed\n");
    return 0;
}
