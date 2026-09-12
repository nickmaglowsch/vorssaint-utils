/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * The libpulse fallback.
 *
 * Why it exists: PipeWire is the assumption, not the requirement. A host still
 * running PulseAudio has the same sink-input API under a different name, and
 * the mixer's core -- list the devices, list what is playing, set a per-app
 * volume, switch the default output -- maps onto it directly. Losing per-app
 * volume entirely on those machines would be a worse answer than a backend
 * with fewer capability bits, which is what the bits are for.
 *
 * Shape: a plain `pa_mainloop`, never `pa_threaded_mainloop`, because this
 * layer starts no threads. The caller drives it through `dispatch`, and the
 * blocking verbs turn it themselves until the server has answered.
 *
 * What it cannot do, and says so: there is no graph, so a routing request
 * cannot be verified by watching links move. It is verified by re-reading
 * which sink the input now sits on, which is the strongest read-back this
 * protocol offers, and `effective_id` is therefore always equal to
 * `target_id` here rather than independently observed.
 */

#include "vs_audio_internal.h"

#include <errno.h>
#include <stdio.h>

#include <pulse/pulseaudio.h>

/*
 * PulseAudio numbers sinks, sources, sink-inputs and source-outputs in four
 * separate index spaces, so an index alone does not identify an object the way
 * PipeWire's `object.serial` does. The public id carries the kind in its top
 * byte. Indices are small sequence numbers in practice; one that did not fit
 * would be reported with an id of 0 rather than aliased onto another object.
 */
#define PA_TAG_SHIFT 24
#define PA_INDEX_MAX ((1u << PA_TAG_SHIFT) - 1u)

enum { TAG_SINK = 1, TAG_SOURCE = 2, TAG_SINK_INPUT = 3, TAG_SOURCE_OUTPUT = 4 };

#define WRITE_BUDGET_MS 2000
#define DEFAULT_BUDGET_MS 3000
#define CONNECT_BUDGET_MS 3000

static vs_audio_id pack_id(unsigned tag, uint32_t index)
{
    if (index > PA_INDEX_MAX) return 0;
    return ((vs_audio_id)tag << PA_TAG_SHIFT) | index;
}
static unsigned id_tag(vs_audio_id id) { return (id >> PA_TAG_SHIFT) & 0xffu; }
static uint32_t id_index(vs_audio_id id) { return id & PA_INDEX_MAX; }

struct pa_backend {
    vs_audio_system system;
    struct vs_audio_events events;

    pa_mainloop *loop;
    pa_mainloop_api *api;
    pa_context *context;

    char default_sink[VS_AUDIO_NAME_MAX];
    char default_source[VS_AUDIO_NAME_MAX];

    /* The listing a query is collecting into. */
    vs_audio_node **collect_nodes;
    size_t *collect_count;
    size_t collect_capacity;
    uint32_t collect_mask;
    bool collect_failed;

    /* Per-operation state. `pending` counts operations still running, so the
     * pump knows when to stop; `succeeded` carries a write's server answer. */
    int pending;
    bool succeeded;
    bool op_failed;

    /* Single-object probe, for the read-backs. */
    float probe_volume;
    bool probe_mute;
    uint32_t probe_sink_index;
    char probe_name[VS_AUDIO_NAME_MAX];
    uint8_t probe_channels;
    bool probe_found;

    pa_time_event *debounce;
    bool debounce_armed;
};

/* --- volume scale ----------------------------------------------------------
 *
 * `pa_volume_t` is not linear: PA_VOLUME_NORM is unity and the scale is cubic,
 * the same curve wpctl uses and the one this API deliberately does not.
 * pa_sw_volume_from_linear / _to_linear are the library's own conversions, so
 * the linear amplitude promised here survives the trip in both directions.
 */
static pa_volume_t to_pa(float linear) { return pa_sw_volume_from_linear((double)linear); }
static float from_pa(pa_volume_t v) { return (float)pa_sw_volume_to_linear(v); }

/* --- pumping the mainloop -------------------------------------------------- */

