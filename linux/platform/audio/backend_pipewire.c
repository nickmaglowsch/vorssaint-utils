/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * The PipeWire backend.
 *
 * Shape: a bare `pw_loop` that this backend never runs on a thread of its own.
 * The caller drives it -- `dispatch` iterates it without blocking, and the
 * blocking verbs iterate it until the server has confirmed what they asked
 * for or their budget runs out. That is the header's threading rule, and it
 * also means events uncovered in the middle of a `set_volume` are queued and
 * handed over by the next `dispatch` rather than delivered from inside the
 * write.
 *
 * What is mirrored locally, and why each one is needed:
 *
 *   nodes      every Audio/Sink, Audio/Source and audio Stream global, bound so
 *              its Props (channelVolumes, mute) can be read and written. Bound
 *              rather than merely noticed: the registry's global event carries
 *              a filtered subset of the properties, and the rest -- notably
 *              application.process.id and media.name -- only arrive on the
 *              node's own info event.
 *   links      just the node ids. `link.output.node` and `link.input.node` are
 *              in the registry global props, so a routing request is checked by
 *              watching where a stream's audio actually lands, not by trusting
 *              that the metadata write "worked".
 *   metadata   the `default` object: it holds the default sink and source, and
 *              it is also where a per-stream `target.object` pin is written.
 *              One object, two jobs.
 */

#include "vs_audio_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>

/* pipewire.h first, and not in alphabetical order: extensions/metadata.h uses
 * struct spa_hook in its prototypes without including the header that defines
 * it, so on its own it forward-declares a second, incompatible one inside
 * prototype scope and every add_listener call stops compiling. */
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/raw.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/parser.h>
#include <spa/utils/json.h>

#define MAX_CHANNELS SPA_AUDIO_MAX_CHANNELS
/** Longest a blocking verb waits for the server to confirm a write. */
#define WRITE_BUDGET_MS 2000
/** Routing waits longer: WirePlumber tears links down and builds new ones. */
#define ROUTE_BUDGET_MS 3000
#define CONNECT_BUDGET_MS 3000

struct pw_backend;

struct node_entry {
    struct spa_list link;
    struct pw_backend *be;

    uint32_t global_id;
    vs_audio_id serial;
    vs_audio_node_kind kind;

    char name[VS_AUDIO_NAME_MAX];
    char description[VS_AUDIO_DESCRIPTION_MAX];
    char app_name[VS_AUDIO_APP_NAME_MAX];
    char icon_name[VS_AUDIO_ICON_NAME_MAX];
    char media_name[VS_AUDIO_MEDIA_NAME_MAX];
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

/** A per-stream `target.object` pin, as the metadata reports it. */
struct target_entry {
    struct spa_list link;
    uint32_t subject; /* the stream's global id */
    char value[128];
};

struct pw_backend {
    vs_audio_system system;
    struct vs_audio_events events;

    struct pw_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct spa_hook core_listener;
    struct spa_hook registry_listener;
    bool entered;

    struct spa_list nodes;
    struct spa_list links;
    struct spa_list targets;

    struct pw_metadata *metadata;
    struct pw_proxy *metadata_proxy;
    struct spa_hook metadata_listener;
    uint32_t metadata_id;

    char default_sink[VS_AUDIO_NAME_MAX];
    char default_source[VS_AUDIO_NAME_MAX];

    struct spa_source *debounce_timer;
    int last_seq;
    bool core_failed;
};

/* --- small helpers -------------------------------------------------------- */

static struct node_entry *find_by_serial(struct pw_backend *be, vs_audio_id serial)
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
        if (strcmp(n->name, name) == 0) return n;
    return NULL;
}

static const char *target_for(struct pw_backend *be, uint32_t subject)
{
    struct target_entry *t;
    spa_list_for_each(t, &be->targets, link)
        if (t->subject == subject) return t->value;
    return NULL;
}

static void set_target(struct pw_backend *be, uint32_t subject, const char *value)
{
    struct target_entry *t, *tmp;
    spa_list_for_each_safe(t, tmp, &be->targets, link) {
        if (t->subject != subject) continue;
        if (!value) { spa_list_remove(&t->link); free(t); return; }
        vs_audio_copy_field(t->value, sizeof(t->value), value);
        return;
    }
    if (!value) return;
    t = calloc(1, sizeof(*t));
    if (!t) return;
    t->subject = subject;
    vs_audio_copy_field(t->value, sizeof(t->value), value);
    spa_list_append(&be->targets, &t->link);
}

