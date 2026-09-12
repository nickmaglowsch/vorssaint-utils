/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * What the two backends share, and the vtable `vs_audio_open` dispatches
 * through. Nothing here is part of the public surface; callers see only
 * vorssaint_platform.h.
 */

#ifndef VS_AUDIO_INTERNAL_H
#define VS_AUDIO_INTERNAL_H

#include "vorssaint_platform.h"

#include <stdlib.h>
#include <string.h>

struct vs_audio_backend;

struct vs_audio {
    const struct vs_audio_backend *vt;
    void *impl;
    uint32_t caps;
    vs_audio_event_fn on_event;
    void *user_data;
    unsigned debounce_ms;
};

struct vs_audio_backend {
    const char *name;
    /** Returns a negative errno on failure and leaves nothing behind. */
    int (*open)(struct vs_audio *audio);
    void (*close)(struct vs_audio *audio);
    int (*list_nodes)(struct vs_audio *audio, uint32_t mask,
                      vs_audio_node **out, size_t *count);
    int (*set_volume)(struct vs_audio *audio, uint32_t id, float v);
    int (*set_mute)(struct vs_audio *audio, uint32_t id, bool mute);
    int (*route_stream)(struct vs_audio *audio, uint32_t stream, uint32_t sink);
    int (*set_default_sink)(struct vs_audio *audio, uint32_t sink);
};

extern const struct vs_audio_backend vs_audio_backend_pipewire;
extern const struct vs_audio_backend vs_audio_backend_pulse;

/* --- node array building ---------------------------------------------------
 *
 * Both backends produce the same thing: an array of vs_audio_node whose
 * strings live in one allocation behind the array, so vs_audio_free_nodes is
 * two frees and callers never see a half-owned struct. The builder grows the
 * array and interns strings; `vs_audio_nodes_finish` hands over ownership.
 */
struct vs_audio_nodes {
    vs_audio_node *items;
    size_t count, capacity;
    char *strings;
    size_t strings_len, strings_capacity;
    bool failed;
};

void vs_audio_nodes_init(struct vs_audio_nodes *b);
/** Appends a node; strings are copied. Sets `failed` on allocation failure. */
vs_audio_node *vs_audio_nodes_add(struct vs_audio_nodes *b);
/**
 * Copies `s` (NULL becomes "") into the builder's string arena and records an
 * offset in `*field`. Offsets are turned into pointers by _finish, because the
 * arena moves as it grows.
 */
void vs_audio_nodes_set_str(struct vs_audio_nodes *b, const char **field,
                            const char *s);
/** Resolves the interned offsets and hands ownership to the caller. */
int vs_audio_nodes_finish(struct vs_audio_nodes *b, vs_audio_node **out,
                          size_t *count);
void vs_audio_nodes_dispose(struct vs_audio_nodes *b);

/** Clamps to [0, VS_AUDIO_MAX_VOLUME] and rejects NaN. */
float vs_audio_clamp_volume(float v);
/** Are two linear volumes the same as far as a read-back check is concerned? */
bool vs_audio_volume_equal(float a, float b);

void vs_audio_emit(struct vs_audio *audio, vs_audio_event_kind kind,
                   uint32_t id, const char *name);

#endif /* VS_AUDIO_INTERNAL_H */
