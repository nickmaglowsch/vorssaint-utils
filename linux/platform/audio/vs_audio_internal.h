/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * What the two audio backends share. Nothing here is part of the contract;
 * callers see only vorssaint_platform.h.
 */

#ifndef VS_AUDIO_INTERNAL_H
#define VS_AUDIO_INTERNAL_H

#include "vorssaint_platform.h"

#include <stdlib.h>
#include <string.h>

/* --- the event queue -------------------------------------------------------
 *
 * The header's threading rule is that the callback runs only inside
 * `dispatch`, on the thread that called it. Both backends have to run their
 * connection during a blocking call too -- a volume write is not finished
 * until the server has confirmed it, and confirming means processing whatever
 * else the server sent in the meantime. Those events cannot be delivered from
 * inside `set_volume`, so they are queued here and handed over by the next
 * `dispatch` instead.
 */

struct vs_audio_queued {
    vs_audio_event_type type;
    vs_audio_id id;
    char name[VS_AUDIO_NAME_MAX];
};

struct vs_audio_events {
    vs_audio_event_cb callback;
    void *user_data;
    struct vs_audio_queued *items;
    size_t head, count, capacity;
    /**
     * Set when the queue overflowed. The reaction is not to grow without
     * bound: a caller that has stopped dispatching is told, once, that the
     * detail is gone and it should re-read everything -- which is what a
     * CHANGED means anyway.
     */
    bool overflowed;
    unsigned debounce_ms;
};

void vs_audio_events_init(struct vs_audio_events *q, unsigned debounce_ms);
void vs_audio_events_dispose(struct vs_audio_events *q);
/** Appends an event. `name` may be NULL. */
void vs_audio_events_push(struct vs_audio_events *q, vs_audio_event_type type,
                          vs_audio_id id, const char *name);
/** Delivers everything queued to the callback. Returns how many were delivered. */
int vs_audio_events_drain(struct vs_audio_events *q);

/* --- shared helpers -------------------------------------------------------- */

/** Clamps to [0, VS_AUDIO_MAX_VOLUME] and turns NaN into 0. */
float vs_audio_clamp_volume(float v);
/** Are two linear volumes the same as far as a read-back check is concerned? */
bool vs_audio_volume_equal(float a, float b);
/** CLOCK_MONOTONIC in milliseconds, for the blocking calls' budgets. */
uint64_t vs_audio_now_ms(void);
/** snprintf into a fixed field, without the truncation warning dance. */
void vs_audio_copy_field(char *dst, size_t len, const char *src);

/** Grows `*nodes` when `count == *capacity`. Returns false on ENOMEM. */
bool vs_audio_nodes_reserve(vs_audio_node **nodes, size_t count, size_t *capacity);
/** Zeroes a node and sets the fields whose "unknown" is not 0. */
void vs_audio_node_init(vs_audio_node *node);

/* Backend constructors. Each returns NULL and sets *result_out on failure. */
vs_audio_system *vs_audio_pipewire_create(int *result_out);
vs_audio_system *vs_audio_pulse_create(int *result_out);

/**
 * The session-lifetime record of which sources `mute_all_inputs` muted.
 *
 * Kept out of both backends because it is the same logic for each and because
 * it is keyed by `node.name`, which is the one identifier that means the same
 * thing to both (PipeWire serials and PulseAudio indices are per-session) and
 * survives a restart.
 */
int vs_audio_mute_all_inputs_generic(vs_audio_system *self, bool mute,
                                     size_t *changed_out);

#endif /* VS_AUDIO_INTERNAL_H */
