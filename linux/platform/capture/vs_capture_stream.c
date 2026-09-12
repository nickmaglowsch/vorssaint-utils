/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * The PipeWire side: one video stream on the fd the portal handed back, and up
 * to two audio streams on a second, ordinary connection.
 *
 * Two decisions here come straight out of the WP-02 spike and are not
 * negotiable:
 *
 *   - Audio never uses the portal's fd. `OpenPipeWireRemote` returns a
 *     *restricted* connection whose permissions expose only the screencast
 *     node; an audio stream created on it negotiates no format and sits in
 *     `paused` forever. A second `pw_context_connect()` to the user's own
 *     daemon is both what works and what is correct, since system-audio capture
 *     is not a portal-mediated permission on any target desktop.
 *   - Timestamps come from `spa_meta_header.pts`, never from a frame counter.
 *     wlroots delivers frames on damage, so the rate varies between zero and
 *     the compositor's cap within a single recording.
 *
 * Delivery keeps the contract in linux/platform/README.md: buffers are copied
 * into a small ring, an eventfd is made readable, and the caller's callbacks
 * run only inside `vs_capture_stream_dispatch`, on the caller's own thread.
 * `vs_capture_stream_next_frame` drains the same ring for a caller that wants
 * the buffer rather than a callback -- the screenshot path and the tests.
 *
 * `direct_callbacks` is the documented exception, and exists for one consumer:
 * WP-B5's encoder wants the mapped buffer with no copy at all, and is willing
 * to run on the PipeWire thread to get it. Nothing else should use it.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <spa/param/audio/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/utils/result.h>

#include "vs_capture_internal.h"

#define DEFAULT_QUEUE_DEPTH 4
#define DEFAULT_AUDIO_RATE 48000
#define DEFAULT_AUDIO_CHANNELS 2

/* Called with the lock held, right after something lands in a queue. The
 * counter is only ever cleared at the top of dispatch, so an item queued during
 * a dispatch still leaves the descriptor readable afterwards. */
static void notify(vs_capture_stream *stream)
{
    if (stream->event_fd < 0) return;
    uint64_t one = 1;
    ssize_t written = write(stream->event_fd, &one, sizeof(one));
    (void)written;   /* EAGAIN only at 2^64 pending, and the count is a hint */
}

static void timespec_in(struct timespec *ts, int timeout_ms)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += timeout_ms / 1000;
    ts->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec += 1;
    }
}

/* --------------------------------------------------------------- video */

static void on_video_state(void *data, enum pw_stream_state old,
                           enum pw_stream_state state, const char *error)
{
    (void)data; (void)old;
    VS_LOG("video stream: %s%s%s", pw_stream_state_as_string(state),
           error ? " -- " : "", error ? error : "");
}

static void on_video_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    vs_capture_stream *stream = data;
    if (!param || id != SPA_PARAM_Format) return;

    uint32_t media_type, media_subtype;
    if (spa_format_parse(param, &media_type, &media_subtype) < 0) return;
    if (media_type != SPA_MEDIA_TYPE_video || media_subtype != SPA_MEDIA_SUBTYPE_raw) return;

    struct spa_video_info_raw info;
    spa_zero(info);
    if (spa_format_video_raw_parse(param, &info) < 0) return;

    pthread_mutex_lock(&stream->lock);
    stream->width = info.size.width;
    stream->height = info.size.height;
    stream->format = vs_capture_format_from_spa(info.format);
    stream->have_format = stream->format != VS_CAPTURE_PIXEL_UNKNOWN;
    pthread_mutex_unlock(&stream->lock);
    VS_LOG("negotiated video: %s %ux%u", vs_capture_pixel_format_name(stream->format),
           info.size.width, info.size.height);

    /* The header meta is what makes a real timeline possible; ask for it
     * explicitly, and for a few buffers so a slow consumer does not starve the
     * compositor's side of the queue. */
    uint8_t buffer[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[2];
    params[0] = spa_pod_builder_add_object(&builder,
        SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
    params[1] = spa_pod_builder_add_object(&builder,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16));
    pw_stream_update_params(stream->video, params, 2);
}

