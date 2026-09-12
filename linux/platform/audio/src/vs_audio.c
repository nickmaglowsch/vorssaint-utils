/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * Backend selection and the parts of the API that are the same whichever
 * backend answered: argument checking, the volume scale, and the node-array
 * builder both backends fill.
 */

#include "vs_audio_internal.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>

/* --- the scale -------------------------------------------------------------
 *
 * The public volumes are linear amplitude with 1.0 at unity, which is what
 * PipeWire's Props `channelVolumes` holds and what the macOS mixer means by
 * 100 %. `wpctl` and the GNOME/KDE sliders are cubic: wpctl 0.5 is a linear
 * 0.125. Neither is wrong; they are different numbers for the same loudness,
 * and every conversion in this codebase goes through these two functions so
 * there is one place to be right.
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
    /* PipeWire stores channel volumes as float and WirePlumber may round-trip
     * them through its cubic scale, so an exact comparison would report a
     * successful write as a failure. A thousandth is below anything audible
     * and well above the round-trip error. */
    return fabsf(a - b) < 0.001f;
}

void vs_audio_emit(struct vs_audio *audio, vs_audio_event_kind kind,
                   uint32_t id, const char *name)
{
    if (!audio || !audio->on_event) return;
    vs_audio_event ev = { .kind = kind, .id = id, .name = name ? name : "" };
    audio->on_event(&ev, audio->user_data);
}

/* --- node array builder --------------------------------------------------- */

void vs_audio_nodes_init(struct vs_audio_nodes *b)
{
    memset(b, 0, sizeof(*b));
}

vs_audio_node *vs_audio_nodes_add(struct vs_audio_nodes *b)
{
    if (b->failed) return NULL;
    if (b->count == b->capacity) {
        size_t next = b->capacity ? b->capacity * 2 : 16;
        vs_audio_node *grown = realloc(b->items, next * sizeof(*grown));
        if (!grown) { b->failed = true; return NULL; }
        b->items = grown;
        b->capacity = next;
    }
    vs_audio_node *n = &b->items[b->count++];
    memset(n, 0, sizeof(*n));
    n->pid = -1;
    /* Until _finish runs, the string fields hold offsets into the arena, not
     * pointers. Offset 0 is a "" the builder plants first, so a field nobody
     * sets still resolves to the empty string rather than to NULL. */
    return n;
}

static bool arena_reserve(struct vs_audio_nodes *b, size_t extra)
{
    if (b->strings_len + extra <= b->strings_capacity) return true;
    size_t next = b->strings_capacity ? b->strings_capacity : 256;
    while (next < b->strings_len + extra) next *= 2;
    char *grown = realloc(b->strings, next);
    if (!grown) { b->failed = true; return false; }
    b->strings = grown;
    b->strings_capacity = next;
    return true;
}

void vs_audio_nodes_set_str(struct vs_audio_nodes *b, const char **field,
                            const char *s)
{
    if (b->failed) return;
    if (b->strings_len == 0) {
        /* plant the shared "" at offset 0 */
        if (!arena_reserve(b, 1)) return;
        b->strings[0] = '\0';
        b->strings_len = 1;
    }
    if (!s || !*s) { *field = (const char *)(uintptr_t)0; return; }
    size_t len = strlen(s) + 1;
    if (!arena_reserve(b, len)) return;
    size_t offset = b->strings_len;
    memcpy(b->strings + offset, s, len);
    b->strings_len += len;
    *field = (const char *)(uintptr_t)offset;
}

/* Resolving offsets to pointers has to happen after every string is interned,
 * because realloc moves the arena; doing it per-field as they were set would
 * leave earlier nodes pointing into a freed block. */
static void resolve(struct vs_audio_nodes *b, const char **field)
{
    size_t offset = (size_t)(uintptr_t)*field;
    *field = b->strings + offset;
}

/* What the caller actually gets a pointer into: the array is handed out, and
 * the string arena that its `const char *` fields point into rides in front of
 * it, so vs_audio_free_nodes can find the arena from the array alone and the
 * caller never has to know there are two allocations. */
struct vs_audio_nodes_block {
    char *strings;
    vs_audio_node items[];
};

int vs_audio_nodes_finish(struct vs_audio_nodes *b, vs_audio_node **out,
                          size_t *count)
{
    if (b->failed) { vs_audio_nodes_dispose(b); return -ENOMEM; }
    if (b->count == 0) {
        vs_audio_nodes_dispose(b);
        *out = NULL;
        *count = 0;
        return 0;
    }
    if (b->strings_len == 0) {
        if (!arena_reserve(b, 1)) { vs_audio_nodes_dispose(b); return -ENOMEM; }
        b->strings[0] = '\0';
        b->strings_len = 1;
    }

    struct vs_audio_nodes_block *block =
        malloc(sizeof(*block) + b->count * sizeof(vs_audio_node));
    if (!block) { vs_audio_nodes_dispose(b); return -ENOMEM; }
    memcpy(block->items, b->items, b->count * sizeof(vs_audio_node));
    block->strings = b->strings;

    /* Offsets become pointers only now: the arena stops growing at this point,
     * so resolving earlier would leave the first nodes pointing into a block
     * that a later realloc moved. */
    for (size_t i = 0; i < b->count; i++) {
        vs_audio_node *n = &block->items[i];
        resolve(b, &n->name);
        resolve(b, &n->description);
        resolve(b, &n->app_name);
        resolve(b, &n->icon_name);
        resolve(b, &n->media_name);
    }

    *out = block->items;
    *count = b->count;
    b->strings = NULL; /* the block owns it now */
    vs_audio_nodes_dispose(b);
    return 0;
}

