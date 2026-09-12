/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * The xdg-desktop-portal side: Request/Response choreography for ScreenCast
 * and Screenshot.
 *
 * Everything here is deliberately literal about what came back. The WP-02
 * spike found that xdg-desktop-portal-wlr 0.7.1 answers a WINDOW request with a
 * MONITOR stream, so `Start` results are not "the stream we asked for" but "a
 * stream, whose own source_type must be read". The engine records what was
 * granted and lets the caller decide; it never reports the request back as if
 * it were the answer.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gio/gunixfdlist.h>

#include "vs_capture_internal.h"

#define PORTAL_BUS "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define SCREENCAST_IFACE "org.freedesktop.portal.ScreenCast"
#define SCREENSHOT_IFACE "org.freedesktop.portal.Screenshot"

/* A portal Request lives at a path derived from our own unique bus name, so we
 * can subscribe to its Response before the call that creates it. */
static char *request_path_for(GDBusConnection *bus, const char *token)
{
    const char *unique = g_dbus_connection_get_unique_name(bus);
    GString *sanitised = g_string_new(unique && unique[0] == ':' ? unique + 1 : unique);
    for (gsize i = 0; i < sanitised->len; i++)
        if (sanitised->str[i] == '.') sanitised->str[i] = '_';
    char *path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s",
                                 sanitised->str, token);
    g_string_free(sanitised, TRUE);
    return path;
}

static char *fresh_token(const char *prefix)
{
    static guint counter = 0;
    return g_strdup_printf("vs_%s_%u_%u", prefix, (guint)getpid(), ++counter);
}

typedef struct {
    GMainLoop *loop;
    GMainContext *context;
    guint32 response;
    GVariant *results;
    bool got;
} request_wait;

static void on_response(GDBusConnection *bus, const gchar *sender, const gchar *path,
                        const gchar *iface, const gchar *signal, GVariant *params,
                        gpointer user_data)
{
    (void)bus; (void)sender; (void)path; (void)iface; (void)signal;
    request_wait *wait = user_data;
    g_variant_get(params, "(u@a{sv})", &wait->response, &wait->results);
    wait->got = true;
    g_main_loop_quit(wait->loop);
}

static gboolean on_request_timeout(gpointer user_data)
{
    request_wait *wait = user_data;
    VS_LOG("portal Request timed out");
    g_main_loop_quit(wait->loop);
    return G_SOURCE_REMOVE;
}

/*
 * Call `method`, then run a main loop on a context of our own until the
 * Response signal arrives. A private GMainContext rather than the default one
 * matters: the engine is a library and the application owns the default
 * context, so spinning that one here would run the application's own sources
 * from inside a capture call.
 */
static GVariant *portal_call(vs_capture_engine *engine, const char *iface,
                             const char *method, GVariant *args, const char *token,
                             guint timeout_s, guint32 *response_out)
{
    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);

    char *predicted = request_path_for(engine->bus, token);
    request_wait wait = { .loop = g_main_loop_new(context, FALSE), .context = context,
                          .response = 2, .results = NULL, .got = false };

    guint subscription = g_dbus_connection_signal_subscribe(
        engine->bus, PORTAL_BUS, "org.freedesktop.portal.Request", "Response",
        predicted, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_response, &wait, NULL);

    GError *error = NULL;
    GVariant *handle = g_dbus_connection_call_sync(
        engine->bus, PORTAL_BUS, PORTAL_PATH, iface, method, args,
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &error);
    if (!handle) {
        VS_LOG("%s.%s failed: %s", iface, method, error->message);
        g_error_free(error);
        g_dbus_connection_signal_unsubscribe(engine->bus, subscription);
        g_main_loop_unref(wait.loop);
        g_main_context_pop_thread_default(context);
        g_main_context_unref(context);
        g_free(predicted);
        if (response_out) *response_out = 2;
        return NULL;
    }
    const char *actual = NULL;
    g_variant_get(handle, "(&o)", &actual);
    if (g_strcmp0(actual, predicted) != 0) {
        /* The portal is allowed to pick its own handle; resubscribe on it. */
        VS_LOG("%s.%s: handle %s (predicted %s)", iface, method, actual, predicted);
        g_dbus_connection_signal_unsubscribe(engine->bus, subscription);
        subscription = g_dbus_connection_signal_subscribe(
            engine->bus, PORTAL_BUS, "org.freedesktop.portal.Request", "Response",
            actual, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_response, &wait, NULL);
    }
    g_variant_unref(handle);

    GSource *timeout = g_timeout_source_new_seconds(timeout_s);
    g_source_set_callback(timeout, on_request_timeout, &wait, NULL);
    g_source_attach(timeout, context);
    g_main_loop_run(wait.loop);
    g_source_destroy(timeout);
    g_source_unref(timeout);

    g_dbus_connection_signal_unsubscribe(engine->bus, subscription);
    g_main_loop_unref(wait.loop);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
    g_free(predicted);

    if (response_out) *response_out = wait.got ? wait.response : 2;
    return wait.results;
}