/**
 * A `target.object` pin is either the sink's `object.serial` or its
 * `node.name`: WirePlumber accepts both, and something other than this backend
 * may have written it. Resolve either to the id this API speaks.
 */
static vs_audio_id resolve_target(struct pw_backend *be, const char *value)
{
    if (!value || !*value) return 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (end && *end == '\0' && parsed > 0)
        return find_by_serial(be, (vs_audio_id)parsed) ? (vs_audio_id)parsed : 0;
    struct node_entry *n = find_by_name(be, value);
    return n ? n->serial : 0;
}

/** The global id of whatever this stream's links actually land on. */
static uint32_t linked_global_of(struct pw_backend *be, uint32_t stream_global)
{
    struct link_entry *l;
    spa_list_for_each(l, &be->links, link) {
        if (l->out_node == stream_global) return l->in_node;
        if (l->in_node == stream_global) return l->out_node;
    }
    return 0;
}

static void schedule_changed(struct pw_backend *be)
{
    if (!be->debounce_timer) return;
    /* Rearmed from now, so a burst -- a device appearing brings a dozen globals
     * and params -- collapses into the one refresh the mixer wants. The timer
     * lives in the loop, so the caller's poll on `event_fd` wakes when it
     * fires; a debounce measured only inside `dispatch` would sit unfired until
     * something else happened to wake the caller. */
    struct timespec value = {
        .tv_sec = be->events.debounce_ms / 1000,
        .tv_nsec = (long)(be->events.debounce_ms % 1000) * 1000000L
    };
    if (value.tv_sec == 0 && value.tv_nsec == 0) value.tv_nsec = 1;
    pw_loop_update_timer(be->loop, be->debounce_timer, &value, NULL, false);
}

static void on_debounce(void *data, uint64_t expirations)
{
    (void)expirations;
    struct pw_backend *be = data;
    vs_audio_events_push(&be->events, VS_AUDIO_EVENT_CHANGED, 0, NULL);
}

/**
 * Runs the connection until `pred` is true or the budget is spent.
 *
 * This is where a write's read-back happens. `pw_node_set_param` and
 * `pw_metadata_set_property` are both asynchronous: they return before the
 * server has seen the request, so a function that returned success here would
 * be reporting that it had sent a message, not that anything had changed.
 */
static int pump_until(struct pw_backend *be, bool (*pred)(struct pw_backend *, void *),
                      void *ctx, unsigned budget_ms)
{
    uint64_t deadline = vs_audio_now_ms() + budget_ms;
    while (!pred(be, ctx)) {
        if (be->core_failed) return VS_ERR_BACKEND;
        uint64_t now = vs_audio_now_ms();
        if (now >= deadline) return VS_ERR_TIMEOUT;
        int slice = (int)(deadline - now);
        if (slice > 20) slice = 20;
        pw_loop_iterate(be->loop, slice);
    }
    return VS_OK;
}

/* --- core ----------------------------------------------------------------- */

static void core_done(void *data, uint32_t id, int seq)
{
    struct pw_backend *be = data;
    if (id == PW_ID_CORE) be->last_seq = seq;
}

static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
    (void)seq;
    struct pw_backend *be = data;
    /* A per-object error (a proxy that went away mid-call) is ordinary churn;
     * only a core error means the connection itself is finished. */
    if (id == PW_ID_CORE) {
        fprintf(stderr, "vs-audio: pipewire core error %d: %s\n", res,
                message ? message : "");
        be->core_failed = true;
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
    return be->last_seq >= ((struct seq_ctx *)ctx)->seq;
}

/** Round-trips the connection: everything the server had queued has arrived. */
static int roundtrip(struct pw_backend *be, unsigned budget_ms)
{
    struct seq_ctx ctx = { .seq = pw_core_sync(be->core, PW_ID_CORE, 0) };
    return pump_until(be, seq_done, &ctx, budget_ms);
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
     * typically the enumeration with everything at unity, the next carries what
     * is actually set. Both arrive as SPA_PARAM_Props, so take whichever
     * mentions channelVolumes and let the later one win, which it does since
     * these run in arrival order. */
    if (n_volumes > 0) {
        memcpy(n->channel_volumes, volumes, n_volumes * sizeof(float));
        n->n_channels = n_volumes;
        n->has_volume = true;
    }
    if (saw_mute) n->mute = mute;
    if (n_volumes > 0 || saw_mute) schedule_changed(n->be);
}