void vs_audio_nodes_dispose(struct vs_audio_nodes *b)
{
    free(b->items);
    free(b->strings);
    memset(b, 0, sizeof(*b));
}

void vs_audio_free_nodes(vs_audio_node *nodes, size_t count)
{
    (void)count;
    if (!nodes) return;
    struct vs_audio_nodes_block *block =
        (void *)((char *)nodes - offsetof(struct vs_audio_nodes_block, items));
    free(block->strings);
    free(block);
}

/* --- open / close --------------------------------------------------------- */

vs_audio *vs_audio_open(const vs_audio_options *options)
{
    static const vs_audio_options defaults;
    if (!options) options = &defaults;
    if (options->require_pipewire && options->force_pulse) {
        errno = EINVAL;
        return NULL;
    }

    vs_audio *audio = calloc(1, sizeof(*audio));
    if (!audio) { errno = ENOMEM; return NULL; }
    audio->on_event = options->on_event;
    audio->user_data = options->user_data;
    audio->debounce_ms = options->debounce_ms ? options->debounce_ms : 80;

    int pw_err = 0;
    if (!options->force_pulse) {
        audio->vt = &vs_audio_backend_pipewire;
        pw_err = audio->vt->open(audio);
        if (pw_err == 0) return audio;
        if (options->require_pipewire) {
            free(audio);
            errno = -pw_err;
            return NULL;
        }
    }

    /* PipeWire did not answer (or was skipped). A PulseAudio-only host still
     * gets the mixer -- the sink-input API is the same shape, it just cannot
     * see the graph. */
    audio->vt = &vs_audio_backend_pulse;
    audio->caps = 0;
    int pa_err = audio->vt->open(audio);
    if (pa_err != 0) {
        free(audio);
        errno = -(pw_err ? pw_err : pa_err);
        if (errno == 0) errno = ENOENT;
        return NULL;
    }
    return audio;
}

void vs_audio_close(vs_audio *audio)
{
    if (!audio) return;
    audio->vt->close(audio);
    free(audio);
}

uint32_t vs_audio_capabilities(const vs_audio *audio)
{
    return audio ? audio->caps : 0;
}

const char *vs_audio_backend_name(const vs_audio *audio)
{
    return audio ? audio->vt->name : "";
}

/* --- forwarding, with the argument checks kept out of both backends ------- */

int vs_audio_list_nodes(vs_audio *audio, uint32_t mask,
                        vs_audio_node **out_nodes, size_t *out_count)
{
    if (!audio || !out_nodes || !out_count) return -EINVAL;
    *out_nodes = NULL;
    *out_count = 0;
    if ((mask & VS_AUDIO_NODE_ANY) == 0) return -EINVAL;
    return audio->vt->list_nodes(audio, mask, out_nodes, out_count);
}

int vs_audio_set_volume(vs_audio *audio, uint32_t id, float linear_volume)
{
    if (!audio || id == 0) return -EINVAL;
    if (isnan(linear_volume)) return -EINVAL;
    return audio->vt->set_volume(audio, id, vs_audio_clamp_volume(linear_volume));
}

int vs_audio_set_mute(vs_audio *audio, uint32_t id, bool mute)
{
    if (!audio || id == 0) return -EINVAL;
    return audio->vt->set_mute(audio, id, mute);
}

int vs_audio_route_stream(vs_audio *audio, uint32_t stream_id, uint32_t sink_id)
{
    if (!audio || stream_id == 0) return -EINVAL;
    return audio->vt->route_stream(audio, stream_id, sink_id);
}

int vs_audio_set_default_sink(vs_audio *audio, uint32_t sink_id)
{
    if (!audio || sink_id == 0) return -EINVAL;
    return audio->vt->set_default_sink(audio, sink_id);
}

/* --- mute every input ------------------------------------------------------
 *
 * Muting is the easy half; the promise is the other one -- unmuting puts back
 * exactly what this feature silenced and nothing else, because a source the
 * user muted themselves while the mic was off must stay muted. That is the
 * rule MicMuteSupport.restoreTargets enforces on macOS, and it needs a record
 * of what was muted before.
 *
 * The record is kept in a file rather than in the handle, for two reasons.
 * Sources are recorded by `node.name`, which is stable across a restart and
 * means the same thing to both backends (ids are not: PipeWire serials and
 * PulseAudio indices are both per-session). And the state has to outlive the
 * process: a crash while the mic is muted would otherwise leave the user's
 * microphone silently off with nothing left that knows to turn it back on.
 * XDG_RUNTIME_DIR is exactly the right lifetime -- it is cleared at logout,
 * which is also when a stale record stops meaning anything.
 */

