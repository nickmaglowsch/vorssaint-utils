/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * Backend selection, the volume scale, the event queue, and the one operation
 * that is the same whichever backend answered: muting every input.
 */

#include "vs_audio_internal.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* --- the scale -------------------------------------------------------------
 *
 * Public volumes are linear amplitude with 1.0 at unity: what PipeWire's
 * `channelVolumes` holds, and what the macOS mixer means by 100 %. `wpctl` and
 * the GNOME/KDE sliders are cubic -- wpctl 0.5 is a linear 0.125. Neither is
 * wrong; they are different numbers for the same loudness, and every
 * conversion in this codebase goes through these two functions so there is one
 * place to be right.
 */

float vs_audio_linear_to_cubic(float linear)
{
    if (!(linear > 0.0f)) return 0.0f; /* also catches NaN */
    return cbrtf(linear);
}

float vs_audio_cubic_to_linear(float cubic)
{
    if (!(cubic > 0.0f)) return 0.0f;
    return cubic * cubic * cubic;
}

float vs_audio_clamp_volume(float v)
{
    if (isnan(v)) return 0.0f;
    if (v < 0.0f) return 0.0f;
    if (v > VS_AUDIO_MAX_VOLUME) return VS_AUDIO_MAX_VOLUME;
    return v;
}

bool vs_audio_volume_equal(float a, float b)
{
    /* Channel volumes are floats that may have been round-tripped through a
     * cubic scale on the way, so an exact comparison would report a successful
     * write as a failure. A thousandth is inaudible and well above the
     * round-trip error. */
    return fabsf(a - b) < 0.001f;
}

uint64_t vs_audio_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

void vs_audio_copy_field(char *dst, size_t len, const char *src)
{
    if (len == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= len) n = len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void vs_audio_node_init(vs_audio_node *node)
{
    memset(node, 0, sizeof(*node));
    /* "Unknown is never zero": 0 is a real pid. */
    node->pid = -1;
    node->volume = VS_AUDIO_UNITY_VOLUME;
}

bool vs_audio_nodes_reserve(vs_audio_node **nodes, size_t count, size_t *capacity)
{
    if (count < *capacity) return true;
    size_t next = *capacity ? *capacity * 2 : 16;
    vs_audio_node *grown = realloc(*nodes, next * sizeof(**nodes));
    if (!grown) return false;
    *nodes = grown;
    *capacity = next;
    return true;
}

/* --- event queue ----------------------------------------------------------- */

#define VS_AUDIO_QUEUE_MAX 256

void vs_audio_events_init(struct vs_audio_events *q, unsigned debounce_ms)
{
    memset(q, 0, sizeof(*q));
    q->debounce_ms = debounce_ms;
}

void vs_audio_events_dispose(struct vs_audio_events *q)
{
    free(q->items);
    memset(q, 0, sizeof(*q));
}

void vs_audio_events_push(struct vs_audio_events *q, vs_audio_event_type type,
                          vs_audio_id id, const char *name)
{
    if (q->count == q->capacity) {
        if (q->capacity >= VS_AUDIO_QUEUE_MAX) {
            /* A caller that stopped dispatching does not need every event it
             * missed; it needs to be told to re-read. Dropping the detail and
             * keeping the ring bounded is the honest answer, and the flag makes
             * sure a CHANGED still reaches it. */
            q->overflowed = true;
            return;
        }
        size_t next = q->capacity ? q->capacity * 2 : 16;
        if (next > VS_AUDIO_QUEUE_MAX) next = VS_AUDIO_QUEUE_MAX;
        struct vs_audio_queued *grown = realloc(q->items, next * sizeof(*grown));
        if (!grown) { q->overflowed = true; return; }
        /* realloc keeps the ring's contents but not its wrap point, so the
         * tail that wrapped past the old end has to move to the new one. */
        if (q->head + q->count > q->capacity) {
            size_t wrapped = q->head + q->count - q->capacity;
            memmove(grown + q->capacity, grown, wrapped * sizeof(*grown));
        }
        q->items = grown;
        q->capacity = next;
    }
    struct vs_audio_queued *slot = &q->items[(q->head + q->count) % q->capacity];
    slot->type = type;
    slot->id = id;
    vs_audio_copy_field(slot->name, sizeof(slot->name), name);
    q->count++;
}

static void deliver(struct vs_audio_events *q, const struct vs_audio_queued *item)
{
    if (!q->callback) return;
    vs_audio_event event = { .type = item->type, .id = item->id, .name = item->name };
    q->callback(&event, q->user_data);
}

int vs_audio_events_drain(struct vs_audio_events *q)
{
    int delivered = 0;
    while (q->count > 0) {
        struct vs_audio_queued item = q->items[q->head];
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        delivered++;
        deliver(q, &item);
    }
    if (q->overflowed) {
        /* Whatever was lost, the caller's cure is the same: re-read the graph.
         * Delivered after the queue is empty and without going through the
         * queue itself -- pushing it while the ring was still full would have
         * been dropped and set the flag straight back, leaving a backend that
         * announced an overflow for ever. */
        q->overflowed = false;
        struct vs_audio_queued resync = { .type = VS_AUDIO_EVENT_CHANGED,
                                          .id = 0, .name = "" };
        delivered++;
        deliver(q, &resync);
    }
    return delivered;
}

/* --- mute every input ------------------------------------------------------
 *
 * Muting is the easy half; the promise is the other one -- unmuting puts back
 * exactly what this feature silenced and nothing else, because a source the
 * user muted themselves while the mic was off must stay muted. That is the rule
 * `MicMuteSupport.restoreTargets` enforces on macOS, and it needs a record of
 * what was muted before.
 *
 * The record is a file, not a field, for two reasons. Sources are recorded by
 * `node.name`, which means the same thing to both backends and survives a
 * restart, where ids do not. And the state has to outlive the process: a crash
 * with the mic muted would otherwise leave a microphone silently off with
 * nothing left that knows to turn it back on. XDG_RUNTIME_DIR is the right
 * lifetime -- cleared at logout, which is also when a stale record stops
 * meaning anything.
 */

struct mic_record {
    char name[VS_AUDIO_NAME_MAX];
    bool was_muted;
};

static int mic_state_path(char *out, size_t len)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime) {
        if (snprintf(out, len, "%s/vorssaint-audio-micmute", runtime) >= (int)len)
            return VS_ERR_INVALID;
        return VS_OK;
    }
    /* No runtime dir (a bare shell, a cron job): /tmp keyed by uid, so two
     * users on one machine cannot read or clobber each other's record. */
    if (snprintf(out, len, "/tmp/vorssaint-audio-micmute-%u",
                 (unsigned)getuid()) >= (int)len)
        return VS_ERR_INVALID;
    return VS_OK;
}