/** One turn of the loop, waiting at most `timeout_ms`. */
static int pump_once(struct pa_backend *be, int timeout_ms)
{
    if (pa_mainloop_prepare(be->loop, timeout_ms * 1000) < 0) return VS_ERR_BACKEND;
    if (pa_mainloop_poll(be->loop) < 0) return VS_ERR_BACKEND;
    if (pa_mainloop_dispatch(be->loop) < 0) return VS_ERR_BACKEND;
    return VS_OK;
}

/** Turns the loop until every outstanding operation has answered. */
static int pump_pending(struct pa_backend *be, unsigned budget_ms)
{
    uint64_t deadline = vs_audio_now_ms() + budget_ms;
    while (be->pending > 0) {
        if (pa_context_get_state(be->context) != PA_CONTEXT_READY) return VS_ERR_BACKEND;
        uint64_t now = vs_audio_now_ms();
        if (now >= deadline) return VS_ERR_TIMEOUT;
        int slice = (int)(deadline - now);
        if (slice > 20) slice = 20;
        int rc = pump_once(be, slice);
        if (rc != VS_OK) return rc;
    }
    return VS_OK;
}

/** Registers an operation and runs the loop until it completes. */
static int run_op(struct pa_backend *be, pa_operation *op, unsigned budget_ms)
{
    if (!op) return VS_ERR_BACKEND;
    be->pending = 1;
    int rc = pump_pending(be, budget_ms);
    if (rc != VS_OK) pa_operation_cancel(op);
    pa_operation_unref(op);
    return rc;
}

static void finish_op(struct pa_backend *be) { if (be->pending > 0) be->pending--; }

static void success_cb(pa_context *c, int success, void *userdata)
{
    (void)c;
    struct pa_backend *be = userdata;
    be->succeeded = success != 0;
    finish_op(be);
}

/* --- events ---------------------------------------------------------------- */

static void on_debounce(pa_mainloop_api *api, pa_time_event *e,
                        const struct timeval *tv, void *userdata)
{
    (void)api; (void)e; (void)tv;
    struct pa_backend *be = userdata;
    be->debounce_armed = false;
    vs_audio_events_push(&be->events, VS_AUDIO_EVENT_CHANGED, 0, NULL);
}

static void schedule_changed(struct pa_backend *be)
{
    if (be->debounce_armed || !be->debounce) return;
    be->debounce_armed = true;
    pa_context_rttime_restart(be->context, be->debounce,
                              pa_rtclock_now() + be->events.debounce_ms * 1000ull);
}

static void subscribe_cb(pa_context *c, pa_subscription_event_type_t type,
                         uint32_t index, void *userdata)
{
    (void)c; (void)type; (void)index;
    /* The event says which facility changed but not what to, and every reader
     * of this backend re-queries anyway, so one debounced CHANGED is the whole
     * reaction. A default-device move arrives as a SERVER event and `list`
     * picks it up when it refreshes the server info. */
    schedule_changed(userdata);
}

/* --- listing --------------------------------------------------------------- */

static vs_audio_node *collect_add(struct pa_backend *be)
{
    if (be->collect_failed) return NULL;
    if (!vs_audio_nodes_reserve(be->collect_nodes, *be->collect_count,
                                &be->collect_capacity)) {
        be->collect_failed = true;
        return NULL;
    }
    vs_audio_node *out = &(*be->collect_nodes)[(*be->collect_count)++];
    vs_audio_node_init(out);
    return out;
}

