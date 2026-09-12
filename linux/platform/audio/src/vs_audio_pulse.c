/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * The libpulse fallback.
 *
 * Why it exists: PipeWire is the assumption, not the requirement. A host still
 * running PulseAudio (older LTS installs, and distributions that have not
 * switched) has the same sink-input API underneath a different name, and the
 * mixer's core -- list the devices, list what is playing, set a per-app volume,
 * switch the default output -- maps onto it directly. Losing per-app volume
 * entirely on those machines would be a worse answer than a backend with a
 * smaller capability set, which is what the capability bits are for.
 *
 * What it cannot do, and says so through the capability bits: there is no
 * graph, so a routing request cannot be verified by watching links move -- it
 * is verified by re-reading which sink the input now sits on, which is the
 * strongest read-back the protocol offers.
 *
 * Everything runs on a pa_threaded_mainloop; the public functions block on the
 * caller's thread until the server has answered, so the API is the same shape
 * as the PipeWire backend's.
 */

#include "vs_audio_internal.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

#include <pulse/pulseaudio.h>

/*
 * PulseAudio numbers sinks, sources, sink-inputs and source-outputs in four
 * separate index spaces, so an index alone does not identify an object the way
 * a PipeWire object.serial does. The public id therefore carries the kind in
 * its top byte. Indices in practice are small sequence numbers; an index that
 * did not fit would be listed with an id of 0 rather than aliased onto another
 * object.
 */
#define PA_TAG_SHIFT 24
#define PA_TAG_MASK 0xffu
#define PA_INDEX_MAX ((1u << PA_TAG_SHIFT) - 1u)

enum { TAG_SINK = 1, TAG_SOURCE = 2, TAG_SINK_INPUT = 3, TAG_SOURCE_OUTPUT = 4 };

static uint32_t pack_id(unsigned tag, uint32_t index)
{
    if (index > PA_INDEX_MAX) return 0;
    return ((uint32_t)tag << PA_TAG_SHIFT) | index;
}
static unsigned id_tag(uint32_t id) { return (id >> PA_TAG_SHIFT) & PA_TAG_MASK; }
static uint32_t id_index(uint32_t id) { return id & PA_INDEX_MAX; }

struct pa_backend {
    struct vs_audio *audio;
    pa_threaded_mainloop *loop;
    pa_context *context;

    char default_sink[256];
    char default_source[256];

    /** Set by the info callbacks that a query is collecting into. */
    struct vs_audio_nodes *collecting;
    uint32_t collect_mask;
    int last_result;

    /* scratch for the single-object queries the read-backs use */
    uint32_t probe_id;
    float probe_volume;
    bool probe_mute;
    uint32_t probe_sink_index;
    char probe_name[256];
    uint8_t probe_channels;
    bool probe_found;

    pa_time_event *debounce;
    bool debounce_armed;
};

/* --- volume scale ----------------------------------------------------------
 *
 * pa_volume_t is not linear: PA_VOLUME_NORM is unity and the scale is cubic,
 * which is the same curve wpctl uses and the same one the public API of this
 * backend does *not* use. pa_sw_volume_from_linear / _to_linear are the
 * library's own conversions between the two, so the linear amplitude this
 * backend promises survives the trip in both directions.
 */
static pa_volume_t to_pa(float linear)
{
    return pa_sw_volume_from_linear((double)linear);
}
static float from_pa(pa_volume_t v)
{
    return (float)pa_sw_volume_to_linear(v);
}

/* --- mainloop plumbing ---------------------------------------------------- */

static void signal_loop(struct pa_backend *be) { pa_threaded_mainloop_signal(be->loop, 0); }

static void context_state_cb(pa_context *c, void *userdata)
{
    (void)c;
    signal_loop(userdata);
}

/** Waits for an operation to finish. Caller holds the mainloop lock. */
static int wait_op(struct pa_backend *be, pa_operation *op)
{
    if (!op) return -EIO;
    int rc = 0;
    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
        if (pa_context_get_state(be->context) != PA_CONTEXT_READY) {
            rc = -EIO;
            pa_operation_cancel(op);
            break;
        }
        pa_threaded_mainloop_wait(be->loop);
    }
    pa_operation_unref(op);
    return rc;
}

static void success_cb(pa_context *c, int success, void *userdata)
{
    (void)c;
    struct pa_backend *be = userdata;
    be->last_result = success ? 0 : -EIO;
    signal_loop(be);
}