/** Writes one "<0|1> <node.name>" line per source. */
static int mic_state_write(const vs_audio_node *sources, size_t count)
{
    char path[512];
    int rc = mic_state_path(path, sizeof(path));
    if (rc != VS_OK) return rc;

    /* Written to a temporary and renamed: a record truncated by a crash
     * mid-write is worse than no record, because it would claim the sources it
     * never got to had not been muted. */
    char tmp[560];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "we");
    if (!f) return VS_ERR_BACKEND;
    for (size_t i = 0; i < count; i++) {
        if (strchr(sources[i].name, '\n')) continue; /* would break the format */
        fprintf(f, "%d %s\n", (sources[i].flags & VS_AUDIO_NODE_MUTED) ? 1 : 0,
                sources[i].name);
    }
    bool bad = fflush(f) != 0 || ferror(f);
    fclose(f);
    if (bad) { unlink(tmp); return VS_ERR_BACKEND; }
    if (rename(tmp, path) != 0) { unlink(tmp); return VS_ERR_BACKEND; }
    return VS_OK;
}

static int mic_state_read(struct mic_record **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    char path[512];
    int rc = mic_state_path(path, sizeof(path));
    if (rc != VS_OK) return rc;

    FILE *f = fopen(path, "re");
    if (!f) return errno == ENOENT ? VS_ERR_NOT_FOUND : VS_ERR_BACKEND;

    struct mic_record *records = NULL;
    size_t n = 0, cap = 0;
    char line[VS_AUDIO_NAME_MAX + 8];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (len < 3 || (line[0] != '0' && line[0] != '1') || line[1] != ' ') continue;
        /* A name too long for the record is dropped rather than truncated: a
         * truncated name is a prefix, and a prefix can match a *different*
         * source, which would unmute a microphone this feature never touched. */
        if (len - 2 >= sizeof(records->name)) continue;
        if (n == cap) {
            size_t next = cap ? cap * 2 : 8;
            struct mic_record *grown = realloc(records, next * sizeof(*grown));
            if (!grown) { free(records); fclose(f); return VS_ERR_NO_MEM; }
            records = grown;
            cap = next;
        }
        records[n].was_muted = line[0] == '1';
        memcpy(records[n].name, line + 2, len - 2);
        records[n].name[len - 2] = '\0';
        n++;
    }
    fclose(f);
    *out = records;
    *count = n;
    return VS_OK;
}