/* Enqueue a copy for the pull API. Called with the lock held. */
static void queue_frame(vs_capture_stream *stream, const vs_capture_frame *frame)
{
    if (stream->frame_count == stream->frame_capacity) {
        stream->stats.frames_dropped_queue++;
        return;
    }
    uint32_t index = (stream->frame_head + stream->frame_count) % stream->frame_capacity;
    vs_frame_slot *slot = &stream->frames[index];
    size_t needed = (size_t)frame->stride * frame->height;
    if (slot->capacity < needed) {
        uint8_t *grown = realloc(slot->data, needed);
        if (!grown) {
            stream->stats.frames_dropped_queue++;
            return;
        }
        slot->data = grown;
        slot->capacity = needed;
    }
    memcpy(slot->data, frame->data, needed);
    slot->frame = *frame;
    slot->frame.data = slot->data;
    stream->frame_count++;
    notify(stream);
    pthread_cond_signal(&stream->frame_ready);
}

static void on_video_process(void *data)
{
    vs_capture_stream *stream = data;
    struct pw_buffer *pw_buffer = pw_stream_dequeue_buffer(stream->video);
    if (!pw_buffer) {
        pthread_mutex_lock(&stream->lock);
        stream->stats.buffers_missed++;
        pthread_mutex_unlock(&stream->lock);
        return;
    }
    struct spa_buffer *buffer = pw_buffer->buffer;
    if (!buffer->datas[0].data || buffer->datas[0].chunk->size == 0) {
        pw_stream_queue_buffer(stream->video, pw_buffer);
        return;
    }
    int64_t arrival_ns = vs_capture_now_ns();

    struct spa_meta_header *header =
        spa_buffer_find_meta_data(buffer, SPA_META_Header, sizeof(*header));
    bool has_pts = header && header->pts > 0;
    int64_t capture_ns = has_pts ? header->pts : arrival_ns;

    uint32_t stride = (uint32_t)buffer->datas[0].chunk->stride;
    if (stride == 0) stride = stream->width * vs_capture_pixel_bytes(stream->format);

    vs_capture_frame frame = { 0 };
    vs_capture_frame_cb callback = NULL;
    void *callback_user = NULL;
    bool deliver = false;

    pthread_mutex_lock(&stream->lock);
    if (stream->stopping || !stream->have_format) {
        pthread_mutex_unlock(&stream->lock);
        pw_stream_queue_buffer(stream->video, pw_buffer);
        return;
    }
    if (stream->clock.paused) {
        stream->stats.frames_dropped_paused++;
        pthread_mutex_unlock(&stream->lock);
        pw_stream_queue_buffer(stream->video, pw_buffer);
        return;
    }
    if (stream->min_frame_interval_ns > 0 && stream->last_delivered_pts_ns != 0 &&
        capture_ns - stream->last_delivered_pts_ns < stream->min_frame_interval_ns) {
        stream->stats.frames_dropped_rate++;
        pthread_mutex_unlock(&stream->lock);
        pw_stream_queue_buffer(stream->video, pw_buffer);
        return;
    }
    stream->last_delivered_pts_ns = capture_ns;

    /* The crop is a pointer offset, not a copy: the region is a rectangle in a
     * packed buffer, so the same bytes are already the answer at a different
     * origin and stride. */
    uint32_t bytes = vs_capture_pixel_bytes(stream->format);
    vs_rect crop = { 0, 0, (int32_t)stream->width, (int32_t)stream->height };
    if (stream->config.region_enabled) {
        crop = stream->config.region;
        if (!vs_capture_clamp_region(&crop, stream->width, stream->height)) {
            stream->stats.frames_dropped_rate++;
            pthread_mutex_unlock(&stream->lock);
            pw_stream_queue_buffer(stream->video, pw_buffer);
            return;
        }
    }
    frame.data = (const uint8_t *)buffer->datas[0].data + buffer->datas[0].chunk->offset +
                 (size_t)stride * (size_t)crop.y + (size_t)crop.x * bytes;
    frame.width = (uint32_t)crop.width;
    frame.height = (uint32_t)crop.height;
    frame.stride = stride;
    frame.format = stream->format;
    frame.pts_ns = capture_ns;
    frame.has_pts = has_pts;
    frame.arrival_ns = arrival_ns;
    frame.timeline_ns = vs_capture_clock_timeline(&stream->clock, capture_ns);
    frame.sequence = stream->frame_sequence++;

    stream->stats.frames_delivered++;
    if (has_pts) {
        stream->stats.frames_with_pts++;
        double latency_ms = (double)(arrival_ns - capture_ns) / 1e6;
        stream->latency_sum_ms += latency_ms;
        if (stream->stats.frames_with_pts == 1) {
            stream->stats.latency_min_ms = latency_ms;
            stream->stats.latency_max_ms = latency_ms;
        } else {
            if (latency_ms < stream->stats.latency_min_ms)
                stream->stats.latency_min_ms = latency_ms;
            if (latency_ms > stream->stats.latency_max_ms)
                stream->stats.latency_max_ms = latency_ms;
        }
        stream->stats.latency_avg_ms =
            stream->latency_sum_ms / (double)stream->stats.frames_with_pts;
    }
    if (frame.sequence == 0) stream->stats.video_timeline_first_ns = frame.timeline_ns;
    stream->stats.video_timeline_last_ns = frame.timeline_ns;

    if (stream->config.direct_callbacks && stream->frame_cb) {
        callback = stream->frame_cb;
        callback_user = stream->frame_cb_user;
        deliver = true;
    } else {
        queue_frame(stream, &frame);
    }
    pthread_mutex_unlock(&stream->lock);

    /* Outside the lock: the callback is the consumer's code and may call back
     * into this stream (pause, stats) without deadlocking. The buffer is still
     * ours until it is requeued below, so `frame.data` stays valid. This only
     * happens under `direct_callbacks`; otherwise the frame was copied into the
     * queue above and nothing of the caller's runs on this thread. */
    if (deliver) callback(&frame, callback_user);
    pw_stream_queue_buffer(stream->video, pw_buffer);
}