int vs_portal_get_uint(vs_capture_engine *engine, const char *iface,
                       const char *property, uint32_t *value_out)
{
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        engine->bus, PORTAL_BUS, PORTAL_PATH, "org.freedesktop.DBus.Properties",
        "Get", g_variant_new("(ss)", iface, property), NULL,
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    if (!reply) {
        VS_LOG("%s.%s unavailable: %s", iface, property, error->message);
        g_error_free(error);
        return VS_ERR_UNSUPPORTED;
    }
    GVariant *inner = NULL;
    g_variant_get(reply, "(v)", &inner);
    int rc = VS_ERR_BACKEND;
    if (inner && g_variant_is_of_type(inner, G_VARIANT_TYPE_UINT32)) {
        if (value_out) *value_out = g_variant_get_uint32(inner);
        rc = VS_OK;
    }
    if (inner) g_variant_unref(inner);
    g_variant_unref(reply);
    return rc;
}

static int create_session(vs_capture_engine *engine, char **handle_out)
{
    char *token = fresh_token("cs");
    char *session_token = fresh_token("sess");
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token));
    g_variant_builder_add(&options, "{sv}", "session_handle_token",
                          g_variant_new_string(session_token));

    guint32 response = 2;
    GVariant *results = portal_call(engine, SCREENCAST_IFACE, "CreateSession",
                                    g_variant_new("(a{sv})", &options), token, 30,
                                    &response);
    g_free(token);
    g_free(session_token);
    if (response != 0 || !results) {
        VS_LOG("CreateSession response=%u", response);
        if (results) g_variant_unref(results);
        return VS_ERR_BACKEND;
    }
    GVariant *handle = g_variant_lookup_value(results, "session_handle",
                                              G_VARIANT_TYPE_STRING);
    if (!handle) {
        g_variant_unref(results);
        return VS_ERR_BACKEND;
    }
    *handle_out = g_strdup(g_variant_get_string(handle, NULL));
    g_variant_unref(handle);
    g_variant_unref(results);
    return VS_OK;
}

static int select_sources(vs_capture_engine *engine, const char *session_handle,
                          uint32_t source_types, uint32_t cursor_mode,
                          const char *restore_token, bool persist)
{
    char *token = fresh_token("ss");
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token));
    g_variant_builder_add(&options, "{sv}", "types", g_variant_new_uint32(source_types));
    g_variant_builder_add(&options, "{sv}", "multiple", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&options, "{sv}", "cursor_mode", g_variant_new_uint32(cursor_mode));
    if (engine->screencast_version >= 4) {
        g_variant_builder_add(&options, "{sv}", "persist_mode",
                              g_variant_new_uint32(persist ? 2u : 0u));
        if (restore_token && restore_token[0])
            g_variant_builder_add(&options, "{sv}", "restore_token",
                                  g_variant_new_string(restore_token));
    }

    guint32 response = 2;
    GVariant *results = portal_call(engine, SCREENCAST_IFACE, "SelectSources",
                                    g_variant_new("(oa{sv})", session_handle, &options),
                                    token, 120, &response);
    g_free(token);
    if (results) g_variant_unref(results);
    if (response != 0) {
        VS_LOG("SelectSources refused, response=%u types=%u", response, source_types);
        /* 1 is "the user said no"; anything else is the backend failing. */
        return response == 1 ? VS_ERR_UNSUPPORTED : VS_ERR_BACKEND;
    }
    return VS_OK;
}

