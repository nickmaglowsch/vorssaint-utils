/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * The PipeWire backend.
 *
 * Shape: one pw_thread_loop owns every proxy and the whole model; the public
 * functions run on the caller's thread and take the loop lock around anything
 * they touch. Nothing in the model is read or written without that lock.
 *
 * What is mirrored locally, and why each one is needed:
 *
 *   nodes      every Audio/Sink, Audio/Source and Stream/Output|Input/Audio global,
 *              bound
 *              so its Props (channelVolumes, mute) can be read and written.
 *   links      only the ids: `link.output.node`/`link.input.node` arrive in
 *              the registry global props, so a routing request can be checked
 *              by watching where a stream's links actually land rather than by
 *              trusting that the metadata write "worked".
 *   metadata   the `default` object: it carries the default sink and source,
 *              and it is also where a per-stream `target.object` pin is
 *              written. One object, two jobs.
 */

#include "vs_audio_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/raw.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/parser.h>
#include <spa/utils/json.h>

#define MAX_CHANNELS SPA_AUDIO_MAX_CHANNELS

struct pw_backend;

struct node_entry {
    struct spa_list link;
    struct pw_backend *be;

    uint32_t global_id;
    uint32_t serial;
    vs_audio_node_kind kind;

    char *name, *description, *app_name, *icon_name, *media_name;
    int32_t pid;

    float channel_volumes[MAX_CHANNELS];
    uint32_t n_channels;
    bool mute;
    bool has_volume;

    struct pw_proxy *proxy;
    struct spa_hook proxy_listener;
    struct spa_hook object_listener;
};

struct link_entry {
    struct spa_list link;
    uint32_t global_id;
    uint32_t out_node;
    uint32_t in_node;
};

struct pw_backend {
    struct vs_audio *audio;

    struct pw_thread_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct spa_hook core_listener;
    struct spa_hook registry_listener;

    struct spa_list nodes;
    struct spa_list links;

    struct pw_metadata *metadata;
    struct spa_hook metadata_listener;
    struct pw_proxy *metadata_proxy;
    uint32_t metadata_id;

    char default_sink[256];   /* node.name from default.audio.sink */
    char default_source[256]; /* node.name from default.audio.source */
    /** Per-stream `target.object` pins, keyed by the stream's global id. */
    struct {
        uint32_t subject;
        char value[128];
    } targets[256];
    size_t n_targets;

    struct spa_source *debounce_timer;
    /** Bumped by anything that changes the model, so waiters can spin on it. */
    uint64_t revision;
    int last_seq;
    bool core_error;
};

/* --- small helpers -------------------------------------------------------- */

static void bump(struct pw_backend *be)
{
    be->revision++;
    pw_thread_loop_signal(be->loop, false);
}