static void on_debounce(pa_mainloop_api *api, pa_time_event *e,
                        const struct timeval *tv, void *userdata)
{
    (void)api; (void)e; (void)tv;
    struct pa_backend *be = userdata;
    be->debounce_armed = false;
    vs_audio_emit(be->audio, VS_AUDIO_EVENT_CHANGED, 0, "");
}

static void schedule_changed(struct pa_backend *be)
{
    if (be->debounce_armed || !be->debounce) return;
    be->debounce_armed = true;
    pa_context_rttime_restart(be->context, be->debounce,
                              pa_rtclock_now() + be->audio->debounce_ms * 1000ull);
}

static void subscribe_cb(pa_context *c, pa_subscription_event_type_t type,
                         uint32_t index, void *userdata)
{
    (void)c; (void)index;
    struct pa_backend *be = userdata;
    /* The event says a facility changed but not what to; every reader of this
     * backend re-queries, so one debounced CHANGED is the whole reaction. A
     * server event is a default-device move, which list_nodes picks up when it
     * refreshes the server info. */
    (void)type;
    schedule_changed(be);
}

/* --- listing -------------------------------------------------------------- */

static void fill_common(struct vs_audio_nodes *b, vs_audio_node *out,
                        uint32_t id, vs_audio_node_kind kind,
                        const char *name, const char *description,
                        pa_volume_t volume, int mute)
{
    out->id = id;
    out->pw_global_id = 0;
    out->kind = kind;
    out->volume = from_pa(volume);
    out->mute = mute != 0;
    out->has_volume = true;
    vs_audio_nodes_set_str(b, &out->name, name);
    vs_audio_nodes_set_str(b, &out->description, description);
}

static void sink_info_cb(pa_context *c, const pa_sink_info *i, int eol, void *ud)
{
    (void)c;
    struct pa_backend *be = ud;
    if (eol) { signal_loop(be); return; }
    if (!be->collecting || !(be->collect_mask & VS_AUDIO_NODE_SINK)) return;
    vs_audio_node *out = vs_audio_nodes_add(be->collecting);
    if (!out) return;
    fill_common(be->collecting, out, pack_id(TAG_SINK, i->index),
                VS_AUDIO_NODE_SINK, i->name, i->description,
                pa_cvolume_max(&i->volume), i->mute);
    out->is_default = strcmp(i->name ? i->name : "", be->default_sink) == 0;
}

static void source_info_cb(pa_context *c, const pa_source_info *i, int eol, void *ud)
{
    (void)c;
    struct pa_backend *be = ud;
    if (eol) { signal_loop(be); return; }
    if (!be->collecting || !(be->collect_mask & VS_AUDIO_NODE_SOURCE)) return;
    /* A sink's monitor source is listed by PulseAudio as an ordinary source.
     * Muting every input must not mute monitors -- that is system audio
     * capture, not a microphone -- so they are left out of the listing
     * entirely, matching what the macOS input list shows. */
    if (i->monitor_of_sink != PA_INVALID_INDEX) return;
    vs_audio_node *out = vs_audio_nodes_add(be->collecting);
    if (!out) return;
    fill_common(be->collecting, out, pack_id(TAG_SOURCE, i->index),
                VS_AUDIO_NODE_SOURCE, i->name, i->description,
                pa_cvolume_max(&i->volume), i->mute);
    out->is_default = strcmp(i->name ? i->name : "", be->default_source) == 0;
}

static void fill_stream_props(struct vs_audio_nodes *b, vs_audio_node *out,
                              pa_proplist *props)
{
    if (!props) return;
    const char *app = pa_proplist_gets(props, PA_PROP_APPLICATION_NAME);
    const char *icon = pa_proplist_gets(props, PA_PROP_APPLICATION_ICON_NAME);
    const char *media = pa_proplist_gets(props, PA_PROP_MEDIA_NAME);
    const char *pid = pa_proplist_gets(props, PA_PROP_APPLICATION_PROCESS_ID);
    vs_audio_nodes_set_str(b, &out->app_name, app);
    vs_audio_nodes_set_str(b, &out->icon_name, icon);
    vs_audio_nodes_set_str(b, &out->media_name, media);
    if (pid) out->pid = (int32_t)strtol(pid, NULL, 10);
}