static int start_session(vs_capture_engine *engine, vs_portal_session *session)
{
    char *token = fresh_token("st");
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token));

    guint32 response = 2;
    GVariant *results = portal_call(engine, SCREENCAST_IFACE, "Start",
                                    g_variant_new("(osa{sv})", session->session_handle,
                                                  "", &options),
                                    token, 120, &response);
    g_free(token);
    if (response != 0 || !results) {
        /* The WP-02 spike's defect 1: on a wlroots session with no DMA-BUF
         * capable renderer, stock xdpw 0.7.1 fails every Start with no
         * explanation. The caller turns this into "this session cannot
         * screencast", not a generic error. */
        VS_LOG("Start response=%u -- this session cannot screencast", response);
        if (results) g_variant_unref(results);
        return VS_ERR_BACKEND;
    }

    GVariant *token_value = g_variant_lookup_value(results, "restore_token",
                                                   G_VARIANT_TYPE_STRING);
    if (token_value) {
        session->restore_token = g_strdup(g_variant_get_string(token_value, NULL));
        g_variant_unref(token_value);
    }

    GVariant *streams = g_variant_lookup_value(results, "streams",
                                               G_VARIANT_TYPE("a(ua{sv})"));
    if (!streams || g_variant_n_children(streams) == 0) {
        VS_LOG("Start returned no streams");
        if (streams) g_variant_unref(streams);
        g_variant_unref(results);
        return VS_ERR_BACKEND;
    }
    GVariant *first = g_variant_get_child_value(streams, 0);
    GVariant *props = NULL;
    g_variant_get(first, "(u@a{sv})", &session->node_id, &props);

    /* The stream's own source_type, not the one that was requested. */
    session->granted_source_type = VS_CAPTURE_SOURCE_MONITOR;
    GVariant *type_value = g_variant_lookup_value(props, "source_type",
                                                  G_VARIANT_TYPE_UINT32);
    if (type_value) {
        session->granted_source_type = g_variant_get_uint32(type_value);
        g_variant_unref(type_value);
    }
    GVariant *size_value = g_variant_lookup_value(props, "size", G_VARIANT_TYPE("(ii)"));
    if (size_value) {
        g_variant_get(size_value, "(ii)", &session->width, &session->height);
        session->size_valid = session->width > 0 && session->height > 0;
        g_variant_unref(size_value);
    }
    if (g_variant_n_children(streams) > 1)
        VS_LOG("Start returned %u streams; using the first",
               (unsigned)g_variant_n_children(streams));

    g_variant_unref(props);
    g_variant_unref(first);
    g_variant_unref(streams);
    g_variant_unref(results);
    return VS_OK;
}

static int open_pipewire_remote(vs_capture_engine *engine, vs_portal_session *session)
{
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    GError *error = NULL;
    GUnixFDList *fds = NULL;
    GVariant *reply = g_dbus_connection_call_with_unix_fd_list_sync(
        engine->bus, PORTAL_BUS, PORTAL_PATH, SCREENCAST_IFACE, "OpenPipeWireRemote",
        g_variant_new("(oa{sv})", session->session_handle, &options),
        G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &fds, NULL, &error);
    if (!reply) {
        VS_LOG("OpenPipeWireRemote: %s", error->message);
        g_error_free(error);
        return VS_ERR_BACKEND;
    }
    gint32 index = -1;
    g_variant_get(reply, "(h)", &index);
    session->pw_fd = g_unix_fd_list_get(fds, index, &error);
    g_variant_unref(reply);
    g_object_unref(fds);
    if (session->pw_fd < 0) {
        VS_LOG("OpenPipeWireRemote gave no fd: %s", error ? error->message : "?");
        if (error) g_error_free(error);
        return VS_ERR_BACKEND;
    }
    return VS_OK;
}

