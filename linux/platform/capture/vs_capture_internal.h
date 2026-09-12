/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * Internals shared between the engine, the portal choreography and the
 * PipeWire streams. Nothing here is part of the contract in
 * ../include/vorssaint_platform.h.
 */

#ifndef VS_CAPTURE_INTERNAL_H
#define VS_CAPTURE_INTERNAL_H

#include <gio/gio.h>
#include <pipewire/pipewire.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "vorssaint_platform.h"

#define VS_CAPTURE_LOG_ENV "VS_CAPTURE_DEBUG"

void vs_capture_logv(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool vs_capture_log_enabled(void);

#define VS_LOG(...)                                                            \
    do {                                                                       \
        if (vs_capture_log_enabled()) vs_capture_logv(__VA_ARGS__);            \
    } while (0)

int64_t vs_capture_now_ns(void);

/* --------------------------------------------------------------- engine */

struct vs_capture_engine {
    GDBusConnection *bus;
    uint32_t capabilities;
    uint32_t source_types;
    uint32_t cursor_modes;
    uint32_t screencast_version;
    bool has_screenshot;
    /* PipeWire is initialised once per process; the engine owns the count. */
    bool pw_initialised;
};

/* ----------------------------------------------------------- portal calls */

/** One negotiated ScreenCast session: the D-Bus session handle, the PipeWire
 *  fd and node, plus what the portal actually granted. */
typedef struct vs_portal_session {
    char *session_handle;
    char *restore_token;
    int pw_fd;
    uint32_t node_id;
    uint32_t granted_source_type;
    uint32_t requested_source_types;
    int32_t width;
    int32_t height;
    bool size_valid;
} vs_portal_session;

int vs_portal_screencast_open(vs_capture_engine *engine,
                              uint32_t source_types, uint32_t cursor_mode,
                              const char *restore_token, bool persist,
                              vs_portal_session *session_out);
void vs_portal_session_close(vs_capture_engine *engine, vs_portal_session *session);

/** `org.freedesktop.portal.Screenshot.Screenshot` with `interactive=false`,
 *  decoded into an image. VS_ERR_UNSUPPORTED when the interface is absent. */
int vs_portal_screenshot(vs_capture_engine *engine, bool include_cursor,
                         vs_capture_image *image_out);

/** Read a portal property; returns VS_ERR_UNSUPPORTED when absent. */
int vs_portal_get_uint(vs_capture_engine *engine, const char *iface,
                       const char *property, uint32_t *value_out);

/** Sweep the user's own PipeWire daemon once and return the audio bits of
 *  `vs_capture_capability` it justifies. */
uint32_t vs_capture_probe_audio(void);

/* ------------------------------------------------------------ pixel utils */

vs_capture_pixel_format vs_capture_format_from_spa(uint32_t spa_format);

/** Read an 8-bit non-interlaced RGB or RGBA PNG into an RGBA image. Anything
 *  else -- 16-bit, palette, greyscale, Adam7 -- returns VS_ERR_UNSUPPORTED
 *  rather than a wrong picture; the caller then falls back to the ScreenCast
 *  path, which produces pixels directly. Every portal backend the port targets
 *  writes one of the two supported forms. */
int vs_capture_png_read(const char *path, vs_capture_image *image_out);

/* ----------------------------------------------------------------- stream */

typedef struct vs_frame_slot {
    uint8_t *data;
    size_t capacity;
    vs_capture_frame frame;
} vs_frame_slot;

typedef struct vs_audio_slot {
    float *samples;
    size_t capacity_floats;
    vs_capture_audio_buffer buffer;
} vs_audio_slot;

struct vs_capture_stream {
    vs_capture_engine *engine;
    vs_portal_session session;
    vs_capture_stream_config config;

    struct pw_thread_loop *loop;
    struct pw_context *context;
    struct pw_core *core;        /* the portal's restricted fd */
    struct pw_core *audio_core;  /* a second, ordinary connection */

    struct pw_stream *video;
    struct spa_hook video_hook;
    /* One per audio kind, so the two share the event table and still know
     * which of them they are. */
    struct vs_audio_source {
        struct vs_capture_stream *stream;
        vs_capture_audio_kind kind;
        struct pw_stream *pw;
        struct spa_hook hook;
        uint32_t rate;
        uint32_t channels;
        bool have_format;
    } system_audio, microphone;

    uint32_t width;
    uint32_t height;
    vs_capture_pixel_format format;
    bool have_format;

    /* Guards the clock, the queues and the stats. Held briefly and never
     * across a user callback. */
    pthread_mutex_t lock;
    pthread_cond_t frame_ready;
    pthread_cond_t audio_ready;

    vs_capture_clock clock;
    bool started;
    bool stopping;

    int64_t min_frame_interval_ns;
    int64_t last_delivered_pts_ns;

    vs_capture_frame_cb frame_cb;
    void *frame_cb_user;
    vs_capture_audio_cb audio_cb;
    void *audio_cb_user;

    vs_frame_slot *frames;
    uint32_t frame_capacity;
    uint32_t frame_head;
    uint32_t frame_count;
    bool frame_held;

    vs_audio_slot *audio;
    uint32_t audio_capacity;
    uint32_t audio_head;
    uint32_t audio_count;
    bool audio_held;

    uint64_t frame_sequence;
    uint64_t audio_sequence;
    vs_capture_stats stats;
    double latency_sum_ms;
};

#endif /* VS_CAPTURE_INTERNAL_H */