static void sink_input_cb(pa_context *c, const pa_sink_input_info *i, int eol,
                          void *ud)
{
    (void)c;
    struct pa_backend *be = ud;
    if (eol) { signal_loop(be); return; }
    if (!be->collecting || !(be->collect_mask & VS_AUDIO_NODE_STREAM_OUTPUT)) return;
    vs_audio_node *out = vs_audio_nodes_add(be->collecting);
    if (!out) return;
    fill_common(be->collecting, out, pack_id(TAG_SINK_INPUT, i->index),
                VS_AUDIO_NODE_STREAM_OUTPUT, i->name, i->name,
                pa_cvolume_max(&i->volume), i->mute);
    /* The sink this input is on is both the route and the observed landing
     * place; PulseAudio does not separate "asked for" from "arrived at". */
    out->target_id = pack_id(TAG_SINK, i->sink);
    out->linked_pw_global_id = 0;
    fill_stream_props(be->collecting, out, i->proplist);
}

static void source_output_cb(pa_context *c, const pa_source_output_info *i,
                             int eol, void *ud)
{
    (void)c;
    struct pa_backend *be = ud;
    if (eol) { signal_loop(be); return; }
    if (!be->collecting || !(be->collect_mask & VS_AUDIO_NODE_STREAM_INPUT)) return;
    vs_audio_node *out = vs_audio_nodes_add(be->collecting);
    if (!out) return;
    fill_common(be->collecting, out, pack_id(TAG_SOURCE_OUTPUT, i->index),
                VS_AUDIO_NODE_STREAM_INPUT, i->name, i->name,
                pa_cvolume_max(&i->volume), i->mute);
    out->target_id = pack_id(TAG_SOURCE, i->source);
    fill_stream_props(be->collecting, out, i->proplist);
}

static void server_info_cb(pa_context *c, const pa_server_info *i, void *ud)
{
    (void)c;
    struct pa_backend *be = ud;
    if (i) {
        snprintf(be->default_sink, sizeof(be->default_sink), "%s",
                 i->default_sink_name ? i->default_sink_name : "");
        snprintf(be->default_source, sizeof(be->default_source), "%s",
                 i->default_source_name ? i->default_source_name : "");
    }
    signal_loop(be);
}

/** Refreshes the cached default names. Caller holds the mainloop lock. */
static int refresh_server_info(struct pa_backend *be)
{
    return wait_op(be, pa_context_get_server_info(be->context, server_info_cb, be));
}

static int pulse_list_nodes(struct vs_audio *audio, uint32_t mask,
                            vs_audio_node **out, size_t *count)
{
    struct pa_backend *be = audio->impl;
    struct vs_audio_nodes b;
    vs_audio_nodes_init(&b);

    pa_threaded_mainloop_lock(be->loop);
    int rc = refresh_server_info(be);
    be->collecting = &b;
    be->collect_mask = mask;
    if (rc == 0 && (mask & VS_AUDIO_NODE_SINK))
        rc = wait_op(be, pa_context_get_sink_info_list(be->context, sink_info_cb, be));
    if (rc == 0 && (mask & VS_AUDIO_NODE_SOURCE))
        rc = wait_op(be, pa_context_get_source_info_list(be->context, source_info_cb, be));
    if (rc == 0 && (mask & VS_AUDIO_NODE_STREAM_OUTPUT))
        rc = wait_op(be, pa_context_get_sink_input_info_list(be->context, sink_input_cb, be));
    if (rc == 0 && (mask & VS_AUDIO_NODE_STREAM_INPUT))
        rc = wait_op(be, pa_context_get_source_output_info_list(be->context,
                                                                source_output_cb, be));
    be->collecting = NULL;
    pa_threaded_mainloop_unlock(be->loop);

    if (rc != 0) { vs_audio_nodes_dispose(&b); return rc; }
    return vs_audio_nodes_finish(&b, out, count);
}

/* --- single-object probes, for read-back ---------------------------------- */

static void probe_sink_cb(pa_context *c, const pa_sink_info *i, int eol, void *ud)
{
    (void)c;
    struct pa_backend *be = ud;
    if (eol) { signal_loop(be); return; }
    be->probe_found = true;
    be->probe_volume = from_pa(pa_cvolume_max(&i->volume));
    be->probe_mute = i->mute != 0;
    be->probe_channels = i->volume.channels;
    snprintf(be->probe_name, sizeof(be->probe_name), "%s", i->name ? i->name : "");
}