static const struct pw_stream_events video_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_video_state,
    .param_changed = on_video_param_changed,
    .process = on_video_process,
};

/* --------------------------------------------------------------- audio */

static void on_audio_state(void *data, enum pw_stream_state old,
                           enum pw_stream_state state, const char *error)
{
    (void)old;
    struct vs_audio_source *source = data;
    VS_LOG("%s audio stream: %s%s%s",
           source->kind == VS_CAPTURE_AUDIO_SYSTEM ? "system" : "microphone",
           pw_stream_state_as_string(state), error ? " -- " : "", error ? error : "");
}

static void on_audio_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    struct vs_audio_source *source = data;
    if (!param || id != SPA_PARAM_Format) return;
    uint32_t media_type, media_subtype;
    if (spa_format_parse(param, &media_type, &media_subtype) < 0) return;
    if (media_type != SPA_MEDIA_TYPE_audio || media_subtype != SPA_MEDIA_SUBTYPE_raw) return;

    struct spa_audio_info_raw info;
    spa_zero(info);
    if (spa_format_audio_raw_parse(param, &info) < 0) return;
    source->rate = info.rate;
    source->channels = info.channels;
    source->have_format = info.rate > 0 && info.channels > 0;
    VS_LOG("negotiated %s audio: %u Hz, %u channels",
           source->kind == VS_CAPTURE_AUDIO_SYSTEM ? "system" : "microphone",
           info.rate, info.channels);

    uint8_t buffer[512];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&builder,
        SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
    pw_stream_update_params(source->pw, params, 1);
}

/* Called with the lock held. */
static void queue_audio(vs_capture_stream *stream, const vs_capture_audio_buffer *buffer)
{
    if (stream->audio_count == stream->audio_capacity) {
        stream->stats.audio_dropped_queue++;
        return;
    }
    uint32_t index = (stream->audio_head + stream->audio_count) % stream->audio_capacity;
    vs_audio_slot *slot = &stream->audio[index];
    size_t floats = (size_t)buffer->frame_count * buffer->channels;
    if (slot->capacity_floats < floats) {
        float *grown = realloc(slot->samples, floats * sizeof(float));
        if (!grown) {
            stream->stats.audio_dropped_queue++;
            return;
        }
        slot->samples = grown;
        slot->capacity_floats = floats;
    }
    memcpy(slot->samples, buffer->samples, floats * sizeof(float));
    slot->buffer = *buffer;
    slot->buffer.samples = slot->samples;
    stream->audio_count++;
    notify(stream);
    pthread_cond_signal(&stream->audio_ready);
}