/**
 * The registry's `global` event carries only a filtered subset of a node's
 * properties -- enough to recognise it, not enough to show it. Measured on
 * PipeWire 1.0.5: `node.name`, `application.name` and `application.icon-name`
 * arrive there, but `application.process.id` and `media.name` do not, so a
 * mixer built on the registry dict alone lists every row with no pid and no
 * "now playing" line. The complete dict is on the node's own info event, which
 * is why every node is bound rather than merely noticed.
 */
static void node_info(void *data, const struct pw_node_info *info)
{
    struct node_entry *n = data;
    if (!info || !info->props) return;
    const struct spa_dict *props = info->props;
    const char *value;

    if ((value = spa_dict_lookup(props, PW_KEY_NODE_NAME)))
        vs_audio_copy_field(n->name, sizeof(n->name), value);
    if ((value = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION)))
        vs_audio_copy_field(n->description, sizeof(n->description), value);
    if ((value = spa_dict_lookup(props, PW_KEY_APP_NAME)))
        vs_audio_copy_field(n->app_name, sizeof(n->app_name), value);
    if ((value = spa_dict_lookup(props, PW_KEY_APP_ICON_NAME)))
        vs_audio_copy_field(n->icon_name, sizeof(n->icon_name), value);
    /* media.name changes as the track changes, which is the point of showing
     * it: the mixer row follows what the app is playing. */
    if ((value = spa_dict_lookup(props, PW_KEY_MEDIA_NAME)))
        vs_audio_copy_field(n->media_name, sizeof(n->media_name), value);
    if ((value = spa_dict_lookup(props, PW_KEY_APP_PROCESS_ID)))
        n->pid = (int32_t)strtol(value, NULL, 10);

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
        vs_audio_copy_field(out, out_len, value);
        return;
    }
    while (spa_json_get_string(&it[1], key, sizeof(key)) > 0) {
        if (strcmp(key, "name") == 0) {
            if (spa_json_get_string(&it[1], out, (int)out_len) <= 0) out[0] = '\0';
            return;
        }
        const char *skip = NULL;
        if (spa_json_next(&it[1], &skip) <= 0) break;
    }
}

static int metadata_property(void *data, uint32_t subject, const char *key,
                             const char *type, const char *value)
{
    (void)type;
    struct pw_backend *be = data;

    if (subject == 0 && key && strcmp(key, "default.audio.sink") == 0) {
        char name[VS_AUDIO_NAME_MAX];
        parse_default_name(value, name, sizeof(name));
        if (strcmp(name, be->default_sink) != 0) {
            vs_audio_copy_field(be->default_sink, sizeof(be->default_sink), name);
            struct node_entry *n = find_by_name(be, name);
            vs_audio_events_push(&be->events, VS_AUDIO_EVENT_DEFAULT_SINK_CHANGED,
                                 n ? n->serial : 0, name);
        }
        schedule_changed(be);
    } else if (subject == 0 && key && strcmp(key, "default.audio.source") == 0) {
        char name[VS_AUDIO_NAME_MAX];
        parse_default_name(value, name, sizeof(name));
        if (strcmp(name, be->default_source) != 0) {
            vs_audio_copy_field(be->default_source, sizeof(be->default_source), name);
            struct node_entry *n = find_by_name(be, name);
            vs_audio_events_push(&be->events, VS_AUDIO_EVENT_DEFAULT_SOURCE_CHANGED,
                                 n ? n->serial : 0, name);
        }
        schedule_changed(be);
    } else if (subject != 0 && key && strcmp(key, "target.object") == 0) {
        set_target(be, subject, value);
        schedule_changed(be);
    } else if (subject != 0 && key == NULL) {
        set_target(be, subject, NULL); /* every key on that subject was cleared */
        schedule_changed(be);
    }
    return 0;
}

static const struct pw_metadata_events metadata_events = {
    PW_VERSION_METADATA_EVENTS,
    .property = metadata_property,
};

/* --- registry ------------------------------------------------------------- */