static void mic_state_clear(void)
{
    char path[512];
    if (mic_state_path(path, sizeof(path)) == VS_OK) unlink(path);
}

int vs_audio_mute_all_inputs_generic(vs_audio_system *self, bool mute,
                                     size_t *changed_out)
{
    vs_audio_node *sources = NULL;
    size_t count = 0;
    int rc = self->list(self, VS_AUDIO_NODE_SOURCE, &sources, &count);
    if (rc != VS_OK) return rc;

    size_t changed = 0;
    if (mute) {
        /* Recorded before anything is muted: once they are all muted there is
         * nothing left to tell apart the ones that already were. */
        rc = mic_state_write(sources, count);
        if (rc != VS_OK) { self->free_list(self, sources, count); return rc; }
        for (size_t i = 0; i < count; i++) {
            if (sources[i].flags & VS_AUDIO_NODE_MUTED) continue;
            int one = self->set_mute(self, sources[i].id, true);
            if (one == VS_OK) changed++;
            else if (one != VS_ERR_NOT_FOUND) rc = one;
        }
    } else {
        struct mic_record *records = NULL;
        size_t n = 0;
        int read_rc = mic_state_read(&records, &n);
        if (read_rc == VS_ERR_NOT_FOUND) {
            /* Nothing was muted by this feature, so there is nothing to undo.
             * Deliberately not "unmute everything": that would open a
             * microphone the user closed themselves. */
            self->free_list(self, sources, count);
            if (changed_out) *changed_out = 0;
            return VS_OK;
        }
        if (read_rc != VS_OK) { self->free_list(self, sources, count); return read_rc; }

        for (size_t i = 0; i < count; i++) {
            if (!(sources[i].flags & VS_AUDIO_NODE_MUTED)) continue;
            for (size_t j = 0; j < n; j++) {
                if (records[j].was_muted) continue;
                if (strcmp(records[j].name, sources[i].name) != 0) continue;
                int one = self->set_mute(self, sources[i].id, false);
                if (one == VS_OK) changed++;
                else if (one != VS_ERR_NOT_FOUND) rc = one;
                break;
            }
        }
        free(records);
        mic_state_clear();
    }

    self->free_list(self, sources, count);
    if (changed_out) *changed_out = changed;
    return rc;
}

/* --- construction ---------------------------------------------------------- */

static const char *const backend_names[] = { "pipewire", "libpulse", NULL };

const char *const *vs_audio_backend_names(void)
{
    return backend_names;
}

vs_audio_system *vs_audio_system_create(const char *preferred, int *result_out)
{
    int result = VS_ERR_NO_BACKEND;
    vs_audio_system *system = NULL;

    if (preferred && strcmp(preferred, "libpulse") == 0) {
        system = vs_audio_pulse_create(&result);
    } else if (preferred && strcmp(preferred, "pipewire") == 0) {
        system = vs_audio_pipewire_create(&result);
    } else if (preferred) {
        result = VS_ERR_NO_BACKEND;
    } else {
        system = vs_audio_pipewire_create(&result);
        if (!system) {
            /* PipeWire did not answer. A PulseAudio-only host still gets the
             * mixer: the sink-input API is the same shape under a different
             * name, and losing per-app volume entirely on those machines would
             * be a worse answer than a backend with fewer capability bits. */
            int pulse_result = VS_ERR_NO_BACKEND;
            system = vs_audio_pulse_create(&pulse_result);
            if (system) result = VS_OK;
        }
    }

    if (result_out) *result_out = system ? VS_OK : result;
    return system;
}