static void on_audio_process(void *data)
{
    struct vs_audio_source *source = data;
    vs_capture_stream *stream = source->stream;
    struct pw_buffer *pw_buffer = pw_stream_dequeue_buffer(source->pw);
    if (!pw_buffer) return;
    struct spa_buffer *buffer = pw_buffer->buffer;
    if (!buffer->datas[0].data || buffer->datas[0].chunk->size == 0 ||
        !source->have_format) {
        pw_stream_queue_buffer(source->pw, pw_buffer);
        return;
    }
    int64_t arrival_ns = vs_capture_now_ns();
    struct spa_meta_header *header =
        spa_buffer_find_meta_data(buffer, SPA_META_Header, sizeof(*header));
    bool has_pts = header && header->pts > 0;
    int64_t capture_ns = has_pts ? header->pts : arrival_ns;

    const float *samples = (const float *)((const uint8_t *)buffer->datas[0].data +
                                           buffer->datas[0].chunk->offset);
    uint32_t frame_count =
        buffer->datas[0].chunk->size / (uint32_t)(sizeof(float) * source->channels);

    vs_capture_audio_buffer out = { 0 };
    vs_capture_audio_cb callback = NULL;
    void *callback_user = NULL;

    pthread_mutex_lock(&stream->lock);
    if (stream->stopping) {
        pthread_mutex_unlock(&stream->lock);
        pw_stream_queue_buffer(source->pw, pw_buffer);
        return;
    }
    if (stream->clock.paused) {
        stream->stats.audio_dropped_paused++;
        pthread_mutex_unlock(&stream->lock);
        pw_stream_queue_buffer(source->pw, pw_buffer);
        return;
    }
    out.kind = source->kind;
    out.samples = samples;
    out.frame_count = frame_count;
    out.rate = source->rate;
    out.channels = source->channels;
    out.pts_ns = capture_ns;
    out.has_pts = has_pts;
    out.arrival_ns = arrival_ns;
    /* The same clock, and therefore the same pause gap, as the frames. */
    out.timeline_ns = vs_capture_clock_timeline(&stream->clock, capture_ns);
    out.sequence = stream->audio_sequence++;

    if (source->kind == VS_CAPTURE_AUDIO_SYSTEM) {
        stream->stats.audio_buffers_system++;
        stream->stats.audio_frames_system += frame_count;
    } else {
        stream->stats.audio_buffers_microphone++;
        stream->stats.audio_frames_microphone += frame_count;
    }
    if (out.sequence == 0) stream->stats.audio_timeline_first_ns = out.timeline_ns;
    stream->stats.audio_timeline_last_ns = out.timeline_ns;

    if (stream->config.direct_callbacks && stream->audio_cb) {
        callback = stream->audio_cb;
        callback_user = stream->audio_cb_user;
    } else {
        queue_audio(stream, &out);
    }
    pthread_mutex_unlock(&stream->lock);

    if (callback) callback(&out, callback_user);
    pw_stream_queue_buffer(source->pw, pw_buffer);
}

static const struct pw_stream_events audio_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_audio_state,
    .param_changed = on_audio_param_changed,
    .process = on_audio_process,
};

static int connect_audio(vs_capture_stream *stream, struct vs_audio_source *source,
                         vs_capture_audio_kind kind, const char *target)
{
    source->stream = stream;
    source->kind = kind;
    source->rate = stream->config.audio_rate ? stream->config.audio_rate
                                             : DEFAULT_AUDIO_RATE;
    source->channels = stream->config.audio_channels ? stream->config.audio_channels
                                                     : DEFAULT_AUDIO_CHANNELS;

    bool is_system = kind == VS_CAPTURE_AUDIO_SYSTEM;
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, is_system ? "Production" : "Communication",
        PW_KEY_NODE_NAME, is_system ? "vorssaint-system-audio" : "vorssaint-microphone",
        NULL);
    if (!props) return VS_ERR_NO_MEM;
    /* `stream.capture.sink` is what turns "connect me to a sink" into "connect
     * me to that sink's monitor"; with no target it follows the default sink,
     * which is what "record system audio" means to a user. */
    if (is_system) pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
    if (target && target[0]) pw_properties_set(props, PW_KEY_TARGET_OBJECT, target);

    source->pw = pw_stream_new(stream->audio_core,
                               is_system ? "vorssaint-system-audio" : "vorssaint-microphone",
                               props);
    if (!source->pw) return VS_ERR_BACKEND;
    pw_stream_add_listener(source->pw, &source->hook, &audio_events, source);

    uint8_t buffer[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    struct spa_audio_info_raw wanted = {
        .format = SPA_AUDIO_FORMAT_F32,
        .rate = source->rate,
        .channels = source->channels,
    };
    if (source->channels == 2) {
        wanted.position[0] = SPA_AUDIO_CHANNEL_FL;
        wanted.position[1] = SPA_AUDIO_CHANNEL_FR;
    } else if (source->channels == 1) {
        wanted.position[0] = SPA_AUDIO_CHANNEL_MONO;
    }
    const struct spa_pod *params[1] = {
        spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &wanted)
    };
    int rc = pw_stream_connect(source->pw, PW_DIRECTION_INPUT, PW_ID_ANY,
                               PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                               params, 1);
    if (rc < 0) {
        VS_LOG("pw_stream_connect(%s audio): %s",
               is_system ? "system" : "microphone", spa_strerror(rc));
        return VS_ERR_BACKEND;
    }
    return VS_OK;
}