static void fill_stream_props(vs_audio_node *out, pa_proplist *props)
{
    if (!props) return;
    vs_audio_copy_field(out->app_name, sizeof(out->app_name),
                        pa_proplist_gets(props, PA_PROP_APPLICATION_NAME));
    vs_audio_copy_field(out->icon_name, sizeof(out->icon_name),
                        pa_proplist_gets(props, PA_PROP_APPLICATION_ICON_NAME));
    vs_audio_copy_field(out->media_name, sizeof(out->media_name),
                        pa_proplist_gets(props, PA_PROP_MEDIA_NAME));
    /* Same three-step fallback as the PipeWire backend, so a volume saved on
     * one host is found again on the other. */
    const char *app_id = pa_proplist_gets(props, PA_PROP_APPLICATION_ID);
    if (!app_id) app_id = pa_proplist_gets(props, PA_PROP_APPLICATION_PROCESS_BINARY);
    if (!app_id) app_id = pa_proplist_gets(props, PA_PROP_APPLICATION_NAME);
    vs_audio_copy_field(out->app_id, sizeof(out->app_id), app_id);
    const char *pid = pa_proplist_gets(props, PA_PROP_APPLICATION_PROCESS_ID);
    if (pid) {
        out->pid = (int32_t)strtol(pid, NULL, 10);
        if (out->pid >= 0) out->flags |= VS_AUDIO_NODE_HAS_PID;
    }
}

static void fill_common(vs_audio_node *out, vs_audio_id id, vs_audio_node_kind kind,
                        const char *name, const char *description,
                        pa_volume_t volume, int mute)
{
    out->id = id;
    out->kind = kind;
    out->volume = from_pa(volume);
    out->flags |= VS_AUDIO_NODE_HAS_VOLUME;
    if (mute) out->flags |= VS_AUDIO_NODE_MUTED;
    vs_audio_copy_field(out->name, sizeof(out->name), name);
    vs_audio_copy_field(out->description, sizeof(out->description), description);
}

/**
 * The bus a device sits on. PulseAudio puts `device.bus` in the proplist
 * directly, so unlike the PipeWire side there is no Device object to chase --
 * only the same normalisation, so both backends hand the UI the same words.
 */
static void fill_transport(vs_audio_node *out, pa_proplist *props)
{
    if (!props) return;
    const char *bus = pa_proplist_gets(props, PA_PROP_DEVICE_BUS);
    if (bus && *bus) {
        if (strcmp(bus, "pci") == 0 || strcmp(bus, "isa") == 0)
            vs_audio_copy_field(out->transport, sizeof(out->transport), "builtin");
        else
            vs_audio_copy_field(out->transport, sizeof(out->transport), bus);
        return;
    }
    const char *api = pa_proplist_gets(props, PA_PROP_DEVICE_API);
    if (api && strcmp(api, "bluez") == 0)
        vs_audio_copy_field(out->transport, sizeof(out->transport), "bluetooth");
}

static void sink_info_cb(pa_context *c, const pa_sink_info *i, int eol, void *ud)
{
    (void)c;
    struct pa_backend *be = ud;
    if (eol) { finish_op(be); return; }
    if (!be->collect_nodes || !(be->collect_mask & VS_AUDIO_NODE_SINK)) return;
    vs_audio_node *out = collect_add(be);
    if (!out) return;
    fill_common(out, pack_id(TAG_SINK, i->index), VS_AUDIO_NODE_SINK,
                i->name, i->description, pa_cvolume_max(&i->volume), i->mute);
    if (strcmp(out->name, be->default_sink) == 0)
        out->flags |= VS_AUDIO_NODE_IS_DEFAULT;
    fill_transport(out, i->proplist);
    if (i->state == PA_SINK_RUNNING) out->flags |= VS_AUDIO_NODE_ACTIVE;
}

static void source_info_cb(pa_context *c, const pa_source_info *i, int eol, void *ud)
{
    (void)c;
    struct pa_backend *be = ud;
    if (eol) { finish_op(be); return; }
    if (!be->collect_nodes || !(be->collect_mask & VS_AUDIO_NODE_SOURCE)) return;
    /* A sink's monitor is listed by PulseAudio as an ordinary source. Muting
     * every input must not mute monitors -- that is system-audio capture, not a
     * microphone -- so they are left out of the listing entirely, which is also
     * what the macOS input list shows. */
    if (i->monitor_of_sink != PA_INVALID_INDEX) return;
    vs_audio_node *out = collect_add(be);
    if (!out) return;
    fill_common(out, pack_id(TAG_SOURCE, i->index), VS_AUDIO_NODE_SOURCE,
                i->name, i->description, pa_cvolume_max(&i->volume), i->mute);
    if (strcmp(out->name, be->default_source) == 0)
        out->flags |= VS_AUDIO_NODE_IS_DEFAULT;
    fill_transport(out, i->proplist);
    if (i->state == PA_SOURCE_RUNNING) out->flags |= VS_AUDIO_NODE_ACTIVE;
}