int vs_portal_screencast_open(vs_capture_engine *engine, uint32_t source_types,
                              uint32_t cursor_mode, const char *restore_token,
                              bool persist, vs_portal_session *session_out)
{
    if (!engine || !session_out) return VS_ERR_INVALID;
    memset(session_out, 0, sizeof(*session_out));
    session_out->pw_fd = -1;
    session_out->requested_source_types = source_types;

    int rc = create_session(engine, &session_out->session_handle);
    if (rc != VS_OK) return rc;
    rc = select_sources(engine, session_out->session_handle, source_types, cursor_mode,
                        restore_token, persist);
    if (rc == VS_OK) rc = start_session(engine, session_out);
    if (rc == VS_OK) rc = open_pipewire_remote(engine, session_out);
    if (rc != VS_OK) {
        vs_portal_session_close(engine, session_out);
        return rc;
    }
    VS_LOG("screencast node=%u granted_source_type=%u size=%dx%d token=%s",
           session_out->node_id, session_out->granted_source_type,
           session_out->width, session_out->height,
           session_out->restore_token ? session_out->restore_token : "(none)");
    return VS_OK;
}

void vs_portal_session_close(vs_capture_engine *engine, vs_portal_session *session)
{
    if (!session) return;
    if (session->pw_fd >= 0) {
        close(session->pw_fd);
        session->pw_fd = -1;
    }
    if (session->session_handle) {
        /* The reply is an empty tuple and of no interest, but it is still a
         * GVariant this call owns a reference to: dropping it on the floor
         * leaks 64 bytes per recording. */
        GVariant *reply = g_dbus_connection_call_sync(
            engine->bus, PORTAL_BUS, session->session_handle,
            "org.freedesktop.portal.Session", "Close", NULL, NULL,
            G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
        if (reply) g_variant_unref(reply);
        g_free(session->session_handle);
        session->session_handle = NULL;
    }
    g_free(session->restore_token);
    session->restore_token = NULL;
}

int vs_portal_screenshot(vs_capture_engine *engine, bool include_cursor,
                         vs_capture_image *image_out)
{
    if (!engine || !image_out) return VS_ERR_INVALID;
    if (!engine->has_screenshot) return VS_ERR_UNSUPPORTED;

    char *token = fresh_token("shot");
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token));
    /* Non-interactive: the app has already chosen what to capture, and an
     * interactive portal dialog would replace our own selector. */
    g_variant_builder_add(&options, "{sv}", "interactive", g_variant_new_boolean(FALSE));
    /* Only honoured where the backend implements it; harmless elsewhere. */
    g_variant_builder_add(&options, "{sv}", "include_pointer",
                          g_variant_new_boolean(include_cursor));

    guint32 response = 2;
    GVariant *results = portal_call(engine, SCREENSHOT_IFACE, "Screenshot",
                                    g_variant_new("(sa{sv})", "", &options), token, 60,
                                    &response);
    g_free(token);
    if (response != 0 || !results) {
        VS_LOG("Screenshot response=%u", response);
        if (results) g_variant_unref(results);
        return response == 1 ? VS_ERR_UNSUPPORTED : VS_ERR_BACKEND;
    }
    GVariant *uri_value = g_variant_lookup_value(results, "uri", G_VARIANT_TYPE_STRING);
    if (!uri_value) {
        g_variant_unref(results);
        return VS_ERR_BACKEND;
    }
    const char *uri = g_variant_get_string(uri_value, NULL);
    GFile *file = g_file_new_for_uri(uri);
    char *path = g_file_get_path(file);
    int rc = VS_ERR_BACKEND;
    if (path) {
        rc = vs_capture_png_read(path, image_out);
        if (rc != VS_OK) VS_LOG("Screenshot %s could not be decoded (%s)", uri,
                                vs_result_string(rc));
    } else {
        VS_LOG("Screenshot returned a non-local uri: %s", uri);
    }
    g_free(path);
    g_object_unref(file);
    g_variant_unref(uri_value);
    g_variant_unref(results);
    return rc;
}