/* --------------------------------------------------------------- start */

static void stream_free(vs_capture_stream *stream)
{
    if (!stream) return;
    if (stream->event_fd >= 0) close(stream->event_fd);
    for (uint32_t i = 0; i < stream->frame_capacity; i++) free(stream->frames[i].data);
    free(stream->frames);
    for (uint32_t i = 0; i < stream->audio_capacity; i++) free(stream->audio[i].samples);
    free(stream->audio);
    pthread_cond_destroy(&stream->frame_ready);
    pthread_cond_destroy(&stream->audio_ready);
    pthread_mutex_destroy(&stream->lock);
    free(stream);
}

int vs_capture_stream_start(vs_capture_engine *engine,
                            const vs_capture_stream_config *config,
                            vs_capture_stream **stream_out)
{
    if (!engine || !config || !stream_out) return VS_ERR_INVALID;
    *stream_out = NULL;

    vs_capture_stream *stream = calloc(1, sizeof(*stream));
    if (!stream) return VS_ERR_NO_MEM;
    stream->engine = engine;
    stream->config = *config;
    stream->session.pw_fd = -1;
    stream->event_fd = config->direct_callbacks
                           ? -1
                           : eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (!config->direct_callbacks && stream->event_fd < 0) {
        free(stream);
        return VS_ERR_NO_MEM;
    }

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutex_init(&stream->lock, &attr);
    pthread_mutexattr_destroy(&attr);
    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    /* The waits below are on CLOCK_MONOTONIC deadlines; a condvar left on the
     * default realtime clock would wait wrong across an NTP step. */
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
    pthread_cond_init(&stream->frame_ready, &cond_attr);
    pthread_cond_init(&stream->audio_ready, &cond_attr);
    pthread_condattr_destroy(&cond_attr);

    uint32_t depth = config->queue_depth ? config->queue_depth : DEFAULT_QUEUE_DEPTH;
    stream->frames = calloc(depth, sizeof(*stream->frames));
    stream->audio = calloc(depth, sizeof(*stream->audio));
    if (!stream->frames || !stream->audio) {
        stream_free(stream);
        return VS_ERR_NO_MEM;
    }
    stream->frame_capacity = depth;
    stream->audio_capacity = depth;
    if (config->max_fps > 0)
        stream->min_frame_interval_ns = 1000000000LL / (int64_t)config->max_fps;

    uint32_t source_types = config->source_types ? config->source_types
                                                 : VS_CAPTURE_SOURCE_MONITOR;
    uint32_t cursor_mode = config->cursor_mode ? config->cursor_mode
                                               : VS_CAPTURE_CURSOR_HIDDEN;
    int rc = vs_portal_screencast_open(engine, source_types, cursor_mode,
                                       config->restore_token, config->persist,
                                       &stream->session);
    if (rc != VS_OK) {
        stream_free(stream);
        return rc;
    }
    if (stream->session.granted_source_type &&
        !(stream->session.granted_source_type & source_types)) {
        /* The request was acknowledged and the session did something else: on
         * xdg-desktop-portal-wlr 0.7.1 a WINDOW request comes back as a whole
         * MONITOR (WP-02, section 7). A caller that offers "record this window"
         * sets require_source_type and gets a refusal, because recording the
         * whole screen instead is a privacy bug, not a degraded result. */
        VS_LOG("requested source types %u, granted %u", source_types,
               stream->session.granted_source_type);
        if (config->require_source_type) {
            vs_portal_session_close(engine, &stream->session);
            stream_free(stream);
            return VS_ERR_NOT_APPLIED;
        }
    }

    stream->loop = pw_thread_loop_new("vorssaint-capture", NULL);
    if (!stream->loop) {
        vs_portal_session_close(engine, &stream->session);
        stream_free(stream);
        return VS_ERR_BACKEND;
    }
    pw_thread_loop_lock(stream->loop);
    stream->context = pw_context_new(pw_thread_loop_get_loop(stream->loop), NULL, 0);
    if (!stream->context) goto fail_locked;

    int duplicated = fcntl(stream->session.pw_fd, F_DUPFD_CLOEXEC, 3);
    if (duplicated < 0) goto fail_locked;
    stream->core = pw_context_connect_fd(stream->context, duplicated, NULL, 0);
    if (!stream->core) {
        VS_LOG("pw_context_connect_fd: %s", strerror(errno));
        goto fail_locked;
    }

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        NULL);
    stream->video = pw_stream_new(stream->core, "vorssaint-screencast", props);
    if (!stream->video) goto fail_locked;
    pw_stream_add_listener(stream->video, &stream->video_hook, &video_events, stream);

    uint32_t nominal_fps = config->max_fps ? config->max_fps : 60;
    uint8_t buffer[2048];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&builder,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5,
            SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx,
            SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
            &SPA_RECTANGLE(1920, 1080), &SPA_RECTANGLE(16, 16),
            &SPA_RECTANGLE(16384, 16384)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
            &SPA_FRACTION(nominal_fps, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(240, 1)));

    int connect_rc = pw_stream_connect(stream->video, PW_DIRECTION_INPUT,
                                       stream->session.node_id,
                                       PW_STREAM_FLAG_AUTOCONNECT |
                                           PW_STREAM_FLAG_MAP_BUFFERS,
                                       params, 1);
    if (connect_rc < 0) {
        VS_LOG("pw_stream_connect(video): %s", spa_strerror(connect_rc));
        goto fail_locked;
    }

    if (config->capture_system_audio || config->capture_microphone) {
        stream->audio_core = pw_context_connect(stream->context, NULL, 0);
        if (!stream->audio_core) {
            VS_LOG("second pw_context_connect for audio failed: %s", strerror(errno));
        } else {
            if (config->capture_system_audio)
                connect_audio(stream, &stream->system_audio, VS_CAPTURE_AUDIO_SYSTEM,
                              config->audio_sink);
            if (config->capture_microphone)
                connect_audio(stream, &stream->microphone, VS_CAPTURE_AUDIO_MICROPHONE,
                              config->audio_source);
        }
    }

    vs_capture_clock_start(&stream->clock, vs_capture_now_ns());
    stream->started = true;
    pw_thread_loop_unlock(stream->loop);
    pw_thread_loop_start(stream->loop);
    *stream_out = stream;
    return VS_OK;