static void sink_input_cb(pa_context *c, const pa_sink_input_info *i, int eol, void *ud)
{
    (void)c;
    struct pa_backend *be = ud;
    if (eol) { finish_op(be); return; }
    if (!be->collect_nodes || !(be->collect_mask & VS_AUDIO_NODE_STREAM_OUTPUT)) return;
    vs_audio_node *out = collect_add(be);
    if (!out) return;
    fill_common(out, pack_id(TAG_SINK_INPUT, i->index), VS_AUDIO_NODE_STREAM_OUTPUT,
                i->name, i->name, pa_cvolume_max(&i->volume), i->mute);
    /* PulseAudio does not separate "asked for" from "arrived at": a sink input
     * is simply on a sink. Both fields carry that one fact rather than
     * pretending to an independent observation. */
    out->target_id = pack_id(TAG_SINK, i->sink);
    out->effective_id = out->target_id;
    /* `corked` is PulseAudio's word for a stream the application paused; not
     * corked is the closest this protocol comes to PipeWire's Running state. */
    if (!i->corked) out->flags |= VS_AUDIO_NODE_ACTIVE;
    fill_stream_props(out, i->proplist);
}

static void source_output_cb(pa_context *c, const pa_source_output_info *i, int eol,
                             void *ud)
{
    (void)c;
    struct pa_backend *be = ud;
    if (eol) { finish_op(be); return; }
    if (!be->collect_nodes || !(be->collect_mask & VS_AUDIO_NODE_STREAM_INPUT)) return;
    vs_audio_node *out = collect_add(be);
    if (!out) return;
    fill_common(out, pack_id(TAG_SOURCE_OUTPUT, i->index), VS_AUDIO_NODE_STREAM_INPUT,
                i->name, i->name, pa_cvolume_max(&i->volume), i->mute);
    out->target_id = pack_id(TAG_SOURCE, i->source);
    out->effective_id = out->target_id;
    if (!i->corked) out->flags |= VS_AUDIO_NODE_ACTIVE;
    fill_stream_props(out, i->proplist);
}

static void server_info_cb(pa_context *c, const pa_server_info *i, void *ud)
{
    (void)c;
    struct pa_backend *be = ud;
    if (i) {
        vs_audio_copy_field(be->default_sink, sizeof(be->default_sink),
                            i->default_sink_name);
        vs_audio_copy_field(be->default_source, sizeof(be->default_source),
                            i->default_source_name);
    }
    finish_op(be);
}

static int refresh_server_info(struct pa_backend *be)
{
    return run_op(be, pa_context_get_server_info(be->context, server_info_cb, be),
                  WRITE_BUDGET_MS);
}