static char *dup_or_empty(const char *s)
{
    char *out = strdup(s ? s : "");
    return out;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/**
 * Waits, with the loop lock held, until `pred` is true or the deadline passes.
 * Every model change signals the loop, so this normally wakes on the first
 * change rather than on the one-second timer floor pw_thread_loop_timed_wait
 * imposes.
 */
static int wait_for(struct pw_backend *be, bool (*pred)(struct pw_backend *, void *),
                    void *ctx, unsigned timeout_ms)
{
    uint64_t deadline = now_ms() + timeout_ms;
    while (!pred(be, ctx)) {
        if (be->core_error) return -EIO;
        if (now_ms() >= deadline) return -ETIMEDOUT;
        pw_thread_loop_timed_wait(be->loop, 1);
    }
    return 0;
}

static struct node_entry *find_by_serial(struct pw_backend *be, uint32_t serial)
{
    struct node_entry *n;
    spa_list_for_each(n, &be->nodes, link)
        if (n->serial == serial) return n;
    return NULL;
}

static struct node_entry *find_by_global(struct pw_backend *be, uint32_t global)
{
    struct node_entry *n;
    spa_list_for_each(n, &be->nodes, link)
        if (n->global_id == global) return n;
    return NULL;
}

static struct node_entry *find_by_name(struct pw_backend *be, const char *name)
{
    if (!name || !*name) return NULL;
    struct node_entry *n;
    spa_list_for_each(n, &be->nodes, link)
        if (n->name && strcmp(n->name, name) == 0) return n;
    return NULL;
}

static const char *target_for(struct pw_backend *be, uint32_t subject)
{
    for (size_t i = 0; i < be->n_targets; i++)
        if (be->targets[i].subject == subject) return be->targets[i].value;
    return NULL;
}

static void set_target(struct pw_backend *be, uint32_t subject, const char *value)
{
    for (size_t i = 0; i < be->n_targets; i++) {
        if (be->targets[i].subject != subject) continue;
        if (!value) {
            be->targets[i] = be->targets[--be->n_targets];
        } else {
            snprintf(be->targets[i].value, sizeof(be->targets[i].value), "%s", value);
        }
        return;
    }
    if (!value || be->n_targets == SPA_N_ELEMENTS(be->targets)) return;
    be->targets[be->n_targets].subject = subject;
    snprintf(be->targets[be->n_targets].value,
             sizeof(be->targets[be->n_targets].value), "%s", value);
    be->n_targets++;
}

/**
 * A `target.object` pin is a string that is either the sink's object.serial or
 * its node.name -- WirePlumber accepts both, and something other than this
 * backend may have written it. Resolve either to the serial the API speaks.
 */
static uint32_t resolve_target(struct pw_backend *be, const char *value)
{
    if (!value || !*value) return 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (end && *end == '\0' && parsed > 0) {
        return find_by_serial(be, (uint32_t)parsed) ? (uint32_t)parsed : 0;
    }
    struct node_entry *n = find_by_name(be, value);
    return n ? n->serial : 0;
}

/** Where this stream's links actually land right now, as a global id. */
static uint32_t linked_target_of(struct pw_backend *be, uint32_t stream_global)
{
    struct link_entry *l;
    spa_list_for_each(l, &be->links, link) {
        if (l->out_node == stream_global) return l->in_node;
        if (l->in_node == stream_global) return l->out_node;
    }
    return 0;
}

/* --- debounce ------------------------------------------------------------- */

static void on_debounce(void *data, uint64_t expirations)
{
    (void)expirations;
    struct pw_backend *be = data;
    vs_audio_emit(be->audio, VS_AUDIO_EVENT_CHANGED, 0, "");
}

static void schedule_changed(struct pw_backend *be)
{
    if (!be->debounce_timer) return;
    /* Rearm from now: a device appearing brings a burst of a dozen globals and
     * params, and the UI wants one refresh after the burst, not twelve. */
    struct timespec value = {
        .tv_sec = be->audio->debounce_ms / 1000,
        .tv_nsec = (long)(be->audio->debounce_ms % 1000) * 1000000L
    };
    if (value.tv_sec == 0 && value.tv_nsec == 0) value.tv_nsec = 1;
    pw_loop_update_timer(pw_thread_loop_get_loop(be->loop), be->debounce_timer,
                         &value, NULL, false);
}

/* --- node params ---------------------------------------------------------- */

static void node_param(void *data, int seq, uint32_t id, uint32_t index,
                       uint32_t next, const struct spa_pod *param)
{
    (void)seq; (void)index; (void)next;
    struct node_entry *n = data;
    if (id != SPA_PARAM_Props || !param) return;

    float volumes[MAX_CHANNELS];
    uint32_t n_volumes = 0;
    bool mute = n->mute;
    bool saw_mute = false;

    struct spa_pod_prop *prop;
    SPA_POD_OBJECT_FOREACH((const struct spa_pod_object *)param, prop) {
        switch (prop->key) {
        case SPA_PROP_channelVolumes: {
            uint32_t got = (uint32_t)spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
                                                        volumes, MAX_CHANNELS);
            if (got > 0) n_volumes = got;
            break;
        }
        case SPA_PROP_mute:
            if (spa_pod_get_bool(&prop->value, &mute) == 0) saw_mute = true;
            break;
        default:
            break;
        }
    }

    /* A node answers the Props subscription more than once: the first reply is
     * usually the enumeration with everything at unity, the next carries what
     * is actually set. Both arrive as SPA_PARAM_Props, so take whichever
     * mentions channelVolumes and let the later one win -- which is what it
     * does, since this runs in arrival order on the loop thread. */
    if (n_volumes > 0) {
        memcpy(n->channel_volumes, volumes, n_volumes * sizeof(float));
        n->n_channels = n_volumes;
        n->has_volume = true;
    }
    if (saw_mute) n->mute = mute;
    if (n_volumes > 0 || saw_mute) {
        bump(n->be);
        schedule_changed(n->be);
    }
}

static void replace(char **field, const char *value)
{
    if (!value) return;
    char *copy = strdup(value);
    if (!copy) return;
    free(*field);
    *field = copy;
}