fail_locked:
    pw_thread_loop_unlock(stream->loop);
    if (stream->core) pw_core_disconnect(stream->core);
    if (stream->context) pw_context_destroy(stream->context);
    pw_thread_loop_destroy(stream->loop);
    vs_portal_session_close(engine, &stream->session);
    stream_free(stream);
    return VS_ERR_BACKEND;
}

void vs_capture_stream_stop(vs_capture_stream *stream)
{
    if (!stream) return;
    pthread_mutex_lock(&stream->lock);
    stream->stopping = true;
    pthread_cond_broadcast(&stream->frame_ready);
    pthread_cond_broadcast(&stream->audio_ready);
    pthread_mutex_unlock(&stream->lock);

    if (stream->loop) {
        pw_thread_loop_lock(stream->loop);
        if (stream->system_audio.pw) pw_stream_destroy(stream->system_audio.pw);
        if (stream->microphone.pw) pw_stream_destroy(stream->microphone.pw);
        if (stream->video) pw_stream_destroy(stream->video);
        pw_thread_loop_unlock(stream->loop);
        /* Stopping the loop joins its thread, so once this returns no callback
         * is running and none can start. */
        pw_thread_loop_stop(stream->loop);
        if (stream->audio_core) pw_core_disconnect(stream->audio_core);
        if (stream->core) pw_core_disconnect(stream->core);
        if (stream->context) pw_context_destroy(stream->context);
        pw_thread_loop_destroy(stream->loop);
    }
    vs_portal_session_close(stream->engine, &stream->session);
    stream_free(stream);
}

/* -------------------------------------------------------------- delivery */

void vs_capture_stream_set_frame_callback(vs_capture_stream *stream,
                                          vs_capture_frame_cb callback, void *user_data)
{
    if (!stream) return;
    pthread_mutex_lock(&stream->lock);
    stream->frame_cb = callback;
    stream->frame_cb_user = user_data;
    pthread_mutex_unlock(&stream->lock);
}