static int pulse_list(vs_audio_system *self, uint32_t kind_mask,
                      vs_audio_node **nodes_out, size_t *count_out)
{
    struct pa_backend *be = self->impl;
    *nodes_out = NULL;
    *count_out = 0;
    if ((kind_mask & VS_AUDIO_NODE_ANY) == 0) return VS_ERR_INVALID;

    int rc = refresh_server_info(be);
    if (rc != VS_OK) return rc;

    be->collect_nodes = nodes_out;
    be->collect_count = count_out;
    be->collect_capacity = 0;
    be->collect_mask = kind_mask;
    be->collect_failed = false;

    if (rc == VS_OK && (kind_mask & VS_AUDIO_NODE_SINK))
        rc = run_op(be, pa_context_get_sink_info_list(be->context, sink_info_cb, be),
                    WRITE_BUDGET_MS);
    if (rc == VS_OK && (kind_mask & VS_AUDIO_NODE_SOURCE))
        rc = run_op(be, pa_context_get_source_info_list(be->context, source_info_cb, be),
                    WRITE_BUDGET_MS);
    if (rc == VS_OK && (kind_mask & VS_AUDIO_NODE_STREAM_OUTPUT))
        rc = run_op(be, pa_context_get_sink_input_info_list(be->context, sink_input_cb, be),
                    WRITE_BUDGET_MS);
    if (rc == VS_OK && (kind_mask & VS_AUDIO_NODE_STREAM_INPUT))
        rc = run_op(be, pa_context_get_source_output_info_list(be->context,
                                                               source_output_cb, be),
                    WRITE_BUDGET_MS);

    bool failed = be->collect_failed;
    be->collect_nodes = NULL;
    be->collect_count = NULL;

    if (rc != VS_OK || failed) {
        free(*nodes_out);
        *nodes_out = NULL;
        *count_out = 0;
        return failed ? VS_ERR_NO_MEM : rc;
    }
    return VS_OK;
}

static void pulse_free_list(vs_audio_system *self, vs_audio_node *nodes, size_t count)
{
    (void)self; (void)count;
    free(nodes);
}

/* --- single-object probes, for the read-backs ------------------------------ */

static void probe_take(struct pa_backend *be, const pa_cvolume *volume, int mute,
                       const char *name)
{
    be->probe_found = true;
    be->probe_volume = from_pa(pa_cvolume_max(volume));
    be->probe_mute = mute != 0;
    be->probe_channels = volume->channels;
    vs_audio_copy_field(be->probe_name, sizeof(be->probe_name), name);
}

static void probe_sink_cb(pa_context *c, const pa_sink_info *i, int eol, void *ud)
{
    (void)c;
    struct pa_backend *be = ud;
    if (eol) { finish_op(be); return; }
    probe_take(be, &i->volume, i->mute, i->name);
}

static void probe_source_cb(pa_context *c, const pa_source_info *i, int eol, void *ud)
{
    (void)c;
    struct pa_backend *be = ud;
    if (eol) { finish_op(be); return; }
    probe_take(be, &i->volume, i->mute, i->name);
}

static void probe_sink_input_cb(pa_context *c, const pa_sink_input_info *i, int eol,
                                void *ud)
{
    (void)c;
    struct pa_backend *be = ud;
    if (eol) { finish_op(be); return; }
    probe_take(be, &i->volume, i->mute, i->name);
    be->probe_sink_index = i->sink;
}

static void probe_source_output_cb(pa_context *c, const pa_source_output_info *i,
                                   int eol, void *ud)
{
    (void)c;
    struct pa_backend *be = ud;
    if (eol) { finish_op(be); return; }
    probe_take(be, &i->volume, i->mute, i->name);
}

/** Re-reads one object. */
static int probe(struct pa_backend *be, vs_audio_id id)
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
        return VS_ERR_INVALID;
    }
    int rc = run_op(be, op, WRITE_BUDGET_MS);
    if (rc != VS_OK) return rc;
    return be->probe_found ? VS_OK : VS_ERR_NOT_FOUND;
}

/* --- writes ---------------------------------------------------------------- */