static uint32_t kind_of(const char *media_class)
{
    if (!media_class) return 0;
    if (strcmp(media_class, "Audio/Sink") == 0) return VS_AUDIO_NODE_SINK;
    if (strcmp(media_class, "Audio/Source") == 0) return VS_AUDIO_NODE_SOURCE;
    if (strcmp(media_class, "Stream/Output/Audio") == 0) return VS_AUDIO_NODE_STREAM_OUTPUT;
    if (strcmp(media_class, "Stream/Input/Audio") == 0) return VS_AUDIO_NODE_STREAM_INPUT;
    /* A monitor or loopback source calls itself Audio/Source/Virtual; it
     * behaves like a source for listing and for muting. */
    if (strcmp(media_class, "Audio/Source/Virtual") == 0) return VS_AUDIO_NODE_SOURCE;
    return 0;
}

static void add_node(struct pw_backend *be, uint32_t id, const struct spa_dict *props)
{
    uint32_t kind = kind_of(spa_dict_lookup(props, PW_KEY_MEDIA_CLASS));
    if (kind == 0) return;

    struct node_entry *n = calloc(1, sizeof(*n));
    if (!n) return;
    n->be = be;
    n->global_id = id;
    n->kind = (vs_audio_node_kind)kind;
    n->pid = -1;
    n->n_channels = 2;

    const char *serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
    n->serial = serial ? (vs_audio_id)strtoul(serial, NULL, 10) : id;
    vs_audio_copy_field(n->name, sizeof(n->name),
                        spa_dict_lookup(props, PW_KEY_NODE_NAME));
    vs_audio_copy_field(n->description, sizeof(n->description),
                        spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION));
    vs_audio_copy_field(n->app_name, sizeof(n->app_name),
                        spa_dict_lookup(props, PW_KEY_APP_NAME));
    vs_audio_copy_field(n->icon_name, sizeof(n->icon_name),
                        spa_dict_lookup(props, PW_KEY_APP_ICON_NAME));

    n->proxy = pw_registry_bind(be->registry, id, PW_TYPE_INTERFACE_Node,
                                PW_VERSION_NODE, 0);
    if (n->proxy) {
        pw_proxy_add_listener(n->proxy, &n->proxy_listener, &node_proxy_events, n);
        pw_proxy_add_object_listener(n->proxy, &n->object_listener, &node_events, n);
        /* Subscribed, not enumerated once: the mixer has to see a volume the
         * user changed in another application's mixer too. */
        uint32_t ids[] = { SPA_PARAM_Props };
        pw_node_subscribe_params((struct pw_node *)n->proxy, ids, 1);
    }

    spa_list_append(&be->nodes, &n->link);
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
    free(n);
}

static void add_link(struct pw_backend *be, uint32_t id, const struct spa_dict *props)
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
         * pulled, a Bluetooth headset dropping. Reported before the node is
         * freed, because once it is gone there is nothing left to name. */
        bool was_default = n->kind == VS_AUDIO_NODE_SINK
                        && strcmp(n->name, be->default_sink) == 0;
        vs_audio_id serial = n->serial;
        char name[VS_AUDIO_NAME_MAX];
        vs_audio_copy_field(name, sizeof(name), n->name);
        free_node(n);
        if (was_default) {
            be->default_sink[0] = '\0';
            vs_audio_events_push(&be->events,
                                 VS_AUDIO_EVENT_DEFAULT_SINK_DISCONNECTED,
                                 serial, name);
        }
        schedule_changed(be);
        return;
    }

    struct link_entry *l, *tl;
    spa_list_for_each_safe(l, tl, &be->links, link) {
        if (l->global_id != id) continue;
        spa_list_remove(&l->link);
        free(l);
        schedule_changed(be);
        return;
    }

    if (be->metadata && id == be->metadata_id) {
        spa_hook_remove(&be->metadata_listener);
        pw_proxy_destroy(be->metadata_proxy);
        be->metadata = NULL;
        be->metadata_proxy = NULL;
    }
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* --- listing -------------------------------------------------------------- */