static void probe_source_cb(pa_context *c, const pa_source_info *i, int eol, void *ud)
{
    (void)c;
    struct pa_backend *be = ud;
    if (eol) { signal_loop(be); return; }
    be->probe_found = true;
    be->probe_volume = from_pa(pa_cvolume_max(&i->volume));
    be->probe_mute = i->mute != 0;
    be->probe_channels = i->volume.channels;
}

static void probe_sink_input_cb(pa_context *c, const pa_sink_input_info *i,
                                int eol, void *ud)
{
    (void)c;
    struct pa_backend *be = ud;
    if (eol) { signal_loop(be); return; }
    be->probe_found = true;
    be->probe_volume = from_pa(pa_cvolume_max(&i->volume));
    be->probe_mute = i->mute != 0;
    be->probe_channels = i->volume.channels;
    be->probe_sink_index = i->sink;
}

static void probe_source_output_cb(pa_context *c, const pa_source_output_info *i,
                                   int eol, void *ud)
{
    (void)c;
    struct pa_backend *be = ud;
    if (eol) { signal_loop(be); return; }
    be->probe_found = true;
    be->probe_volume = from_pa(pa_cvolume_max(&i->volume));
    be->probe_mute = i->mute != 0;
    be->probe_channels = i->volume.channels;
}

/** Re-reads one object. Caller holds the mainloop lock. */
static int probe(struct pa_backend *be, uint32_t id)
{
    be->probe_found = false;
    be->probe_channels = 0;
    be->probe_name[0] = '\0';
    be->probe_sink_index = PA_INVALID_INDEX;
    uint32_t index = id_index(id);
    pa_operation *op = NULL;
    switch (id_tag(id)) {
    case TAG_SINK:
        op = pa_context_get_sink_info_by_index(be->context, index, probe_sink_cb, be);
        break;
    case TAG_SOURCE:
        op = pa_context_get_source_info_by_index(be->context, index, probe_source_cb, be);
        break;
    case TAG_SINK_INPUT:
        op = pa_context_get_sink_input_info(be->context, index, probe_sink_input_cb, be);
        break;
    case TAG_SOURCE_OUTPUT:
        op = pa_context_get_source_output_info(be->context, index,
                                               probe_source_output_cb, be);
        break;
    default:
        return -EINVAL;
    }
    int rc = wait_op(be, op);
    if (rc != 0) return rc;
    return be->probe_found ? 0 : -ENOENT;
}

/* --- writes --------------------------------------------------------------- */

static int pulse_set_volume(struct vs_audio *audio, uint32_t id, float linear)
{
    struct pa_backend *be = audio->impl;
    uint32_t index = id_index(id);

    pa_threaded_mainloop_lock(be->loop);
    /* The channel map has to come from the object itself: writing a stereo
     * cvolume onto a mono source sets a channel that is not there, and
     * PulseAudio answers with a failure rather than a guess. */
    int rc = probe(be, id);
    if (rc != 0) { pa_threaded_mainloop_unlock(be->loop); return rc; }

    switch (id_tag(id)) {
    case TAG_SINK:
    case TAG_SOURCE:
    case TAG_SINK_INPUT:
    case TAG_SOURCE_OUTPUT:
        break;
    default:
        pa_threaded_mainloop_unlock(be->loop);
        return -EINVAL;
    }
    pa_cvolume cv;
    unsigned channels = be->probe_channels ? be->probe_channels : 2u;
    pa_cvolume_set(&cv, channels, to_pa(linear));

    pa_operation *op = NULL;
    switch (id_tag(id)) {
    case TAG_SINK:
        op = pa_context_set_sink_volume_by_index(be->context, index, &cv,
                                                 success_cb, be);
        break;
    case TAG_SOURCE:
        op = pa_context_set_source_volume_by_index(be->context, index, &cv,
                                                   success_cb, be);
        break;
    case TAG_SINK_INPUT:
        op = pa_context_set_sink_input_volume(be->context, index, &cv,
                                              success_cb, be);
        break;
    case TAG_SOURCE_OUTPUT:
        op = pa_context_set_source_output_volume(be->context, index, &cv,
                                                 success_cb, be);
        break;
    }
    be->last_result = 0;
    rc = wait_op(be, op);
    if (rc == 0) rc = be->last_result;
    if (rc == 0) {
        rc = probe(be, id);
        if (rc == 0 && !vs_audio_volume_equal(be->probe_volume, linear)) rc = -EIO;
    }
    pa_threaded_mainloop_unlock(be->loop);
    return rc;
}