static int pulse_set_volume(vs_audio_system *self, vs_audio_id id, float linear)
{
    struct pa_backend *be = self->impl;
    if (id == 0) return VS_ERR_INVALID;
    unsigned tag = id_tag(id);
    if (tag < TAG_SINK || tag > TAG_SOURCE_OUTPUT) return VS_ERR_INVALID;

    /* The channel count has to come from the object: writing a stereo cvolume
     * onto a mono source sets a channel that is not there, and PulseAudio
     * answers with a failure rather than a guess. */
    int rc = probe(be, id);
    if (rc != VS_OK) return rc;

    float wanted = vs_audio_clamp_volume(linear);
    pa_cvolume cv;
    pa_cvolume_set(&cv, be->probe_channels ? be->probe_channels : 2u, to_pa(wanted));

    uint32_t index = id_index(id);
    pa_operation *op = NULL;
    switch (tag) {
    case TAG_SINK:
        op = pa_context_set_sink_volume_by_index(be->context, index, &cv, success_cb, be);
        break;
    case TAG_SOURCE:
        op = pa_context_set_source_volume_by_index(be->context, index, &cv, success_cb, be);
        break;
    case TAG_SINK_INPUT:
        op = pa_context_set_sink_input_volume(be->context, index, &cv, success_cb, be);
        break;
    default:
        op = pa_context_set_source_output_volume(be->context, index, &cv, success_cb, be);
        break;
    }
    be->succeeded = false;
    rc = run_op(be, op, WRITE_BUDGET_MS);
    if (rc != VS_OK) return rc;
    if (!be->succeeded) return VS_ERR_BACKEND;

    rc = probe(be, id);
    if (rc != VS_OK) return rc;
    return vs_audio_volume_equal(be->probe_volume, wanted) ? VS_OK : VS_ERR_NOT_APPLIED;
}

static int pulse_set_mute(vs_audio_system *self, vs_audio_id id, bool mute)
{
    struct pa_backend *be = self->impl;
    if (id == 0) return VS_ERR_INVALID;
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
        return VS_ERR_INVALID;
    }
    be->succeeded = false;
    int rc = run_op(be, op, WRITE_BUDGET_MS);
    if (rc != VS_OK) return rc;
    if (!be->succeeded) return VS_ERR_BACKEND;

    rc = probe(be, id);
    if (rc != VS_OK) return rc;
    return be->probe_mute == mute ? VS_OK : VS_ERR_NOT_APPLIED;
}

static int pulse_route_stream(vs_audio_system *self, vs_audio_id stream_id,
                              vs_audio_id sink_id)
{
    struct pa_backend *be = self->impl;
    if (stream_id == 0) return VS_ERR_INVALID;
    if (id_tag(stream_id) != TAG_SINK_INPUT) return VS_ERR_UNSUPPORTED;

    vs_audio_id target = sink_id;
    if (target == 0) {
        /* PulseAudio has no "unpin": a sink input is always on some sink.
         * Clearing therefore means moving it to the current default, which is
         * the same visible result as clearing `target.object` in PipeWire. */
        int rc = refresh_server_info(be);
        if (rc != VS_OK) return rc;
        if (!be->default_sink[0]) return VS_ERR_NOT_FOUND;
        be->succeeded = false;
        rc = run_op(be, pa_context_move_sink_input_by_name(be->context,
                                                           id_index(stream_id),
                                                           be->default_sink,
                                                           success_cb, be),
                    DEFAULT_BUDGET_MS);
        if (rc != VS_OK) return rc;
        return be->succeeded ? VS_OK : VS_ERR_BACKEND;
    }
    if (id_tag(target) != TAG_SINK) return VS_ERR_INVALID;

    be->succeeded = false;
    int rc = run_op(be, pa_context_move_sink_input_by_index(be->context,
                                                            id_index(stream_id),
                                                            id_index(target),
                                                            success_cb, be),
                    DEFAULT_BUDGET_MS);
    if (rc != VS_OK) return rc;
    if (!be->succeeded) return VS_ERR_BACKEND;

    rc = probe(be, stream_id);
    if (rc != VS_OK) return rc;
    return be->probe_sink_index == id_index(target) ? VS_OK : VS_ERR_NOT_APPLIED;
}

/**
 * Waits for the server's idea of the default sink to become `name`.
 *
 * The read-back needs a wait, not a single query. `pa_context_set_default_sink`
 * completes as soon as the server has accepted the change, and where this API
 * is served by pipewire-pulse the change then has to travel on to WirePlumber's
 * metadata and back. Measured here: one immediate `pa_context_get_server_info`
 * after a successful set still reported the old sink, which made a write that
 * had in fact worked look like a failure.
 */