static int pw_list(vs_audio_system *self, uint32_t kind_mask,
                   vs_audio_node **nodes_out, size_t *count_out)
{
    struct pw_backend *be = self->impl;
    *nodes_out = NULL;
    *count_out = 0;
    if ((kind_mask & VS_AUDIO_NODE_ANY) == 0) return VS_ERR_INVALID;

    /* One non-blocking turn of the loop first, so a listing taken right after
     * something changed reflects the change rather than the state before it. */
    pw_loop_iterate(be->loop, 0);

    vs_audio_node *nodes = NULL;
    size_t count = 0, capacity = 0;
    struct node_entry *n;
    spa_list_for_each(n, &be->nodes, link) {
        if ((n->kind & kind_mask) == 0) continue;
        if (!vs_audio_nodes_reserve(&nodes, count, &capacity)) {
            free(nodes);
            return VS_ERR_NO_MEM;
        }
        vs_audio_node *out = &nodes[count++];
        vs_audio_node_init(out);
        out->id = n->serial;
        out->kind = n->kind;
        vs_audio_copy_field(out->name, sizeof(out->name), n->name);
        vs_audio_copy_field(out->description, sizeof(out->description), n->description);
        vs_audio_copy_field(out->app_name, sizeof(out->app_name), n->app_name);
        vs_audio_copy_field(out->icon_name, sizeof(out->icon_name), n->icon_name);
        vs_audio_copy_field(out->media_name, sizeof(out->media_name), n->media_name);
        out->pid = n->pid;
        if (n->pid >= 0) out->flags |= VS_AUDIO_NODE_HAS_PID;
        if (n->has_volume) {
            /* Channels can differ if another mixer set a balance; the single
             * number this API exposes is the loudest, so a slider never reports
             * less than the user can hear. */
            float volume = 0.0f;
            for (uint32_t c = 0; c < n->n_channels; c++)
                if (n->channel_volumes[c] > volume) volume = n->channel_volumes[c];
            out->volume = volume;
            out->flags |= VS_AUDIO_NODE_HAS_VOLUME;
        }
        if (n->mute) out->flags |= VS_AUDIO_NODE_MUTED;
        if ((n->kind == VS_AUDIO_NODE_SINK && strcmp(n->name, be->default_sink) == 0)
            || (n->kind == VS_AUDIO_NODE_SOURCE
                && strcmp(n->name, be->default_source) == 0))
            out->flags |= VS_AUDIO_NODE_IS_DEFAULT;
        if (n->kind & VS_AUDIO_NODE_ANY_STREAM) {
            out->target_id = resolve_target(be, target_for(be, n->global_id));
            struct node_entry *landed =
                find_by_global(be, linked_global_of(be, n->global_id));
            out->effective_id = landed ? landed->serial : 0;
        }
    }

    *nodes_out = nodes;
    *count_out = count;
    return VS_OK;
}

static void pw_free_list(vs_audio_system *self, vs_audio_node *nodes, size_t count)
{
    (void)self; (void)count;
    free(nodes);
}

/* --- writes --------------------------------------------------------------- */

