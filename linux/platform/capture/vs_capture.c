/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * Engine lifecycle, capability probing, pixel helpers and the screenshot path.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <spa/param/video/format-utils.h>

#include "vs_capture_internal.h"

/* ------------------------------------------------------------------ log */

bool vs_capture_log_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv(VS_CAPTURE_LOG_ENV);
        enabled = (value && value[0] && strcmp(value, "0") != 0) ? 1 : 0;
    }
    return enabled == 1;
}

void vs_capture_logv(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fputs("[vs-capture] ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

int64_t vs_capture_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* --------------------------------------------------------------- pixels */

uint32_t vs_capture_pixel_bytes(vs_capture_pixel_format format)
{
    switch (format) {
    case VS_CAPTURE_PIXEL_BGRX:
    case VS_CAPTURE_PIXEL_BGRA:
    case VS_CAPTURE_PIXEL_RGBX:
    case VS_CAPTURE_PIXEL_RGBA:
        return 4;
    case VS_CAPTURE_PIXEL_UNKNOWN:
    default:
        return 0;
    }
}

const char *vs_capture_pixel_format_name(vs_capture_pixel_format format)
{
    switch (format) {
    case VS_CAPTURE_PIXEL_BGRX: return "BGRx";
    case VS_CAPTURE_PIXEL_BGRA: return "BGRA";
    case VS_CAPTURE_PIXEL_RGBX: return "RGBx";
    case VS_CAPTURE_PIXEL_RGBA: return "RGBA";
    case VS_CAPTURE_PIXEL_UNKNOWN:
    default: return "unknown";
    }
}

vs_capture_pixel_format vs_capture_format_from_spa(uint32_t spa_format)
{
    switch (spa_format) {
    case SPA_VIDEO_FORMAT_BGRx: return VS_CAPTURE_PIXEL_BGRX;
    case SPA_VIDEO_FORMAT_BGRA: return VS_CAPTURE_PIXEL_BGRA;
    case SPA_VIDEO_FORMAT_RGBx: return VS_CAPTURE_PIXEL_RGBX;
    case SPA_VIDEO_FORMAT_RGBA: return VS_CAPTURE_PIXEL_RGBA;
    default: return VS_CAPTURE_PIXEL_UNKNOWN;
    }
}

void vs_capture_image_free(vs_capture_image *image)
{
    if (!image) return;
    free(image->data);
    memset(image, 0, sizeof(*image));
}

int vs_capture_image_from_frame(const uint8_t *data, uint32_t width, uint32_t height,
                                uint32_t stride, vs_capture_pixel_format format,
                                const vs_rect *region, vs_capture_image *image_out)
{
    if (!data || !image_out) return VS_ERR_INVALID;
    uint32_t bytes = vs_capture_pixel_bytes(format);
    if (bytes == 0 || width == 0 || height == 0) return VS_ERR_INVALID;

    vs_rect crop = { 0, 0, (int32_t)width, (int32_t)height };
    if (region) {
        crop = *region;
        if (!vs_capture_clamp_region(&crop, width, height)) return VS_ERR_INVALID;
    }
    uint32_t out_stride = (uint32_t)crop.width * bytes;
    uint8_t *pixels = malloc((size_t)out_stride * (size_t)crop.height);
    if (!pixels) return VS_ERR_NO_MEM;
    for (int32_t y = 0; y < crop.height; y++) {
        memcpy(pixels + (size_t)out_stride * (size_t)y,
               data + (size_t)stride * (size_t)(crop.y + y) + (size_t)crop.x * bytes,
               out_stride);
    }
    image_out->data = pixels;
    image_out->width = (uint32_t)crop.width;
    image_out->height = (uint32_t)crop.height;
    image_out->stride = out_stride;
    image_out->format = format;
    image_out->byte_length = (size_t)out_stride * (size_t)crop.height;
    return VS_OK;
}

/* --------------------------------------------------------------- engine */

static const char *const backend_names[] = { "portal", NULL };

const char *const *vs_capture_backend_names(void)
{
    return backend_names;
}

vs_capture_engine *vs_capture_engine_create(const char *preferred, int *result_out)
{
    if (preferred && strcmp(preferred, "portal") != 0) {
        VS_LOG("no capture backend named '%s'", preferred);
        if (result_out) *result_out = VS_ERR_NO_BACKEND;
        return NULL;
    }
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!bus) {
        VS_LOG("no session bus: %s", error->message);
        g_error_free(error);
        if (result_out) *result_out = VS_ERR_NO_BACKEND;
        return NULL;
    }

    vs_capture_engine *engine = calloc(1, sizeof(*engine));
    if (!engine) {
        g_object_unref(bus);
        if (result_out) *result_out = VS_ERR_NO_MEM;
        return NULL;
    }
    engine->bus = bus;

    uint32_t version = 0;
    if (vs_portal_get_uint(engine, "org.freedesktop.portal.ScreenCast", "version",
                           &version) != VS_OK) {
        /* Without ScreenCast there is no capture at all: the Screenshot portal
         * alone cannot record, and on the sessions that have neither there is
         * nothing for this engine to wrap. */
        VS_LOG("org.freedesktop.portal.ScreenCast is absent; no capture backend");
        vs_capture_engine_destroy(engine);
        if (result_out) *result_out = VS_ERR_NO_BACKEND;
        return NULL;
    }
    engine->screencast_version = version;
    vs_portal_get_uint(engine, "org.freedesktop.portal.ScreenCast",
                       "AvailableSourceTypes", &engine->source_types);
    vs_portal_get_uint(engine, "org.freedesktop.portal.ScreenCast",
                       "AvailableCursorModes", &engine->cursor_modes);

    uint32_t screenshot_version = 0;
    engine->has_screenshot = vs_portal_get_uint(engine, "org.freedesktop.portal.Screenshot",
                                                "version", &screenshot_version) == VS_OK;

    if (engine->has_screenshot) engine->capabilities |= VS_CAPTURE_HAS_SCREENSHOT_PORTAL;
    if (engine->source_types & VS_CAPTURE_SOURCE_WINDOW)
        engine->capabilities |= VS_CAPTURE_HAS_WINDOW_SOURCE;
    if (engine->source_types & VS_CAPTURE_SOURCE_VIRTUAL)
        engine->capabilities |= VS_CAPTURE_HAS_VIRTUAL_SOURCE;
    if (engine->cursor_modes & VS_CAPTURE_CURSOR_EMBEDDED)
        engine->capabilities |= VS_CAPTURE_HAS_CURSOR_EMBEDDED;
    /* persist_mode and restore_token arrived in ScreenCast version 4. */
    if (engine->screencast_version >= 4) engine->capabilities |= VS_CAPTURE_HAS_RESTORE_TOKEN;
    /* VS_CAPTURE_HAS_REGION is never set: no portal backend has a region
     * source. The engine crops instead, which is a different claim. */

    pw_init(NULL, NULL);
    engine->pw_initialised = true;
    engine->capabilities |= vs_capture_probe_audio();

    VS_LOG("engine up: ScreenCast v%u Screenshot=%s source_types=%u cursor_modes=%u "
           "capabilities=0x%02x", engine->screencast_version,
           engine->has_screenshot ? "yes" : "no", engine->source_types,
           engine->cursor_modes, engine->capabilities);
    if (result_out) *result_out = VS_OK;
    return engine;
}

void vs_capture_engine_destroy(vs_capture_engine *engine)
{
    if (!engine) return;
    if (engine->bus) g_object_unref(engine->bus);
    if (engine->pw_initialised) pw_deinit();
    free(engine);
}

const char *vs_capture_engine_name(const vs_capture_engine *engine)
{
    return engine ? "portal" : "";
}

uint32_t vs_capture_engine_capabilities(const vs_capture_engine *engine)
{
    return engine ? engine->capabilities : 0;
}

uint32_t vs_capture_engine_source_types(const vs_capture_engine *engine)
{
    return engine ? engine->source_types : 0;
}

uint32_t vs_capture_engine_cursor_modes(const vs_capture_engine *engine)
{
    return engine ? engine->cursor_modes : 0;
}

/* ----------------------------------------------------------- screenshot */

/*
 * One frame out of a ScreenCast session. This is the path a bare wlroots
 * session takes for every screenshot, because xdg-desktop-portal-wlr
 * implements no `org.freedesktop.impl.portal.Access` and the frontend refuses
 * to export the Screenshot portal without one (WP-02, section 3). It is a real
 * path, not a degraded one: the pixels come off the same `zwlr_screencopy`
 * buffer the Screenshot portal would have used.
 */
static int screenshot_via_screencast(vs_capture_engine *engine,
                                     const vs_capture_shot_request *request,
                                     vs_capture_image *image_out, char **token_out)
{
    vs_capture_stream_config config = { 0 };
    config.source_types = request->source_types ? request->source_types
                                                : VS_CAPTURE_SOURCE_MONITOR;
    config.cursor_mode = request->include_cursor ? VS_CAPTURE_CURSOR_EMBEDDED
                                                 : VS_CAPTURE_CURSOR_HIDDEN;
    config.restore_token = request->restore_token;
    config.persist = request->persist;
    config.queue_depth = 2;

    vs_capture_stream *stream = NULL;
    int rc = vs_capture_stream_start(engine, &config, &stream);
    if (rc != VS_OK) return rc;

    uint32_t timeout_ms = request->timeout_ms ? request->timeout_ms : 5000;
    vs_capture_frame frame;
    rc = vs_capture_stream_next_frame(stream, &frame, (int)timeout_ms);
    if (rc == VS_OK) {
        rc = vs_capture_image_from_frame(frame.data, frame.width, frame.height,
                                         frame.stride, frame.format,
                                         request->region_enabled ? &request->region : NULL,
                                         image_out);
        vs_capture_stream_release_frame(stream);
    } else {
        VS_LOG("no frame within %u ms; a damage-driven session delivers nothing "
               "until something repaints", timeout_ms);
    }
    if (rc == VS_OK && token_out) {
        const char *token = vs_capture_stream_restore_token(stream);
        *token_out = token ? strdup(token) : NULL;
    }
    vs_capture_stream_stop(stream);
    return rc;
}

int vs_capture_screenshot(vs_capture_engine *engine,
                          const vs_capture_shot_request *request,
                          vs_capture_image *image_out,
                          vs_capture_shot_method *method_used_out,
                          char **token_out)
{
    if (!engine || !request || !image_out) return VS_ERR_INVALID;
    memset(image_out, 0, sizeof(*image_out));
    if (token_out) *token_out = NULL;

    bool try_portal = request->method != VS_CAPTURE_SHOT_SCREENCAST && engine->has_screenshot;
    if (request->method == VS_CAPTURE_SHOT_PORTAL && !engine->has_screenshot) {
        VS_LOG("the Screenshot portal was asked for by name and is not present");
        return VS_ERR_UNSUPPORTED;
    }

    if (try_portal) {
        int rc = vs_portal_screenshot(engine, request->include_cursor, image_out);
        if (rc == VS_OK && request->region_enabled) {
            vs_capture_image cropped;
            rc = vs_capture_image_from_frame(image_out->data, image_out->width,
                                             image_out->height, image_out->stride,
                                             image_out->format, &request->region,
                                             &cropped);
            vs_capture_image_free(image_out);
            if (rc == VS_OK) *image_out = cropped;
        }
        if (rc == VS_OK) {
            if (method_used_out) *method_used_out = VS_CAPTURE_SHOT_PORTAL;
            return VS_OK;
        }
        if (request->method == VS_CAPTURE_SHOT_PORTAL) return rc;
        VS_LOG("Screenshot portal did not answer (%s); falling back to a "
               "one-frame ScreenCast", vs_result_string(rc));
    }

    int rc = screenshot_via_screencast(engine, request, image_out, token_out);
    if (rc == VS_OK && method_used_out) *method_used_out = VS_CAPTURE_SHOT_SCREENCAST;
    return rc;
}