static int mic_state_path(char *out, size_t len)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime) {
        if (snprintf(out, len, "%s/vorssaint-audio-micmute", runtime) >= (int)len)
            return -ENAMETOOLONG;
        return 0;
    }
    /* No runtime dir (a bare shell, a cron job): /tmp keyed by uid, so two
     * users on one machine cannot read or clobber each other's record. */
    if (snprintf(out, len, "/tmp/vorssaint-audio-micmute-%u",
                 (unsigned)getuid()) >= (int)len)
        return -ENAMETOOLONG;
    return 0;
}

/** Writes one "<0|1> <node.name>" line per source. */
static int mic_state_write(const vs_audio_node *sources, size_t count)
{
    char path[512];
    int rc = mic_state_path(path, sizeof(path));
    if (rc != 0) return rc;

    /* Written to a temporary and renamed: a record truncated by a crash
     * mid-write would be worse than no record, because it would claim the
     * unlisted sources had never been muted. */
    char tmp[560];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "we");
    if (!f) return -errno;
    for (size_t i = 0; i < count; i++) {
        if (strchr(sources[i].name, '\n')) continue; /* would break the format */
        fprintf(f, "%d %s\n", sources[i].mute ? 1 : 0, sources[i].name);
    }
    if (fflush(f) != 0 || ferror(f)) { fclose(f); unlink(tmp); return -EIO; }
    fclose(f);
    if (rename(tmp, path) != 0) { int e = -errno; unlink(tmp); return e; }
    return 0;
}

struct mic_record {
    char name[256];
    bool was_muted;
};

static int mic_state_read(struct mic_record **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    char path[512];
    int rc = mic_state_path(path, sizeof(path));
    if (rc != 0) return rc;

    FILE *f = fopen(path, "re");
    if (!f) return errno == ENOENT ? -ENOENT : -errno;

    struct mic_record *records = NULL;
    size_t n = 0, cap = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (len < 3 || (line[0] != '0' && line[0] != '1') || line[1] != ' ') continue;
        /* A name too long for the record is dropped rather than truncated: a
         * truncated name is a prefix, and a prefix can match a *different*
         * source, which would unmute a microphone this feature never touched. */
        if (len - 2 >= sizeof(((struct mic_record *)0)->name)) continue;
        if (n == cap) {
            size_t next = cap ? cap * 2 : 8;
            struct mic_record *grown = realloc(records, next * sizeof(*grown));
            if (!grown) { free(records); fclose(f); return -ENOMEM; }
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
    return 0;
}

static void mic_state_clear(void)
{
    char path[512];
    if (mic_state_path(path, sizeof(path)) == 0) unlink(path);
}

int vs_audio_mute_all_inputs(vs_audio *audio, bool mute, size_t *out_affected)
{
    if (out_affected) *out_affected = 0;
    if (!audio) return -EINVAL;

    vs_audio_node *sources = NULL;
    size_t count = 0;
    int rc = vs_audio_list_nodes(audio, VS_AUDIO_NODE_SOURCE, &sources, &count);
    if (rc != 0) return rc;

    size_t changed = 0;
    if (mute) {
        /* Record before muting: once they are all muted there is nothing left
         * to distinguish the ones that already were. */
        rc = mic_state_write(sources, count);
        if (rc != 0) { vs_audio_free_nodes(sources, count); return rc; }
        for (size_t i = 0; i < count; i++) {
            if (sources[i].mute) continue;
            int one = audio->vt->set_mute(audio, sources[i].id, true);
            if (one == 0) changed++;
            else if (one != -ENOENT) rc = one;
        }
    } else {
        struct mic_record *records = NULL;
        size_t n = 0;
        int read_rc = mic_state_read(&records, &n);
        if (read_rc == -ENOENT) {
            /* Nothing was muted by this feature, so there is nothing to undo.
             * Deliberately not "unmute everything": that would open a
             * microphone the user closed themselves. */
            vs_audio_free_nodes(sources, count);
            return 0;
        }
        if (read_rc != 0) { vs_audio_free_nodes(sources, count); return read_rc; }

        for (size_t i = 0; i < count; i++) {
            if (!sources[i].mute) continue;
            for (size_t j = 0; j < n; j++) {
                if (records[j].was_muted) continue;
                if (strcmp(records[j].name, sources[i].name) != 0) continue;
                int one = audio->vt->set_mute(audio, sources[i].id, false);
                if (one == 0) changed++;
                else if (one != -ENOENT) rc = one;
                break;
            }
        }
        free(records);
        mic_state_clear();
    }

    vs_audio_free_nodes(sources, count);
    if (out_affected) *out_affected = changed;
    return rc;
}