/**
 * The registry's `global` event carries only a filtered subset of a node's
 * properties -- enough to recognise it, not enough to show it. Measured on
 * PipeWire 1.0.5: `node.name`, `application.name` and `application.icon-name`
 * arrive there, but `application.process.id` and `media.name` do not, so a
 * mixer built on the registry dict alone lists every row with no pid and no
 * "now playing" line. The complete dict is on the Node's own info event, which
 * is why every node is bound rather than merely noticed.
 */
static void node_info(void *data, const struct pw_node_info *info)
{
    struct node_entry *n = data;
    if (!info || !info->props) return;

    replace(&n->name, spa_dict_lookup(info->props, PW_KEY_NODE_NAME));
    replace(&n->description, spa_dict_lookup(info->props, PW_KEY_NODE_DESCRIPTION));
    replace(&n->app_name, spa_dict_lookup(info->props, PW_KEY_APP_NAME));
    replace(&n->icon_name, spa_dict_lookup(info->props, PW_KEY_APP_ICON_NAME));
    /* media.name changes as the track changes, which is the point of showing
     * it: the mixer row follows what the app is playing. */
    replace(&n->media_name, spa_dict_lookup(info->props, PW_KEY_MEDIA_NAME));
    const char *pid = spa_dict_lookup(info->props, PW_KEY_APP_PROCESS_ID);
    if (pid) n->pid = (int32_t)strtol(pid, NULL, 10);

    bump(n->be);
    schedule_changed(n->be);
}

static const struct pw_node_events node_events = {
    PW_VERSION_NODE_EVENTS,
    .info = node_info,
    .param = node_param,
};

static void node_proxy_removed(void *data)
{
    struct node_entry *n = data;
    pw_proxy_destroy(n->proxy);
}

static void node_proxy_destroy(void *data)
{
    struct node_entry *n = data;
    n->proxy = NULL;
}

static const struct pw_proxy_events node_proxy_events = {
    PW_VERSION_PROXY_EVENTS,
    .removed = node_proxy_removed,
    .destroy = node_proxy_destroy,
};

/* --- metadata ------------------------------------------------------------- */

/** Pulls "vs-sink-a" out of `{"name":"vs-sink-a"}`, which is how the default
 *  keys are written. A bare string is accepted too. */
static void parse_default_name(const char *value, char *out, size_t out_len)
{
    out[0] = '\0';
    if (!value || !*value) return;

    struct spa_json it[2];
    char key[128];
    spa_json_init(&it[0], value, strlen(value));
    if (spa_json_enter_object(&it[0], &it[1]) <= 0) {
        snprintf(out, out_len, "%s", value);
        return;
    }
    while (spa_json_get_string(&it[1], key, sizeof(key)) > 0) {
        if (strcmp(key, "name") == 0) {
            if (spa_json_get_string(&it[1], out, (int)out_len) > 0) return;
            out[0] = '\0';
            return;
        }
        if (spa_json_next(&it[1], &(const char *){ NULL }) <= 0) break;
    }
}

static int metadata_property(void *data, uint32_t subject, const char *key,
                             const char *type, const char *value)
{
    (void)type;
    struct pw_backend *be = data;

    if (subject == 0 && key && strcmp(key, "default.audio.sink") == 0) {
        char name[sizeof(be->default_sink)];
        parse_default_name(value, name, sizeof(name));
        if (strcmp(name, be->default_sink) != 0) {
            snprintf(be->default_sink, sizeof(be->default_sink), "%s", name);
            struct node_entry *n = find_by_name(be, name);
            vs_audio_emit(be->audio, VS_AUDIO_EVENT_DEFAULT_SINK_CHANGED,
                          n ? n->serial : 0, name);
        }
        bump(be);
        schedule_changed(be);
    } else if (subject == 0 && key && strcmp(key, "default.audio.source") == 0) {
        char name[sizeof(be->default_source)];
        parse_default_name(value, name, sizeof(name));
        if (strcmp(name, be->default_source) != 0) {
            snprintf(be->default_source, sizeof(be->default_source), "%s", name);
            struct node_entry *n = find_by_name(be, name);
            vs_audio_emit(be->audio, VS_AUDIO_EVENT_DEFAULT_SOURCE_CHANGED,
                          n ? n->serial : 0, name);
        }
        bump(be);
        schedule_changed(be);
    } else if (subject != 0 && key && strcmp(key, "target.object") == 0) {
        set_target(be, subject, value);
        bump(be);
        schedule_changed(be);
    } else if (subject != 0 && key == NULL) {
        /* every key on that subject was cleared */
        set_target(be, subject, NULL);
        bump(be);
    }
    return 0;
}

static const struct pw_metadata_events metadata_events = {
    PW_VERSION_METADATA_EVENTS,
    .property = metadata_property,
};