static int wait_for_default(struct pa_backend *be, const char *name, unsigned budget_ms)
{
    uint64_t deadline = vs_audio_now_ms() + budget_ms;
    for (;;) {
        int rc = refresh_server_info(be);
        if (rc != VS_OK) return rc;
        if (strcmp(be->default_sink, name) == 0) return VS_OK;
        if (vs_audio_now_ms() >= deadline) return VS_ERR_NOT_APPLIED;
        rc = pump_once(be, 50);
        if (rc != VS_OK) return rc;
    }
}

static int pulse_set_default_sink(vs_audio_system *self, vs_audio_id sink_id)
{
    struct pa_backend *be = self->impl;
    if (sink_id == 0) return VS_ERR_INVALID;
    if (id_tag(sink_id) != TAG_SINK) return VS_ERR_INVALID;

    /* set_default_sink takes a name, so the index is resolved first -- and the
     * probe doubles as the check that the sink exists at all. */
    int rc = probe(be, sink_id);
    if (rc != VS_OK) return rc;
    if (!be->probe_name[0]) return VS_ERR_NOT_FOUND;

    char name[VS_AUDIO_NAME_MAX];
    vs_audio_copy_field(name, sizeof(name), be->probe_name);

    be->succeeded = false;
    rc = run_op(be, pa_context_set_default_sink(be->context, name, success_cb, be),
                WRITE_BUDGET_MS);
    if (rc != VS_OK) return rc;
    if (!be->succeeded) return VS_ERR_BACKEND;
    return wait_for_default(be, name, DEFAULT_BUDGET_MS);
}

/** The source counterpart of wait_for_default, and for the same reason. */
static int wait_for_default_source(struct pa_backend *be, const char *name,
                                   unsigned budget_ms)
{
    uint64_t deadline = vs_audio_now_ms() + budget_ms;
    for (;;) {
        int rc = refresh_server_info(be);
        if (rc != VS_OK) return rc;
        if (strcmp(be->default_source, name) == 0) return VS_OK;
        if (vs_audio_now_ms() >= deadline) return VS_ERR_NOT_APPLIED;
        rc = pump_once(be, 50);
        if (rc != VS_OK) return rc;
    }
}

static int pulse_set_default_source(vs_audio_system *self, vs_audio_id source_id)
{
    struct pa_backend *be = self->impl;
    if (source_id == 0) return VS_ERR_INVALID;
    if (id_tag(source_id) != TAG_SOURCE) return VS_ERR_INVALID;

    int rc = probe(be, source_id);
    if (rc != VS_OK) return rc;
    if (!be->probe_name[0]) return VS_ERR_NOT_FOUND;

    char name[VS_AUDIO_NAME_MAX];
    vs_audio_copy_field(name, sizeof(name), be->probe_name);

    be->succeeded = false;
    rc = run_op(be, pa_context_set_default_source(be->context, name, success_cb, be),
                WRITE_BUDGET_MS);
    if (rc != VS_OK) return rc;
    if (!be->succeeded) return VS_ERR_BACKEND;
    return wait_for_default_source(be, name, DEFAULT_BUDGET_MS);
}

static int pulse_mute_all_inputs(vs_audio_system *self, bool mute, size_t *changed_out)
{
    return vs_audio_mute_all_inputs_generic(self, mute, changed_out);
}

/* --- events ---------------------------------------------------------------- */

static int pulse_set_event_callback(vs_audio_system *self, vs_audio_event_cb callback,
                                    void *user_data)
{
    struct pa_backend *be = self->impl;
    be->events.callback = callback;
    be->events.user_data = user_data;
    return VS_OK;
}

static int pulse_event_fd(vs_audio_system *self)
{
    (void)self;
    /* `pa_mainloop` polls a set of descriptors it does not expose, and there is
     * no single fd that stands for "something happened". Rather than invent one
     * -- a timerfd would be readable on a schedule, not on an event, which is
     * not what this member promises -- the backend says it has none and the
     * caller calls `dispatch` on a timer. VS_AUDIO_HAS_EVENTS is still set:
     * events do arrive, there is just nothing to wait on. */
    return -1;
}

