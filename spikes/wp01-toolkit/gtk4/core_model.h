// CoreModel for GTK: the GTK4 analogue of the Qt CoreModel. A GObject that
// holds one service's decoded snapshot and forwards commands to the bridge.
//
// GTK has no QVariantMap and no property binding engine, so the snapshot is
// kept as a parsed JsonNode-equivalent -- here a GVariant dictionary built
// from the JSON by hand -- and views connect to the "changed" signal. That
// difference is the point of the bake-off: see the report's view-code counts.
#pragma once

#define VS_HISTORY_MAX 64

#include <glib-object.h>

G_BEGIN_DECLS

#define CORE_TYPE_MODEL (core_model_get_type())
G_DECLARE_FINAL_TYPE(CoreModel, core_model, CORE, MODEL, GObject)

CoreModel *core_model_new(const char *service);

// Snapshot accessors. The model owns the returned history array.
double      core_model_get_double(CoreModel *self, const char *key, double fallback);
gboolean    core_model_get_bool(CoreModel *self, const char *key, gboolean fallback);
const char *core_model_get_string(CoreModel *self, const char *key, const char *fallback);
guint core_model_copy_history(CoreModel *self, double *out, guint cap);

// Send a command; `json` is the Codable command enum encoded as JSON.
int core_model_invoke(CoreModel *self, const char *json);

G_END_DECLS