/** Caller holds the mainloop lock. */
static int set_mute_locked(struct pa_backend *be, uint32_t id, bool mute)
{
    uint32_t index = id_index(id);
    pa_operation *op = NULL;
    switch (id_tag(id)) {
    case TAG_SINK:
        op = pa_context_set_sink_mute_by_index(be->context, index, mute, success_cb, be);
        break;
    case TAG_SOURCE:
        op = pa_context_set_source_mute_by_index(be->context, index, mute, success_cb, be);
        break;
    case TAG_SINK_INPUT:
        op = pa_context_set_sink_input_mute(be->context, index, mute, success_cb, be);
        break;
    case TAG_SOURCE_OUTPUT:
        op = pa_context_set_source_output_mute(be->context, index, mute, success_cb, be);
        break;
    default:
        return -EINVAL;
    }
    be->last_result = 0;
    int rc = wait_op(be, op);
    if (rc == 0) rc = be->last_result;
    if (rc == 0) {
        rc = probe(be, id);
        if (rc == 0 && be->probe_mute != mute) rc = -EIO;
    }
    return rc;
}

static int pulse_set_mute(struct vs_audio *audio, uint32_t id, bool mute)
{
    struct pa_backend *be = audio->impl;
    pa_threaded_mainloop_lock(be->loop);
    int rc = set_mute_locked(be, id, mute);
    pa_threaded_mainloop_unlock(be->loop);
    return rc;
}

static int pulse_route_stream(struct vs_audio *audio, uint32_t stream_id,
                              uint32_t sink_id)
{
    struct pa_backend *be = audio->impl;
    if (id_tag(stream_id) != TAG_SINK_INPUT) return -ENOTSUP;
    /* PulseAudio has no "unpin" for a sink input: a stream is always on some
     * sink. Clearing therefore means "move it to the current default", which
     * is the same visible result as clearing target.object in PipeWire. */
    pa_threaded_mainloop_lock(be->loop);
    uint32_t target = sink_id;
    if (target == 0) {
        int rc = refresh_server_info(be);
        if (rc != 0) { pa_threaded_mainloop_unlock(be->loop); return rc; }
        pa_operation *op = pa_context_move_sink_input_by_name(
            be->context, id_index(stream_id), be->default_sink, success_cb, be);
        be->last_result = 0;
        rc = wait_op(be, op);
        if (rc == 0) rc = be->last_result;
        pa_threaded_mainloop_unlock(be->loop);
        return rc;
    }
    if (id_tag(target) != TAG_SINK) { pa_threaded_mainloop_unlock(be->loop); return -EINVAL; }

    pa_operation *op = pa_context_move_sink_input_by_index(
        be->context, id_index(stream_id), id_index(target), success_cb, be);
    be->last_result = 0;
    int rc = wait_op(be, op);
    if (rc == 0) rc = be->last_result;
    if (rc == 0) {
        /* No link graph to observe, so the read-back is the input's own idea
         * of which sink it is on. */
        rc = probe(be, stream_id);
        if (rc == 0 && be->probe_sink_index != id_index(target)) rc = -EIO;
    }
    pa_threaded_mainloop_unlock(be->loop);
    return rc;
}

/**
 * Waits for the server's idea of the default sink to become `name`.
 *
 * The read-back needs a wait, not a single query: pa_context_set_default_sink
 * completes as soon as the server has accepted the change, and where the
 * PulseAudio API is served by pipewire-pulse the change then has to travel
 * through to WirePlumber's metadata and back. Measured here: one immediate
 * pa_context_get_server_info after a successful set still reported the old
 * sink, which made a write that had in fact worked look like an -EIO.
 * Caller holds the mainloop lock.
 */
static int wait_for_default(struct pa_backend *be, const char *name,
                            unsigned timeout_ms)
{
    for (unsigned waited = 0;; waited += 50) {
        int rc = refresh_server_info(be);
        if (rc != 0) return rc;
        if (strcmp(be->default_sink, name) == 0) return 0;
        if (waited >= timeout_ms) return -EIO;
        /* The lock has to go while sleeping, or the mainloop cannot make the
         * progress this is waiting for. */
        pa_threaded_mainloop_unlock(be->loop);
        struct timespec nap = { .tv_sec = 0, .tv_nsec = 50L * 1000000L };
        nanosleep(&nap, NULL);
        pa_threaded_mainloop_lock(be->loop);
    }
}