static int pulse_dispatch(vs_audio_system *self)
{
    struct pa_backend *be = self->impl;
    if (pa_context_get_state(be->context) != PA_CONTEXT_READY) return VS_ERR_BACKEND;
    int rc = pump_once(be, 0);
    if (rc != VS_OK) return rc;
    return vs_audio_events_drain(&be->events);
}

/* --- lifecycle -------------------------------------------------------------- */

static void pulse_destroy(vs_audio_system *self)
{
    struct pa_backend *be = self->impl;
    if (!be) return;
    if (be->debounce && be->api) be->api->time_free(be->debounce);
    if (be->context) {
        pa_context_disconnect(be->context);
        pa_context_unref(be->context);
    }
    if (be->loop) pa_mainloop_free(be->loop);
    vs_audio_events_dispose(&be->events);
    free(be);
}

vs_audio_system *vs_audio_pulse_create(int *result_out)
{
    struct pa_backend *be = calloc(1, sizeof(*be));
    if (!be) { *result_out = VS_ERR_NO_MEM; return NULL; }
    vs_audio_events_init(&be->events, 80);

    be->system = (vs_audio_system){
        .name = "libpulse",
        .impl = be,
        .list = pulse_list,
        .free_list = pulse_free_list,
        .set_volume = pulse_set_volume,
        .set_mute = pulse_set_mute,
        .route_stream = pulse_route_stream,
        .set_default_sink = pulse_set_default_sink,
        .set_default_source = pulse_set_default_source,
        .mute_all_inputs = pulse_mute_all_inputs,
        .set_event_callback = pulse_set_event_callback,
        .event_fd = pulse_event_fd,
        .dispatch = pulse_dispatch,
        .destroy = pulse_destroy,
    };

    int result = VS_ERR_NO_BACKEND;
    be->loop = pa_mainloop_new();
    if (!be->loop) { result = VS_ERR_NO_MEM; goto fail; }
    be->api = pa_mainloop_get_api(be->loop);

    be->context = pa_context_new(be->api, "Vorssaint");
    if (!be->context) { result = VS_ERR_NO_MEM; goto fail; }

    if (pa_context_connect(be->context, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        result = VS_ERR_NO_BACKEND;
        goto fail;
    }

    uint64_t deadline = vs_audio_now_ms() + CONNECT_BUDGET_MS;
    for (;;) {
        pa_context_state_t state = pa_context_get_state(be->context);
        if (state == PA_CONTEXT_READY) break;
        if (!PA_CONTEXT_IS_GOOD(state)) { result = VS_ERR_NO_BACKEND; goto fail; }
        if (vs_audio_now_ms() >= deadline) { result = VS_ERR_TIMEOUT; goto fail; }
        if (pump_once(be, 20) != VS_OK) { result = VS_ERR_BACKEND; goto fail; }
    }

    pa_context_set_subscribe_callback(be->context, subscribe_cb, be);
    be->debounce = pa_context_rttime_new(be->context, PA_USEC_INVALID, on_debounce, be);

    be->succeeded = false;
    int rc = run_op(be, pa_context_subscribe(
                            be->context,
                            PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE
                                | PA_SUBSCRIPTION_MASK_SINK_INPUT
                                | PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT
                                | PA_SUBSCRIPTION_MASK_SERVER,
                            success_cb, be),
                    CONNECT_BUDGET_MS);
    if (rc == VS_OK) rc = refresh_server_info(be);
    if (rc != VS_OK) { result = rc; goto fail; }

    be->system.capabilities = VS_AUDIO_HAS_PULSE_FALLBACK
                            | VS_AUDIO_CAN_ROUTE_PER_STREAM
                            | VS_AUDIO_CAN_BOOST_OVER_100
                            | VS_AUDIO_HAS_EVENTS;
    *result_out = VS_OK;
    return &be->system;

fail:
    pulse_destroy(&be->system);
    *result_out = result;
    return NULL;
}