void vs_capture_stream_set_audio_callback(vs_capture_stream *stream,
                                          vs_capture_audio_cb callback, void *user_data)
{
    if (!stream) return;
    pthread_mutex_lock(&stream->lock);
    stream->audio_cb = callback;
    stream->audio_cb_user = user_data;
    pthread_mutex_unlock(&stream->lock);
}

int vs_capture_stream_event_fd(const vs_capture_stream *stream)
{
    return stream ? stream->event_fd : -1;
}

int vs_capture_stream_dispatch(vs_capture_stream *stream)
{
    if (!stream) return VS_ERR_INVALID;
    if (stream->event_fd >= 0) {
        /* Cleared before the queues are read, never after: an item that arrives
         * mid-dispatch writes its own token and the descriptor stays readable,
         * so the caller comes back for it instead of sleeping through it. */
        uint64_t drained;
        ssize_t got = read(stream->event_fd, &drained, sizeof(drained));
        (void)got;
    }

    int delivered = 0;
    for (;;) {
        vs_capture_frame frame;
        vs_capture_frame_cb frame_cb = NULL;
        vs_capture_audio_buffer audio;
        vs_capture_audio_cb audio_cb = NULL;
        void *user_data = NULL;

        pthread_mutex_lock(&stream->lock);
        if (stream->frame_count && !stream->frame_held && stream->frame_cb) {
            frame = stream->frames[stream->frame_head].frame;
            frame_cb = stream->frame_cb;
            user_data = stream->frame_cb_user;
            stream->frame_held = true;
        } else if (stream->audio_count && !stream->audio_held && stream->audio_cb) {
            audio = stream->audio[stream->audio_head].buffer;
            audio_cb = stream->audio_cb;
            user_data = stream->audio_cb_user;
            stream->audio_held = true;
        }
        pthread_mutex_unlock(&stream->lock);

        if (frame_cb) {
            frame_cb(&frame, user_data);
            vs_capture_stream_release_frame(stream);
        } else if (audio_cb) {
            audio_cb(&audio, user_data);
            vs_capture_stream_release_audio(stream);
        } else {
            break;
        }
        delivered++;
    }
    return delivered;
}

int vs_capture_stream_next_frame(vs_capture_stream *stream, vs_capture_frame *frame_out,
                                 int timeout_ms)
{
    if (!stream || !frame_out) return VS_ERR_INVALID;
    pthread_mutex_lock(&stream->lock);
    if (stream->frame_held) {
        pthread_mutex_unlock(&stream->lock);
        VS_LOG("next_frame called without releasing the previous frame");
        return VS_ERR_INVALID;
    }
    int rc = VS_OK;
    if (stream->frame_count == 0) {
        if (timeout_ms == 0) {
            rc = VS_ERR_TIMEOUT;
        } else if (timeout_ms < 0) {
            while (stream->frame_count == 0 && !stream->stopping)
                pthread_cond_wait(&stream->frame_ready, &stream->lock);
        } else {
            struct timespec deadline;
            timespec_in(&deadline, timeout_ms);
            while (stream->frame_count == 0 && !stream->stopping) {
                if (pthread_cond_timedwait(&stream->frame_ready, &stream->lock,
                                           &deadline) != 0)
                    break;
            }
        }
    }
    if (rc == VS_OK && stream->frame_count == 0) rc = VS_ERR_TIMEOUT;
    if (rc == VS_OK) {
        *frame_out = stream->frames[stream->frame_head].frame;
        stream->frame_held = true;
    }
    pthread_mutex_unlock(&stream->lock);
    return rc;
}

void vs_capture_stream_release_frame(vs_capture_stream *stream)
{
    if (!stream) return;
    pthread_mutex_lock(&stream->lock);
    if (stream->frame_held) {
        stream->frame_held = false;
        stream->frame_head = (stream->frame_head + 1) % stream->frame_capacity;
        stream->frame_count--;
    }
    pthread_mutex_unlock(&stream->lock);
}