static int pulse_set_default_sink(struct vs_audio *audio, uint32_t sink_id)
{
    struct pa_backend *be = audio->impl;
    if (id_tag(sink_id) != TAG_SINK) return -EINVAL;

    pa_threaded_mainloop_lock(be->loop);
    /* set_default_sink takes a name, so the index is resolved first -- and the
     * probe doubles as the check that the sink exists at all. */
    int rc = probe(be, sink_id);
    if (rc != 0 || !be->probe_name[0]) {
        pa_threaded_mainloop_unlock(be->loop);
        return rc != 0 ? rc : -ENOENT;
    }
    char name[sizeof(be->probe_name)];
    snprintf(name, sizeof(name), "%s", be->probe_name);

    be->last_result = 0;
    rc = wait_op(be, pa_context_set_default_sink(be->context, name, success_cb, be));
    if (rc == 0) rc = be->last_result;
    if (rc == 0) rc = wait_for_default(be, name, 3000);
    pa_threaded_mainloop_unlock(be->loop);
    return rc;
}

/* --- open / close --------------------------------------------------------- */

static void pulse_close(struct vs_audio *audio);

static int pulse_open(struct vs_audio *audio)
{
    struct pa_backend *be = calloc(1, sizeof(*be));
    if (!be) return -ENOMEM;
    be->audio = audio;
    audio->impl = be;

    be->loop = pa_threaded_mainloop_new();
    if (!be->loop) { pulse_close(audio); return -ENOMEM; }

    be->context = pa_context_new(pa_threaded_mainloop_get_api(be->loop),
                                 "Vorssaint");
    if (!be->context) { pulse_close(audio); return -ENOMEM; }

    pa_context_set_state_callback(be->context, context_state_cb, be);
    if (pa_context_connect(be->context, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        pulse_close(audio);
        return -ENOENT;
    }
    if (pa_threaded_mainloop_start(be->loop) < 0) { pulse_close(audio); return -EIO; }

    pa_threaded_mainloop_lock(be->loop);
    for (;;) {
        pa_context_state_t state = pa_context_get_state(be->context);
        if (state == PA_CONTEXT_READY) break;
        if (!PA_CONTEXT_IS_GOOD(state)) {
            pa_threaded_mainloop_unlock(be->loop);
            pulse_close(audio);
            return -ENOENT;
        }
        pa_threaded_mainloop_wait(be->loop);
    }

    pa_context_set_subscribe_callback(be->context, subscribe_cb, be);
    be->debounce = pa_context_rttime_new(be->context, PA_USEC_INVALID,
                                         on_debounce, be);
    int rc = wait_op(be, pa_context_subscribe(
        be->context,
        PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE
            | PA_SUBSCRIPTION_MASK_SINK_INPUT
            | PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT | PA_SUBSCRIPTION_MASK_SERVER,
        success_cb, be));
    if (rc == 0) rc = refresh_server_info(be);
    pa_threaded_mainloop_unlock(be->loop);
    if (rc != 0) { pulse_close(audio); return rc; }

    audio->caps = VS_AUDIO_CAP_HAS_PULSE_FALLBACK
                | VS_AUDIO_CAP_CAN_ROUTE_PER_STREAM
                | VS_AUDIO_CAP_CAN_BOOST_OVER_100
                | VS_AUDIO_CAP_HAS_EVENTS;
    return 0;
}

static void pulse_close(struct vs_audio *audio)
{
    struct pa_backend *be = audio->impl;
    if (!be) return;
    if (be->loop) pa_threaded_mainloop_stop(be->loop);
    if (be->context) {
        pa_context_disconnect(be->context);
        pa_context_unref(be->context);
    }
    if (be->loop) pa_threaded_mainloop_free(be->loop);
    free(be);
    audio->impl = NULL;
}

const struct vs_audio_backend vs_audio_backend_pulse = {
    .name = "libpulse",
    .open = pulse_open,
    .close = pulse_close,
    .list_nodes = pulse_list_nodes,
    .set_volume = pulse_set_volume,
    .set_mute = pulse_set_mute,
    .route_stream = pulse_route_stream,
    .set_default_sink = pulse_set_default_sink,
};