/* --- registry ------------------------------------------------------------- */

static vs_audio_node_kind kind_of(const char *media_class)
{
    if (!media_class) return 0;
    if (strcmp(media_class, "Audio/Sink") == 0) return VS_AUDIO_NODE_SINK;
    if (strcmp(media_class, "Audio/Source") == 0) return VS_AUDIO_NODE_SOURCE;
    if (strcmp(media_class, "Stream/Output/Audio") == 0)
        return VS_AUDIO_NODE_STREAM_OUTPUT;
    if (strcmp(media_class, "Stream/Input/Audio") == 0)
        return VS_AUDIO_NODE_STREAM_INPUT;
    /* Audio/Source/Virtual is what a monitor or a loopback source calls
     * itself; it behaves like a source for muting and listing. */
    if (strcmp(media_class, "Audio/Source/Virtual") == 0)
        return VS_AUDIO_NODE_SOURCE;
    return 0;
}

static void add_node(struct pw_backend *be, uint32_t id,
                     const struct spa_dict *props)
{
    vs_audio_node_kind kind = kind_of(spa_dict_lookup(props, PW_KEY_MEDIA_CLASS));
    if (kind == 0) return;

    struct node_entry *n = calloc(1, sizeof(*n));
    if (!n) return;
    n->be = be;
    n->global_id = id;
    n->kind = kind;
    n->pid = -1;
    n->n_channels = 2;

    const char *serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
    n->serial = serial ? (uint32_t)strtoul(serial, NULL, 10) : id;
    n->name = dup_or_empty(spa_dict_lookup(props, PW_KEY_NODE_NAME));
    n->description = dup_or_empty(spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION));
    n->app_name = dup_or_empty(spa_dict_lookup(props, PW_KEY_APP_NAME));
    n->icon_name = dup_or_empty(spa_dict_lookup(props, PW_KEY_APP_ICON_NAME));
    n->media_name = dup_or_empty(spa_dict_lookup(props, PW_KEY_MEDIA_NAME));
    const char *pid = spa_dict_lookup(props, PW_KEY_APP_PROCESS_ID);
    if (pid) n->pid = (int32_t)strtol(pid, NULL, 10);

    n->proxy = pw_registry_bind(be->registry, id, PW_TYPE_INTERFACE_Node,
                                PW_VERSION_NODE, 0);
    if (n->proxy) {
        pw_proxy_add_listener(n->proxy, &n->proxy_listener, &node_proxy_events, n);
        pw_proxy_add_object_listener(n->proxy, &n->object_listener, &node_events, n);
        /* Subscribing rather than a one-shot enum_params: the mixer has to
         * see a volume the user changed in another app's mixer too. */
        uint32_t ids[] = { SPA_PARAM_Props };
        pw_node_subscribe_params((struct pw_node *)n->proxy, ids, 1);
    }

    spa_list_append(&be->nodes, &n->link);
    bump(be);
    schedule_changed(be);
}

static void free_node(struct node_entry *n)
{
    spa_list_remove(&n->link);
    if (n->proxy) {
        spa_hook_remove(&n->object_listener);
        spa_hook_remove(&n->proxy_listener);
        pw_proxy_destroy(n->proxy);
    }
    free(n->name);
    free(n->description);
    free(n->app_name);
    free(n->icon_name);
    free(n->media_name);
    free(n);
}

static void add_link(struct pw_backend *be, uint32_t id,
                     const struct spa_dict *props)
{
    const char *out = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE);
    const char *in = spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE);
    if (!out || !in) return;
    struct link_entry *l = calloc(1, sizeof(*l));
    if (!l) return;
    l->global_id = id;
    l->out_node = (uint32_t)strtoul(out, NULL, 10);
    l->in_node = (uint32_t)strtoul(in, NULL, 10);
    spa_list_append(&be->links, &l->link);
    bump(be);
    schedule_changed(be);
}

static void registry_global(void *data, uint32_t id, uint32_t permissions,
                            const char *type, uint32_t version,
                            const struct spa_dict *props)
{
    (void)permissions; (void)version;
    struct pw_backend *be = data;
    if (!props) return;

    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        add_node(be, id, props);
    } else if (strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
        add_link(be, id, props);
    } else if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0 && !be->metadata) {
        const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (!name || strcmp(name, "default") != 0) return;
        be->metadata_proxy = pw_registry_bind(be->registry, id,
                                              PW_TYPE_INTERFACE_Metadata,
                                              PW_VERSION_METADATA, 0);
        if (!be->metadata_proxy) return;
        be->metadata = (struct pw_metadata *)be->metadata_proxy;
        be->metadata_id = id;
        pw_metadata_add_listener(be->metadata, &be->metadata_listener,
                                 &metadata_events, be);
        bump(be);
    }
}