int vs_capture_stream_next_audio(vs_capture_stream *stream,
                                 vs_capture_audio_buffer *buffer_out, int timeout_ms)
{
    if (!stream || !buffer_out) return VS_ERR_INVALID;
    pthread_mutex_lock(&stream->lock);
    if (stream->audio_held) {
        pthread_mutex_unlock(&stream->lock);
        VS_LOG("next_audio called without releasing the previous buffer");
        return VS_ERR_INVALID;
    }
    int rc = VS_OK;
    if (stream->audio_count == 0) {
        if (timeout_ms == 0) {
            rc = VS_ERR_TIMEOUT;
        } else if (timeout_ms < 0) {
            while (stream->audio_count == 0 && !stream->stopping)
                pthread_cond_wait(&stream->audio_ready, &stream->lock);
        } else {
            struct timespec deadline;
            timespec_in(&deadline, timeout_ms);
            while (stream->audio_count == 0 && !stream->stopping) {
                if (pthread_cond_timedwait(&stream->audio_ready, &stream->lock,
                                           &deadline) != 0)
                    break;
            }
        }
    }
    if (rc == VS_OK && stream->audio_count == 0) rc = VS_ERR_TIMEOUT;
    if (rc == VS_OK) {
        *buffer_out = stream->audio[stream->audio_head].buffer;
        stream->audio_held = true;
    }
    pthread_mutex_unlock(&stream->lock);
    return rc;
}

void vs_capture_stream_release_audio(vs_capture_stream *stream)
{
    if (!stream) return;
    pthread_mutex_lock(&stream->lock);
    if (stream->audio_held) {
        stream->audio_held = false;
        stream->audio_head = (stream->audio_head + 1) % stream->audio_capacity;
        stream->audio_count--;
    }
    pthread_mutex_unlock(&stream->lock);
}

/* ---------------------------------------------------------- pause/resume */

void vs_capture_stream_pause(vs_capture_stream *stream)
{
    if (!stream) return;
    pthread_mutex_lock(&stream->lock);
    vs_capture_clock_pause(&stream->clock, vs_capture_now_ns());
    pthread_mutex_unlock(&stream->lock);
}

void vs_capture_stream_resume(vs_capture_stream *stream)
{
    if (!stream) return;
    pthread_mutex_lock(&stream->lock);
    vs_capture_clock_resume(&stream->clock, vs_capture_now_ns());
    /* The rate cap compares against the last delivered capture instant; after a
     * pause that instant is arbitrarily old, so clearing it lets the first
     * frame back through immediately instead of being judged against a gap. */
    stream->last_delivered_pts_ns = 0;
    pthread_mutex_unlock(&stream->lock);
}

bool vs_capture_stream_is_paused(const vs_capture_stream *stream)
{
    if (!stream) return false;
    vs_capture_stream *mutable_stream = (vs_capture_stream *)stream;
    pthread_mutex_lock(&mutable_stream->lock);
    bool paused = stream->clock.paused;
    pthread_mutex_unlock(&mutable_stream->lock);
    return paused;
}

/* ------------------------------------------------------------ accessors */

uint32_t vs_capture_stream_source_type(const vs_capture_stream *stream)
{
    return stream ? stream->session.granted_source_type : 0;
}

bool vs_capture_stream_source_mismatch(const vs_capture_stream *stream)
{
    if (!stream || !stream->session.granted_source_type) return false;
    return (stream->session.granted_source_type &
            stream->session.requested_source_types) == 0;
}

int vs_capture_stream_size(const vs_capture_stream *stream, uint32_t *width_out,
                           uint32_t *height_out)
{
    if (!stream) return VS_ERR_INVALID;
    vs_capture_stream *mutable_stream = (vs_capture_stream *)stream;
    pthread_mutex_lock(&mutable_stream->lock);
    bool have = stream->have_format;
    if (have) {
        if (width_out) *width_out = stream->width;
        if (height_out) *height_out = stream->height;
    }
    pthread_mutex_unlock(&mutable_stream->lock);
    return have ? VS_OK : VS_ERR_TIMEOUT;
}

vs_capture_pixel_format vs_capture_stream_format(const vs_capture_stream *stream)
{
    return stream ? stream->format : VS_CAPTURE_PIXEL_UNKNOWN;
}

const char *vs_capture_stream_restore_token(const vs_capture_stream *stream)
{
    return stream ? stream->session.restore_token : NULL;
}

void vs_capture_stream_stats(const vs_capture_stream *stream, vs_capture_stats *stats_out)
{
    if (!stream || !stats_out) return;
    vs_capture_stream *mutable_stream = (vs_capture_stream *)stream;
    pthread_mutex_lock(&mutable_stream->lock);
    *stats_out = stream->stats;
    stats_out->paused_total_ns = stream->clock.paused_total_ns;
    pthread_mutex_unlock(&mutable_stream->lock);
}