/** Builds and sends a Props pod. */
static int send_props(struct node_entry *n, const float *volumes,
                      uint32_t n_volumes, const bool *mute)
{
    uint8_t buffer[1024];
    struct spa_pod_builder bld = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    struct spa_pod_frame frame;

    spa_pod_builder_push_object(&bld, &frame, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    if (volumes && n_volumes > 0) {
        spa_pod_builder_prop(&bld, SPA_PROP_channelVolumes, 0);
        spa_pod_builder_array(&bld, sizeof(float), SPA_TYPE_Float, n_volumes, volumes);
    }
    if (mute) {
        spa_pod_builder_prop(&bld, SPA_PROP_mute, 0);
        spa_pod_builder_bool(&bld, *mute);
    }
    const struct spa_pod *pod = spa_pod_builder_pop(&bld, &frame);
    if (!pod) return VS_ERR_NO_MEM;
    if (!n->proxy) return VS_ERR_NOT_FOUND;

    pw_node_set_param((struct pw_node *)n->proxy, SPA_PARAM_Props, 0, pod);
    return VS_OK;
}

struct volume_ctx { vs_audio_id serial; float wanted; };

static bool volume_arrived(struct pw_backend *be, void *ctx)
{
    struct volume_ctx *v = ctx;
    struct node_entry *n = find_by_serial(be, v->serial);
    if (!n) return true; /* it went away; the wait is over either way */
    if (!n->has_volume) return false;
    for (uint32_t c = 0; c < n->n_channels; c++)
        if (!vs_audio_volume_equal(n->channel_volumes[c], v->wanted)) return false;
    return true;
}

static int pw_set_volume(vs_audio_system *self, vs_audio_id id, float linear)
{
    struct pw_backend *be = self->impl;
    if (id == 0) return VS_ERR_INVALID;
    struct node_entry *n = find_by_serial(be, id);
    if (!n) return VS_ERR_NOT_FOUND;

    float wanted = vs_audio_clamp_volume(linear);
    uint32_t channels = n->n_channels ? n->n_channels : 2;
    float volumes[MAX_CHANNELS];
    for (uint32_t c = 0; c < channels; c++) volumes[c] = wanted;

    int rc = send_props(n, volumes, channels, NULL);
    if (rc != VS_OK) return rc;

    struct volume_ctx ctx = { .serial = id, .wanted = wanted };
    rc = pump_until(be, volume_arrived, &ctx, WRITE_BUDGET_MS);
    if (rc == VS_ERR_TIMEOUT) {
        /* A node that answered with a *different* value is a server that
         * clamped, not one that was slow; say which. */
        struct node_entry *again = find_by_serial(be, id);
        if (again && again->has_volume) return VS_ERR_NOT_APPLIED;
    }
    return rc;
}

struct mute_ctx { vs_audio_id serial; bool wanted; };

static bool mute_arrived(struct pw_backend *be, void *ctx)
{
    struct mute_ctx *m = ctx;
    struct node_entry *n = find_by_serial(be, m->serial);
    return !n || n->mute == m->wanted;
}

static int pw_set_mute(vs_audio_system *self, vs_audio_id id, bool mute)
{
    struct pw_backend *be = self->impl;
    if (id == 0) return VS_ERR_INVALID;
    struct node_entry *n = find_by_serial(be, id);
    if (!n) return VS_ERR_NOT_FOUND;
    if (n->mute == mute) return VS_OK;

    int rc = send_props(n, NULL, 0, &mute);
    if (rc != VS_OK) return rc;
    struct mute_ctx ctx = { .serial = id, .wanted = mute };
    rc = pump_until(be, mute_arrived, &ctx, WRITE_BUDGET_MS);
    return rc == VS_ERR_TIMEOUT ? VS_ERR_NOT_APPLIED : rc;
}

/* --- routing -------------------------------------------------------------- */

struct route_ctx { uint32_t stream_global; uint32_t sink_global; };

static bool links_moved(struct pw_backend *be, void *ctx)
{
    struct route_ctx *r = ctx;
    if (!find_by_global(be, r->stream_global)) return true; /* the stream ended */
    /* Not "a link to the sink exists" but "every one of this stream's links is
     * on it": while WirePlumber moves a stream there is a moment with links to
     * both, and calling that success would report the move before it happened. */
    bool any = false;
    struct link_entry *l;
    spa_list_for_each(l, &be->links, link) {
        uint32_t other = 0;
        if (l->out_node == r->stream_global) other = l->in_node;
        else if (l->in_node == r->stream_global) other = l->out_node;
        else continue;
        if (other != r->sink_global) return false;
        any = true;
    }
    return any;
}

struct clear_ctx { uint32_t stream_global; };

static bool target_cleared(struct pw_backend *be, void *ctx)
{
    return target_for(be, ((struct clear_ctx *)ctx)->stream_global) == NULL;
}

static int pw_route_stream(vs_audio_system *self, vs_audio_id stream_id,
                           vs_audio_id sink_id)
{
    struct pw_backend *be = self->impl;
    if (stream_id == 0) return VS_ERR_INVALID;
    struct node_entry *stream = find_by_serial(be, stream_id);
    if (!stream || !(stream->kind & VS_AUDIO_NODE_ANY_STREAM)) return VS_ERR_NOT_FOUND;
    if (!be->metadata) return VS_ERR_UNSUPPORTED;

    if (sink_id == 0) {
        /* Clearing the pin puts the stream back under the default, which is
         * what "Follow system output" in the mixer means. */
        pw_metadata_set_property(be->metadata, stream->global_id,
                                 "target.object", NULL, NULL);
        struct clear_ctx ctx = { .stream_global = stream->global_id };
        int rc = pump_until(be, target_cleared, &ctx, ROUTE_BUDGET_MS);
        return rc == VS_ERR_TIMEOUT ? VS_ERR_NOT_APPLIED : rc;
    }

    struct node_entry *sink = find_by_serial(be, sink_id);
    if (!sink || sink->kind != VS_AUDIO_NODE_SINK) return VS_ERR_NOT_FOUND;

    /* The serial, not the global id: `target.object` is matched against
     * `object.serial` (or `node.name`), and a global id is recycled -- a pin
     * written against one would eventually capture an unrelated later node.
     * That is not hypothetical; it happened while developing this backend. */
    char value[32];
    snprintf(value, sizeof(value), "%" PRIu32, sink->serial);
    pw_metadata_set_property(be->metadata, stream->global_id, "target.object",
                             "Spa:String", value);

    struct route_ctx ctx = { .stream_global = stream->global_id,
                             .sink_global = sink->global_id };
    int rc = pump_until(be, links_moved, &ctx, ROUTE_BUDGET_MS);
    return rc == VS_ERR_TIMEOUT ? VS_ERR_NOT_APPLIED : rc;
}

/* --- default sink --------------------------------------------------------- */

static bool default_is(struct pw_backend *be, void *ctx)
{
    return strcmp(be->default_sink, (const char *)ctx) == 0;
}

static int pw_set_default_sink(vs_audio_system *self, vs_audio_id sink_id)
{
    struct pw_backend *be = self->impl;
    if (sink_id == 0) return VS_ERR_INVALID;
    struct node_entry *sink = find_by_serial(be, sink_id);
    if (!sink || sink->kind != VS_AUDIO_NODE_SINK) return VS_ERR_NOT_FOUND;
    if (!be->metadata) return VS_ERR_UNSUPPORTED;

    char json[VS_AUDIO_NAME_MAX + 16];
    snprintf(json, sizeof(json), "{\"name\":\"%s\"}", sink->name);
    /* Both keys. `default.configured.audio.sink` is the user's choice, which
     * WirePlumber restores at the next login; `default.audio.sink` is what is
     * in effect now. Writing only the configured one leaves the session on the
     * old sink until something re-evaluates, and only the effective one is
     * forgotten at logout. */
    pw_metadata_set_property(be->metadata, 0, "default.configured.audio.sink",
                             "Spa:String:JSON", json);
    pw_metadata_set_property(be->metadata, 0, "default.audio.sink",
                             "Spa:String:JSON", json);

    char wanted[VS_AUDIO_NAME_MAX];
    vs_audio_copy_field(wanted, sizeof(wanted), sink->name);
    int rc = pump_until(be, default_is, wanted, ROUTE_BUDGET_MS);
    return rc == VS_ERR_TIMEOUT ? VS_ERR_NOT_APPLIED : rc;
}

static int pw_mute_all_inputs(vs_audio_system *self, bool mute, size_t *changed_out)
{
    return vs_audio_mute_all_inputs_generic(self, mute, changed_out);
}

/* --- events --------------------------------------------------------------- */

static int pw_set_event_callback(vs_audio_system *self, vs_audio_event_cb callback,
                                 void *user_data)
{
    struct pw_backend *be = self->impl;
    be->events.callback = callback;
    be->events.user_data = user_data;
    return VS_OK;
}

static int pw_event_fd(vs_audio_system *self)
{
    struct pw_backend *be = self->impl;
    return pw_loop_get_fd(be->loop);
}

static int pw_dispatch(vs_audio_system *self)
{
    struct pw_backend *be = self->impl;
    /* Non-blocking: the header says dispatch never blocks, and the caller's
     * own poll on event_fd is what decides when there is work. */
    pw_loop_iterate(be->loop, 0);
    if (be->core_failed) return VS_ERR_BACKEND;
    return vs_audio_events_drain(&be->events);
}

/* --- lifecycle ------------------------------------------------------------ */

static void pw_destroy(vs_audio_system *self)
{
    struct pw_backend *be = self->impl;
    if (!be) return;

    struct node_entry *n, *tn;
    spa_list_for_each_safe(n, tn, &be->nodes, link) free_node(n);
    struct link_entry *l, *tl;
    spa_list_for_each_safe(l, tl, &be->links, link) { spa_list_remove(&l->link); free(l); }
    struct target_entry *t, *tt;
    spa_list_for_each_safe(t, tt, &be->targets, link) { spa_list_remove(&t->link); free(t); }

    if (be->metadata_proxy) {
        spa_hook_remove(&be->metadata_listener);
        pw_proxy_destroy(be->metadata_proxy);
    }
    if (be->registry) pw_proxy_destroy((struct pw_proxy *)be->registry);
    if (be->core) pw_core_disconnect(be->core);
    if (be->context) pw_context_destroy(be->context);
    if (be->debounce_timer) pw_loop_destroy_source(be->loop, be->debounce_timer);
    if (be->loop) {
        if (be->entered) pw_loop_leave(be->loop);
        pw_loop_destroy(be->loop);
    }
    vs_audio_events_dispose(&be->events);
    free(be);
}

vs_audio_system *vs_audio_pipewire_create(int *result_out)
{
    static bool initialised;
    if (!initialised) {
        /* pw_init is refcounted but deliberately never undone: a library that
         * tore down PipeWire's global state on close would break a host process
         * that also uses PipeWire (the capture backend will). */
        pw_init(NULL, NULL);
        initialised = true;
    }

    struct pw_backend *be = calloc(1, sizeof(*be));
    if (!be) { *result_out = VS_ERR_NO_MEM; return NULL; }
    spa_list_init(&be->nodes);
    spa_list_init(&be->links);
    spa_list_init(&be->targets);
    vs_audio_events_init(&be->events, 80);

    be->system = (vs_audio_system){
        .name = "pipewire",
        .impl = be,
        .list = pw_list,
        .free_list = pw_free_list,
        .set_volume = pw_set_volume,
        .set_mute = pw_set_mute,
        .route_stream = pw_route_stream,
        .set_default_sink = pw_set_default_sink,
        .mute_all_inputs = pw_mute_all_inputs,
        .set_event_callback = pw_set_event_callback,
        .event_fd = pw_event_fd,
        .dispatch = pw_dispatch,
        .destroy = pw_destroy,
    };

    int result = VS_ERR_NO_BACKEND;
    be->loop = pw_loop_new(NULL);
    if (!be->loop) { result = VS_ERR_NO_MEM; goto fail; }
    /* The loop belongs to this thread from here on; the header's rule that one
     * instance belongs to one thread is what makes that safe. */
    pw_loop_enter(be->loop);
    be->entered = true;

    be->context = pw_context_new(be->loop, NULL, 0);
    if (!be->context) { result = VS_ERR_NO_MEM; goto fail; }

    be->core = pw_context_connect(be->context, NULL, 0);
    if (!be->core) {
        /* This is the whole of the PipeWire-absence detection. There is no
         * separate probe, because anything short of a real connection can be
         * wrong: a socket can exist with nothing behind it. */
        result = VS_ERR_NO_BACKEND;
        goto fail;
    }

    pw_core_add_listener(be->core, &be->core_listener, &core_events, be);
    be->registry = pw_core_get_registry(be->core, PW_VERSION_REGISTRY, 0);
    if (!be->registry) { result = VS_ERR_BACKEND; goto fail; }
    pw_registry_add_listener(be->registry, &be->registry_listener, &registry_events, be);

    be->debounce_timer = pw_loop_add_timer(be->loop, on_debounce, be);

    /* Two round trips: the first brings the registry's globals, the second the
     * replies to the binds and param subscriptions the first one triggered.
     * With one, a freshly created backend lists nodes whose volume is not known
     * yet and whose pid is still -1. */
    int rc = roundtrip(be, CONNECT_BUDGET_MS);
    if (rc == VS_OK) rc = roundtrip(be, CONNECT_BUDGET_MS);
    if (rc != VS_OK) { result = rc; goto fail; }

    /* The graph that just arrived is the starting state, not news. */
    be->events.count = 0;
    be->events.head = 0;
    be->events.overflowed = false;

    be->system.capabilities = VS_AUDIO_HAS_PIPEWIRE
                            | VS_AUDIO_CAN_ROUTE_PER_STREAM
                            | VS_AUDIO_CAN_BOOST_OVER_100
                            | VS_AUDIO_HAS_EVENTS;
    *result_out = VS_OK;
    return &be->system;

fail:
    pw_destroy(&be->system);
    *result_out = result;
    return NULL;
}