static void registry_global_remove(void *data, uint32_t id)
{
    struct pw_backend *be = data;

    struct node_entry *n, *tn;
    spa_list_for_each_safe(n, tn, &be->nodes, link) {
        if (n->global_id != id) continue;
        /* The one removal the app has to react to rather than merely redraw:
         * the default sink going away is headphones being unplugged, a USB DAC
         * pulled, a Bluetooth headset dropping. Report it before the node is
         * freed, because by the time the UI re-lists there is nothing to name. */
        bool was_default = n->kind == VS_AUDIO_NODE_SINK && n->name
                        && strcmp(n->name, be->default_sink) == 0;
        uint32_t serial = n->serial;
        char name[256];
        snprintf(name, sizeof(name), "%s", n->name ? n->name : "");
        free_node(n);
        if (was_default) {
            be->default_sink[0] = '\0';
            vs_audio_emit(be->audio, VS_AUDIO_EVENT_DEFAULT_SINK_DISCONNECTED,
                          serial, name);
        }
        bump(be);
        schedule_changed(be);
        return;
    }

    struct link_entry *l, *tl;
    spa_list_for_each_safe(l, tl, &be->links, link) {
        if (l->global_id != id) continue;
        spa_list_remove(&l->link);
        free(l);
        bump(be);
        schedule_changed(be);
        return;
    }

    if (be->metadata && id == be->metadata_id) {
        spa_hook_remove(&be->metadata_listener);
        pw_proxy_destroy(be->metadata_proxy);
        be->metadata = NULL;
        be->metadata_proxy = NULL;
        bump(be);
    }
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* --- core sync ------------------------------------------------------------ */

static void core_done(void *data, uint32_t id, int seq)
{
    struct pw_backend *be = data;
    if (id == PW_ID_CORE) {
        be->last_seq = seq;
        pw_thread_loop_signal(be->loop, false);
    }
}

static void core_error(void *data, uint32_t id, int seq, int res,
                       const char *message)
{
    (void)id; (void)seq;
    struct pw_backend *be = data;
    /* A per-object error (a proxy that went away mid-call) is normal churn; a
     * core error means the connection itself is finished. */
    if (id == PW_ID_CORE) {
        fprintf(stderr, "vs-audio: pipewire core error %d: %s\n", res,
                message ? message : "");
        be->core_error = true;
        pw_thread_loop_signal(be->loop, false);
    }
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = core_done,
    .error = core_error,
};

struct seq_ctx { int seq; };

static bool seq_done(struct pw_backend *be, void *ctx)
{
    struct seq_ctx *s = ctx;
    return be->last_seq >= s->seq;
}

/** Round-trips the connection: everything the server had queued has arrived. */
static int roundtrip(struct pw_backend *be, unsigned timeout_ms)
{
    struct seq_ctx ctx = { .seq = pw_core_sync(be->core, PW_ID_CORE, 0) };
    return wait_for(be, seq_done, &ctx, timeout_ms);
}

/* --- open / close --------------------------------------------------------- */

static void pw_close(struct vs_audio *audio);

static int pw_open(struct vs_audio *audio)
{
    static bool initialised;
    if (!initialised) {
        pw_init(NULL, NULL);
        initialised = true; /* pw_init is refcounted but never deinit'd here:
                             * a library that tore down the global state would
                             * break a host that also uses PipeWire. */
    }

    struct pw_backend *be = calloc(1, sizeof(*be));
    if (!be) return -ENOMEM;
    be->audio = audio;
    audio->impl = be;
    spa_list_init(&be->nodes);
    spa_list_init(&be->links);

    be->loop = pw_thread_loop_new("vs-audio", NULL);
    if (!be->loop) { pw_close(audio); return -ENOMEM; }

    be->context = pw_context_new(pw_thread_loop_get_loop(be->loop), NULL, 0);
    if (!be->context) { pw_close(audio); return -ENOMEM; }

    be->core = pw_context_connect(be->context, NULL, 0);
    if (!be->core) {
        /* This is the whole PipeWire-absence detection: there is no separate
         * probe, because anything short of a real connection can be wrong
         * (a socket can exist with nothing behind it). */
        int err = errno ? -errno : -ENOENT;
        pw_close(audio);
        return err;
    }

    pw_core_add_listener(be->core, &be->core_listener, &core_events, be);
    be->registry = pw_core_get_registry(be->core, PW_VERSION_REGISTRY, 0);
    if (!be->registry) { pw_close(audio); return -EIO; }
    pw_registry_add_listener(be->registry, &be->registry_listener,
                             &registry_events, be);

    be->debounce_timer = pw_loop_add_timer(pw_thread_loop_get_loop(be->loop),
                                           on_debounce, be);

    if (pw_thread_loop_start(be->loop) < 0) { pw_close(audio); return -EIO; }

    pw_thread_loop_lock(be->loop);
    /* Two round trips: the first brings the registry's globals, the second the
     * replies to the binds and param subscriptions the first one triggered.
     * With one, a freshly opened handle lists nodes whose volume is still
     * unknown. */
    int rc = roundtrip(be, 3000);
    if (rc == 0) rc = roundtrip(be, 3000);
    pw_thread_loop_unlock(be->loop);
    if (rc != 0) { pw_close(audio); return rc; }

    audio->caps = VS_AUDIO_CAP_HAS_PIPEWIRE
                | VS_AUDIO_CAP_CAN_ROUTE_PER_STREAM
                | VS_AUDIO_CAP_CAN_BOOST_OVER_100
                | VS_AUDIO_CAP_HAS_EVENTS;
    return 0;
}

static void pw_close(struct vs_audio *audio)
{
    struct pw_backend *be = audio->impl;
    if (!be) return;

    if (be->loop) pw_thread_loop_stop(be->loop);

    struct node_entry *n, *tn;
    spa_list_for_each_safe(n, tn, &be->nodes, link) free_node(n);
    struct link_entry *l, *tl;
    spa_list_for_each_safe(l, tl, &be->links, link) { spa_list_remove(&l->link); free(l); }

    if (be->metadata_proxy) {
        spa_hook_remove(&be->metadata_listener);
        pw_proxy_destroy(be->metadata_proxy);
    }
    if (be->registry) pw_proxy_destroy((struct pw_proxy *)be->registry);
    if (be->core) pw_core_disconnect(be->core);
    if (be->context) pw_context_destroy(be->context);
    if (be->loop) pw_thread_loop_destroy(be->loop);
    free(be);
    audio->impl = NULL;
}

/* --- listing -------------------------------------------------------------- */

static int pw_list_nodes(struct vs_audio *audio, uint32_t mask,
                         vs_audio_node **out, size_t *count)
{
    struct pw_backend *be = audio->impl;
    struct vs_audio_nodes b;
    vs_audio_nodes_init(&b);

    pw_thread_loop_lock(be->loop);
    struct node_entry *n;
    spa_list_for_each(n, &be->nodes, link) {
        if ((n->kind & mask) == 0) continue;
        vs_audio_node *out_node = vs_audio_nodes_add(&b);
        if (!out_node) break;
        out_node->id = n->serial;
        out_node->pw_global_id = n->global_id;
        out_node->kind = n->kind;
        out_node->pid = n->pid;
        /* Channel volumes can differ per channel if another mixer set a
         * balance; the single number this API exposes is the loudest channel,
         * so a slider never silently reports less than the user can hear. */
        float volume = 0.0f;
        for (uint32_t c = 0; c < n->n_channels; c++)
            if (n->channel_volumes[c] > volume) volume = n->channel_volumes[c];
        out_node->volume = n->has_volume ? volume : VS_AUDIO_UNITY_VOLUME;
        out_node->mute = n->mute;
        out_node->has_volume = n->has_volume;
        out_node->is_default =
            (n->kind == VS_AUDIO_NODE_SINK && strcmp(n->name, be->default_sink) == 0)
            || (n->kind == VS_AUDIO_NODE_SOURCE
                && strcmp(n->name, be->default_source) == 0);
        if (n->kind & VS_AUDIO_NODE_ANY_STREAM) {
            out_node->target_id = resolve_target(be, target_for(be, n->global_id));
            out_node->linked_pw_global_id = linked_target_of(be, n->global_id);
        }
        vs_audio_nodes_set_str(&b, &out_node->name, n->name);
        vs_audio_nodes_set_str(&b, &out_node->description, n->description);
        vs_audio_nodes_set_str(&b, &out_node->app_name, n->app_name);
        vs_audio_nodes_set_str(&b, &out_node->icon_name, n->icon_name);
        vs_audio_nodes_set_str(&b, &out_node->media_name, n->media_name);
    }
    pw_thread_loop_unlock(be->loop);

    return vs_audio_nodes_finish(&b, out, count);
}

/* --- writes --------------------------------------------------------------- */

/** Builds and sends a Props pod. Caller holds the loop lock. */
static int send_props(struct node_entry *n, const float *volumes,
                      uint32_t n_volumes, const bool *mute)
{
    uint8_t buffer[1024];
    struct spa_pod_builder bld = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    struct spa_pod_frame frame;

    spa_pod_builder_push_object(&bld, &frame, SPA_TYPE_OBJECT_Props,
                                SPA_PARAM_Props);
    if (volumes && n_volumes > 0) {
        spa_pod_builder_prop(&bld, SPA_PROP_channelVolumes, 0);
        spa_pod_builder_array(&bld, sizeof(float), SPA_TYPE_Float, n_volumes,
                              volumes);
    }
    if (mute) {
        spa_pod_builder_prop(&bld, SPA_PROP_mute, 0);
        spa_pod_builder_bool(&bld, *mute);
    }
    const struct spa_pod *pod = spa_pod_builder_pop(&bld, &frame);
    if (!pod) return -ENOMEM;
    if (!n->proxy) return -ENODEV;

    pw_node_set_param((struct pw_node *)n->proxy, SPA_PARAM_Props, 0, pod);
    return 0;
}

struct volume_ctx { uint32_t serial; float wanted; };

static bool volume_arrived(struct pw_backend *be, void *ctx)
{
    struct volume_ctx *v = ctx;
    struct node_entry *n = find_by_serial(be, v->serial);
    if (!n) return true; /* the node went away; the wait is over either way */
    if (!n->has_volume) return false;
    for (uint32_t c = 0; c < n->n_channels; c++)
        if (!vs_audio_volume_equal(n->channel_volumes[c], v->wanted)) return false;
    return true;
}

static int pw_set_volume(struct vs_audio *audio, uint32_t id, float linear)
{
    struct pw_backend *be = audio->impl;
    pw_thread_loop_lock(be->loop);

    struct node_entry *n = find_by_serial(be, id);
    if (!n) { pw_thread_loop_unlock(be->loop); return -ENOENT; }

    uint32_t channels = n->n_channels ? n->n_channels : 2;
    float volumes[MAX_CHANNELS];
    for (uint32_t c = 0; c < channels; c++) volumes[c] = linear;

    int rc = send_props(n, volumes, channels, NULL);
    if (rc == 0) {
        /* The playbook's rule, and not a formality here: pw_node_set_param is
         * asynchronous and returns before the node has seen the pod, and a
         * server that clamps (a device whose hardware volume stops at unity)
         * reports success and then a different number. */
        struct volume_ctx ctx = { .serial = id, .wanted = linear };
        rc = wait_for(be, volume_arrived, &ctx, 2000);
        if (rc == -ETIMEDOUT) {
            struct node_entry *again = find_by_serial(be, id);
            if (again && again->has_volume) rc = -EIO; /* came back different */
        }
    }
    pw_thread_loop_unlock(be->loop);
    return rc;
}

struct mute_ctx { uint32_t serial; bool wanted; };

static bool mute_arrived(struct pw_backend *be, void *ctx)
{
    struct mute_ctx *m = ctx;
    struct node_entry *n = find_by_serial(be, m->serial);
    return !n || n->mute == m->wanted;
}

static int set_mute_locked(struct pw_backend *be, uint32_t id, bool mute)
{
    struct node_entry *n = find_by_serial(be, id);
    if (!n) return -ENOENT;
    if (n->mute == mute) return 0;
    int rc = send_props(n, NULL, 0, &mute);
    if (rc != 0) return rc;
    struct mute_ctx ctx = { .serial = id, .wanted = mute };
    return wait_for(be, mute_arrived, &ctx, 2000);
}

static int pw_set_mute(struct vs_audio *audio, uint32_t id, bool mute)
{
    struct pw_backend *be = audio->impl;
    pw_thread_loop_lock(be->loop);
    int rc = set_mute_locked(be, id, mute);
    pw_thread_loop_unlock(be->loop);
    return rc;
}

/* --- routing -------------------------------------------------------------- */

struct route_ctx { uint32_t stream_global; uint32_t sink_global; };

static bool links_moved(struct pw_backend *be, void *ctx)
{
    struct route_ctx *r = ctx;
    if (!find_by_global(be, r->stream_global)) return true; /* stream ended */
    uint32_t landed = linked_target_of(be, r->stream_global);
    /* Not "some link exists" but "the links are on the sink asked for": while
     * WirePlumber moves a stream there is a moment with links to both, and
     * calling that a success would make the test pass before the move did. */
    struct link_entry *l;
    spa_list_for_each(l, &be->links, link) {
        if (l->out_node == r->stream_global && l->in_node != r->sink_global)
            return false;
        if (l->in_node == r->stream_global && l->out_node != r->sink_global)
            return false;
    }
    return landed == r->sink_global;
}

struct clear_ctx { uint32_t stream_global; };

static bool target_cleared(struct pw_backend *be, void *ctx)
{
    struct clear_ctx *c = ctx;
    return target_for(be, c->stream_global) == NULL;
}

static int pw_route_stream(struct vs_audio *audio, uint32_t stream_id,
                           uint32_t sink_id)
{
    struct pw_backend *be = audio->impl;
    pw_thread_loop_lock(be->loop);

    struct node_entry *stream = find_by_serial(be, stream_id);
    if (!stream || !(stream->kind & VS_AUDIO_NODE_ANY_STREAM)) {
        pw_thread_loop_unlock(be->loop);
        return -ENOENT;
    }
    if (!be->metadata) { pw_thread_loop_unlock(be->loop); return -ENOTSUP; }

    if (sink_id == 0) {
        /* Clearing the pin puts the stream back under the default, which is
         * what "Follow system output" in the mixer means. */
        pw_metadata_set_property(be->metadata, stream->global_id,
                                 "target.object", NULL, NULL);
        struct clear_ctx ctx = { .stream_global = stream->global_id };
        int rc = wait_for(be, target_cleared, &ctx, 2000);
        pw_thread_loop_unlock(be->loop);
        return rc;
    }

    struct node_entry *sink = find_by_serial(be, sink_id);
    if (!sink || sink->kind != VS_AUDIO_NODE_SINK) {
        pw_thread_loop_unlock(be->loop);
        return -ENOENT;
    }

    /* The serial, not the global id: `target.object` is matched against
     * object.serial (or node.name), and a global id is recycled -- a stale pin
     * written against one would silently capture an unrelated later node.
     * That is not hypothetical; it happened while developing this backend. */
    char value[32];
    snprintf(value, sizeof(value), "%" PRIu32, sink->serial);
    pw_metadata_set_property(be->metadata, stream->global_id, "target.object",
                             "Spa:String", value);

    struct route_ctx ctx = { .stream_global = stream->global_id,
                             .sink_global = sink->global_id };
    int rc = wait_for(be, links_moved, &ctx, 3000);
    if (rc == -ETIMEDOUT) rc = -EIO; /* accepted, never arrived */
    pw_thread_loop_unlock(be->loop);
    return rc;
}

/* --- default sink --------------------------------------------------------- */

struct default_ctx { const char *name; };

static bool default_is(struct pw_backend *be, void *ctx)
{
    struct default_ctx *d = ctx;
    return strcmp(be->default_sink, d->name) == 0;
}

static int pw_set_default_sink(struct vs_audio *audio, uint32_t sink_id)
{
    struct pw_backend *be = audio->impl;
    pw_thread_loop_lock(be->loop);

    struct node_entry *sink = find_by_serial(be, sink_id);
    if (!sink || sink->kind != VS_AUDIO_NODE_SINK) {
        pw_thread_loop_unlock(be->loop);
        return -ENOENT;
    }
    if (!be->metadata) { pw_thread_loop_unlock(be->loop); return -ENOTSUP; }

    char json[320];
    snprintf(json, sizeof(json), "{\"name\":\"%s\"}", sink->name);
    /* Both keys: `default.configured.audio.sink` is the user's choice that
     * WirePlumber restores at the next login, `default.audio.sink` is what is
     * in effect now. Writing only the configured one leaves the session on the
     * old sink until something re-evaluates; only the effective one is
     * forgotten at logout. */
    pw_metadata_set_property(be->metadata, 0, "default.configured.audio.sink",
                             "Spa:String:JSON", json);
    pw_metadata_set_property(be->metadata, 0, "default.audio.sink",
                             "Spa:String:JSON", json);

    char wanted[sizeof(be->default_sink)];
    snprintf(wanted, sizeof(wanted), "%s", sink->name);
    struct default_ctx ctx = { .name = wanted };
    int rc = wait_for(be, default_is, &ctx, 2000);
    pw_thread_loop_unlock(be->loop);
    return rc;
}

const struct vs_audio_backend vs_audio_backend_pipewire = {
    .name = "pipewire",
    .open = pw_open,
    .close = pw_close,
    .list_nodes = pw_list_nodes,
    .set_volume = pw_set_volume,
    .set_mute = pw_set_mute,
    .route_stream = pw_route_stream,
    .set_default_sink = pw_set_default_sink,
};
