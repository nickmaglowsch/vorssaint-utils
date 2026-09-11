#include "core_model.h"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "corebridge.h"



struct _CoreModel {
    GObject parent_instance;
    char *service;
    GMutex lock;
    JsonNode *snapshot;          // owned, guarded by lock
    double history[VS_HISTORY_MAX];
    guint history_len;
};

G_DEFINE_FINAL_TYPE(CoreModel, core_model, G_TYPE_OBJECT)

enum { SIG_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void core_model_finalize(GObject *obj)
{
    CoreModel *self = CORE_MODEL(obj);
    g_clear_pointer(&self->snapshot, json_node_unref);
    g_free(self->service);
    g_mutex_clear(&self->lock);
    G_OBJECT_CLASS(core_model_parent_class)->finalize(obj);
}

static void core_model_class_init(CoreModelClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = core_model_finalize;
    signals[SIG_CHANGED] = g_signal_new("changed", CORE_TYPE_MODEL, G_SIGNAL_RUN_LAST,
                                        0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void core_model_init(CoreModel *self)
{
    g_mutex_init(&self->lock);
}

// Runs on the GTK main thread; emits "changed" for the views.
static gboolean emit_changed(gpointer data)
{
    g_signal_emit(CORE_MODEL(data), signals[SIG_CHANGED], 0);
    g_object_unref(data);
    return G_SOURCE_REMOVE;
}

static void apply_json(CoreModel *self, const char *json, gboolean from_thread)
{
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json, -1, NULL)) {
        g_object_unref(parser);
        return;
    }
    JsonNode *root = json_node_ref(json_parser_get_root(parser));
    g_object_unref(parser);

    g_mutex_lock(&self->lock);
    g_clear_pointer(&self->snapshot, json_node_unref);
    self->snapshot = root;

    self->history_len = 0;
    JsonObject *obj = json_node_get_object(root);
    if (obj && json_object_has_member(obj, "history")) {
        JsonArray *arr = json_object_get_array_member(obj, "history");
        guint n = MIN(json_array_get_length(arr), VS_HISTORY_MAX);
        for (guint i = 0; i < n; i++)
            self->history[i] = json_array_get_double_element(arr, i);
        self->history_len = n;
    }
    g_mutex_unlock(&self->lock);

    if (from_thread)
        g_idle_add(emit_changed, g_object_ref(self)); // hop to the GTK thread
    else
        g_signal_emit(self, signals[SIG_CHANGED], 0);
}

// Called on the bridge's thread, per the corebridge.h contract.
static void snapshot_trampoline(const char *json, void *ctx)
{
    apply_json(CORE_MODEL(ctx), json, TRUE);
}

CoreModel *core_model_new(const char *service)
{
    CoreModel *self = g_object_new(CORE_TYPE_MODEL, NULL);
    self->service = g_strdup(service);
    char *json = vs_snapshot(service);
    if (json) {
        apply_json(self, json, FALSE);
        vs_free(json);
    }
    vs_subscribe(service, snapshot_trampoline, self);
    return self;
}

static JsonObject *snap_obj(CoreModel *self)
{
    return self->snapshot ? json_node_get_object(self->snapshot) : NULL;
}

double core_model_get_double(CoreModel *self, const char *key, double fallback)
{
    g_mutex_lock(&self->lock);
    JsonObject *o = snap_obj(self);
    double v = (o && json_object_has_member(o, key))
                   ? json_object_get_double_member(o, key) : fallback;
    g_mutex_unlock(&self->lock);
    return v;
}

gboolean core_model_get_bool(CoreModel *self, const char *key, gboolean fallback)
{
    g_mutex_lock(&self->lock);
    JsonObject *o = snap_obj(self);
    gboolean v = (o && json_object_has_member(o, key))
                     ? json_object_get_boolean_member(o, key) : fallback;
    g_mutex_unlock(&self->lock);
    return v;
}

const char *core_model_get_string(CoreModel *self, const char *key, const char *fallback)
{
    // Borrowed from the snapshot node, which lives until the next snapshot.
    g_mutex_lock(&self->lock);
    JsonObject *o = snap_obj(self);
    const char *v = (o && json_object_has_member(o, key))
                        ? json_object_get_string_member(o, key) : fallback;
    g_mutex_unlock(&self->lock);
    return v ? v : fallback;
}

guint core_model_copy_history(CoreModel *self, double *out, guint cap)
{
    /* apply_json() rewrites history on the bridge's ticker thread under
       self->lock; hand the caller a snapshot instead of a live pointer. */
    g_mutex_lock(&self->lock);
    guint n = MIN(self->history_len, cap);
    memcpy(out, self->history, n * sizeof(double));
    g_mutex_unlock(&self->lock);
    return n;
}

int core_model_invoke(CoreModel *self, const char *json)
{
    return vs_command(self->service, json);
}
