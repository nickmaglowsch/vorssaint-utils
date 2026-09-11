/*
 * WP-02 spike: xdg-desktop-portal Screenshot + ScreenCast -> PipeWire -> H.264/AAC.
 *
 * This is throwaway proof code for the Linux port. It deliberately uses the
 * same three pieces the shipping implementation would use, and nothing else:
 *
 *   GLib/GDBus          portal Request/Response choreography
 *   libpipewire         the stream the portal hands back
 *   libav*              encode + mux, so the "bundled ffmpeg" story is tested
 *
 * Subcommands:
 *   capture screenshot -o shot.png
 *   capture screencast -o out.mp4 [-d 10] [--audio-sink NAME]
 *                      [--source-type monitor|window|both]
 *                      [--restore-token FILE] [--no-persist]
 *   capture probe                       print portal versions / source types
 *
 * Everything it measures (frame count, inter-frame gaps, spa_meta_header pts
 * versus CLOCK_MONOTONIC, own CPU time) is printed as `key=value` lines
 * prefixed with `STAT ` so a shell can grep them.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/audio/format-utils.h>
#include <spa/debug/types.h>
#include <spa/utils/result.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

/* ------------------------------------------------------------------ utils */

#define PORTAL_BUS   "org.freedesktop.portal.Desktop"
#define PORTAL_PATH  "/org/freedesktop/portal/desktop"

static int g_verbose = 1;

#define LOGI(fmt, ...) do { fprintf(stderr, "[capture] " fmt "\n", ##__VA_ARGS__); } while (0)
#define LOGV(fmt, ...) do { if (g_verbose > 1) fprintf(stderr, "[capture] " fmt "\n", ##__VA_ARGS__); } while (0)
#define STAT(fmt, ...) do { fprintf(stdout, "STAT " fmt "\n", ##__VA_ARGS__); fflush(stdout); } while (0)

static int64_t now_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* utime+stime of this process, in seconds. */
static double self_cpu_seconds(void)
{
    FILE *f = fopen("/proc/self/stat", "r");
    if (!f) return -1.0;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return -1.0;
    buf[n] = 0;
    /* comm may contain spaces and parentheses: field 2 ends at the last ')'. */
    char *p = strrchr(buf, ')');
    if (!p) return -1.0;
    p += 2;                       /* skip ") " -> now at field 3 (state) */
    unsigned long utime = 0, stime = 0;
    int field = 3;
    char *tok, *save = NULL;
    for (tok = strtok_r(p, " ", &save); tok; tok = strtok_r(NULL, " ", &save), field++) {
        if (field == 14) utime = strtoul(tok, NULL, 10);
        else if (field == 15) { stime = strtoul(tok, NULL, 10); break; }
    }
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    return (double)(utime + stime) / (double)hz;
}

/* Portal Request objects live at a path derived from our unique bus name. */
static char *request_path_for(GDBusConnection *bus, const char *token)
{
    const char *unique = g_dbus_connection_get_unique_name(bus);
    GString *s = g_string_new(unique && unique[0] == ':' ? unique + 1 : unique);
    for (gsize i = 0; i < s->len; i++)
        if (s->str[i] == '.') s->str[i] = '_';
    char *path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s",
                                 s->str, token);
    g_string_free(s, TRUE);
    return path;
}

static char *fresh_token(const char *prefix)
{
    static guint counter = 0;
    return g_strdup_printf("%s_%u_%u", prefix, (guint)getpid(), ++counter);
}

/* One portal Request: subscribe to Response first, then call, then spin a
 * nested main loop until the Response arrives (or we time out). */
typedef struct {
    GMainLoop *loop;
    guint32    response;      /* 0 success, 1 user cancelled, 2 other */
    GVariant  *results;       /* a{sv}, owned */
    bool       got;
} ReqWait;

static void on_response(GDBusConnection *bus, const gchar *sender,
                        const gchar *path, const gchar *iface,
                        const gchar *signal, GVariant *params, gpointer user)
{
    (void)bus; (void)sender; (void)path; (void)iface; (void)signal;
    ReqWait *w = user;
    g_variant_get(params, "(u@a{sv})", &w->response, &w->results);
    w->got = true;
    g_main_loop_quit(w->loop);
}

static gboolean on_req_timeout(gpointer user)
{
    ReqWait *w = user;
    LOGI("ERROR: portal Request timed out");
    g_main_loop_quit(w->loop);
    return G_SOURCE_REMOVE;
}

/*
 * Call `method` on `iface` with `args` (which must already contain the
 * handle_token we generated), wait for the Response signal.
 * Returns the results dict (transfer full) or NULL; *response gets the code.
 */
static GVariant *portal_call(GDBusConnection *bus, const char *iface,
                             const char *method, GVariant *args,
                             const char *token, guint timeout_s,
                             guint32 *response)
{
    char *req_path = request_path_for(bus, token);
    ReqWait w = { .loop = g_main_loop_new(NULL, FALSE), .response = 2,
                  .results = NULL, .got = false };

    guint sub = g_dbus_connection_signal_subscribe(
        bus, PORTAL_BUS, "org.freedesktop.portal.Request", "Response",
        req_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_response, &w, NULL);

    GError *err = NULL;
    GVariant *ret = g_dbus_connection_call_sync(
        bus, PORTAL_BUS, PORTAL_PATH, iface, method, args, G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &err);
    if (!ret) {
        LOGI("ERROR: %s.%s failed: %s", iface, method, err->message);
        g_error_free(err);
        g_dbus_connection_signal_unsubscribe(bus, sub);
        g_main_loop_unref(w.loop);
        g_free(req_path);
        if (response) *response = 2;
        return NULL;
    }
    const char *actual = NULL;
    g_variant_get(ret, "(&o)", &actual);
    if (g_strcmp0(actual, req_path) != 0) {
        /* The portal may pick a different handle; resubscribe on the real one. */
        LOGV("%s.%s: handle %s (predicted %s)", iface, method, actual, req_path);
        g_dbus_connection_signal_unsubscribe(bus, sub);
        sub = g_dbus_connection_signal_subscribe(
            bus, PORTAL_BUS, "org.freedesktop.portal.Request", "Response",
            actual, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_response, &w, NULL);
    }
    g_variant_unref(ret);

    guint to = g_timeout_add_seconds(timeout_s, on_req_timeout, &w);
    g_main_loop_run(w.loop);
    g_source_remove(to);
    g_dbus_connection_signal_unsubscribe(bus, sub);
    g_main_loop_unref(w.loop);
    g_free(req_path);

    if (response) *response = w.got ? w.response : 2;
    return w.results;   /* may be NULL on timeout */
}

/* --------------------------------------------------------------- encoding */

typedef struct {
    AVFormatContext *fmt;
    /* video */
    AVStream        *vstream;
    AVCodecContext  *vctx;
    struct SwsContext *sws;
    AVFrame         *vframe;
    const char      *vencoder_name;
    /* audio */
    AVStream        *astream;
    AVCodecContext  *actx;
    SwrContext      *swr;
    AVFrame         *aframe;
    int64_t          apts;
    uint8_t         *afifo;      /* interleaved f32 waiting for a full frame */
    size_t           afifo_bytes;
    size_t           afifo_cap;
    bool             header_written;
    bool             audio_wanted;
} Encoder;

static const AVCodec *pick_video_encoder(const char **name_out)
{
    /* Preference order is a licensing decision as much as a technical one; see
     * the spike report. We report which one we actually got. */
    static const char *cands[] = { "libx264", "libopenh264", "h264_vaapi", "mpeg4", NULL };
    for (int i = 0; cands[i]; i++) {
        const AVCodec *c = avcodec_find_encoder_by_name(cands[i]);
        if (c) { *name_out = cands[i]; return c; }
    }
    *name_out = NULL;
    return NULL;
}

static int encoder_open_video(Encoder *e, const char *path, int w, int h, int fps)
{
    const AVCodec *codec = pick_video_encoder(&e->vencoder_name);
    if (!codec) { LOGI("ERROR: no usable video encoder in this libavcodec"); return -1; }
    LOGI("video encoder: %s", e->vencoder_name);
    STAT("video_encoder=%s", e->vencoder_name);

    if (avformat_alloc_output_context2(&e->fmt, NULL, NULL, path) < 0 || !e->fmt) {
        LOGI("ERROR: avformat_alloc_output_context2 failed for %s", path);
        return -1;
    }
    e->vstream = avformat_new_stream(e->fmt, NULL);
    if (!e->vstream) return -1;

    e->vctx = avcodec_alloc_context3(codec);
    e->vctx->width = w;
    e->vctx->height = h;
    e->vctx->pix_fmt = AV_PIX_FMT_YUV420P;
    /* microsecond timebase: PipeWire hands us real timestamps, not a cadence. */
    e->vctx->time_base = (AVRational){ 1, 1000000 };
    e->vctx->framerate = (AVRational){ fps, 1 };
    e->vctx->gop_size = fps * 2;
    e->vctx->max_b_frames = 0;
    e->vctx->bit_rate = 4000000;
    if (e->fmt->oformat->flags & AVFMT_GLOBALHEADER)
        e->vctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (strcmp(e->vencoder_name, "libx264") == 0) {
        av_opt_set(e->vctx->priv_data, "preset", "veryfast", 0);
        av_opt_set(e->vctx->priv_data, "tune", "zerolatency", 0);
    }
    int rc = avcodec_open2(e->vctx, codec, NULL);
    if (rc < 0) { LOGI("ERROR: avcodec_open2(video) %s", av_err2str(rc)); return -1; }
    avcodec_parameters_from_context(e->vstream->codecpar, e->vctx);
    e->vstream->time_base = e->vctx->time_base;

    e->vframe = av_frame_alloc();
    e->vframe->format = AV_PIX_FMT_YUV420P;
    e->vframe->width = w;
    e->vframe->height = h;
    if (av_frame_get_buffer(e->vframe, 32) < 0) return -1;
    return 0;
}

static int encoder_open_audio(Encoder *e, int rate, int channels)
{
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) { LOGI("WARNING: no AAC encoder, audio disabled"); return -1; }
    e->astream = avformat_new_stream(e->fmt, NULL);
    if (!e->astream) return -1;
    e->actx = avcodec_alloc_context3(codec);
    e->actx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    e->actx->sample_rate = rate;
    e->actx->bit_rate = 128000;
    av_channel_layout_default(&e->actx->ch_layout, channels);
    e->actx->time_base = (AVRational){ 1, rate };
    if (e->fmt->oformat->flags & AVFMT_GLOBALHEADER)
        e->actx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    int rc = avcodec_open2(e->actx, codec, NULL);
    if (rc < 0) { LOGI("ERROR: avcodec_open2(audio) %s", av_err2str(rc)); return -1; }
    avcodec_parameters_from_context(e->astream->codecpar, e->actx);
    e->astream->time_base = e->actx->time_base;

    /* PipeWire gives us interleaved F32; AAC wants planar float. */
    e->swr = NULL;
    rc = swr_alloc_set_opts2(&e->swr,
                             &e->actx->ch_layout, AV_SAMPLE_FMT_FLTP, rate,
                             &e->actx->ch_layout, AV_SAMPLE_FMT_FLT,  rate,
                             0, NULL);
    if (rc < 0 || swr_init(e->swr) < 0) { LOGI("ERROR: swr init"); return -1; }

    e->aframe = av_frame_alloc();
    e->aframe->format = AV_SAMPLE_FMT_FLTP;
    e->aframe->sample_rate = rate;
    e->aframe->nb_samples = e->actx->frame_size;
    av_channel_layout_copy(&e->aframe->ch_layout, &e->actx->ch_layout);
    if (av_frame_get_buffer(e->aframe, 0) < 0) return -1;

    e->afifo_cap = (size_t)e->actx->frame_size * channels * sizeof(float) * 4;
    e->afifo = malloc(e->afifo_cap);
    e->afifo_bytes = 0;
    STAT("audio_encoder=aac rate=%d channels=%d frame_size=%d",
         rate, channels, e->actx->frame_size);
    return 0;
}

static int encoder_write_header(Encoder *e, const char *path)
{
    if (!(e->fmt->oformat->flags & AVFMT_NOFILE)) {
        int rc = avio_open(&e->fmt->pb, path, AVIO_FLAG_WRITE);
        if (rc < 0) { LOGI("ERROR: avio_open %s: %s", path, av_err2str(rc)); return -1; }
    }
    int rc = avformat_write_header(e->fmt, NULL);
    if (rc < 0) { LOGI("ERROR: avformat_write_header: %s", av_err2str(rc)); return -1; }
    e->header_written = true;
    return 0;
}

static void drain(Encoder *e, AVCodecContext *ctx, AVStream *st)
{
    AVPacket *pkt = av_packet_alloc();
    while (avcodec_receive_packet(ctx, pkt) == 0) {
        pkt->stream_index = st->index;
        av_packet_rescale_ts(pkt, ctx->time_base, st->time_base);
        av_interleaved_write_frame(e->fmt, pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
}

static void encoder_close(Encoder *e)
{
    if (!e->fmt) return;
    if (e->header_written) {
        if (e->vctx) { avcodec_send_frame(e->vctx, NULL); drain(e, e->vctx, e->vstream); }
        if (e->actx) { avcodec_send_frame(e->actx, NULL); drain(e, e->actx, e->astream); }
        av_write_trailer(e->fmt);
    }
    if (e->fmt->pb) avio_closep(&e->fmt->pb);
    if (e->sws) sws_freeContext(e->sws);
    if (e->vframe) av_frame_free(&e->vframe);
    if (e->aframe) av_frame_free(&e->aframe);
    if (e->vctx) avcodec_free_context(&e->vctx);
    if (e->actx) avcodec_free_context(&e->actx);
    if (e->swr) swr_free(&e->swr);
    free(e->afifo);
    avformat_free_context(e->fmt);
    memset(e, 0, sizeof(*e));
}

/* --------------------------------------------------------- pipewire capture */

typedef struct {
    struct pw_thread_loop *loop;
    struct pw_context     *context;
    struct pw_core        *core;    /* the portal's restricted fd */
    struct pw_core        *acore;   /* a normal connection, for audio */

    struct pw_stream      *vstream;
    struct spa_hook        vlistener;
    struct spa_video_info_raw vinfo;
    bool                   vhave_format;

    struct pw_stream      *astream;
    struct spa_hook        alistener;
    struct spa_audio_info_raw ainfo;
    bool                   ahave_format;

    Encoder                enc;
    const char            *out_path;
    int                    target_fps;

    /* measurements */
    uint64_t  frames;
    uint64_t  frames_with_header;
    int64_t   first_wall_ns;
    int64_t   last_wall_ns;
    int64_t   first_pts_ns;
    double    lat_sum_ms;
    double    lat_min_ms;
    double    lat_max_ms;
    uint64_t  audio_frames;
    uint64_t  audio_samples;
    uint64_t  dropped;
} Capture;

static void on_video_state(void *data, enum pw_stream_state old,
                           enum pw_stream_state state, const char *err)
{
    (void)data; (void)old;
    LOGI("video stream state: %s%s%s", pw_stream_state_as_string(state),
         err ? " err=" : "", err ? err : "");
}

static void on_video_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    Capture *c = data;
    if (!param || id != SPA_PARAM_Format) return;

    uint32_t mtype, msubtype;
    if (spa_format_parse(param, &mtype, &msubtype) < 0) return;
    if (mtype != SPA_MEDIA_TYPE_video || msubtype != SPA_MEDIA_SUBTYPE_raw) return;
    if (spa_format_video_raw_parse(param, &c->vinfo) < 0) return;

    LOGI("negotiated video: %s %ux%u @ %u/%u",
         spa_debug_type_find_short_name(spa_type_video_format, c->vinfo.format),
         c->vinfo.size.width, c->vinfo.size.height,
         c->vinfo.max_framerate.num ? c->vinfo.max_framerate.num : c->vinfo.framerate.num,
         c->vinfo.max_framerate.denom ? c->vinfo.max_framerate.denom : c->vinfo.framerate.denom);
    STAT("negotiated_format=%s width=%u height=%u",
         spa_debug_type_find_short_name(spa_type_video_format, c->vinfo.format),
         c->vinfo.size.width, c->vinfo.size.height);
    c->vhave_format = true;

    /* Ask for a header meta so we can read the capture timestamp. */
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[2];
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
    params[1] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16));
    pw_stream_update_params(c->vstream, params, 2);

    if (c->enc.fmt) return;   /* already set up */

    enum AVPixelFormat src;
    switch (c->vinfo.format) {
    case SPA_VIDEO_FORMAT_BGRx: src = AV_PIX_FMT_BGR0; break;
    case SPA_VIDEO_FORMAT_BGRA: src = AV_PIX_FMT_BGRA; break;
    case SPA_VIDEO_FORMAT_RGBx: src = AV_PIX_FMT_RGB0; break;
    case SPA_VIDEO_FORMAT_RGBA: src = AV_PIX_FMT_RGBA; break;
    case SPA_VIDEO_FORMAT_RGB:  src = AV_PIX_FMT_RGB24; break;
    case SPA_VIDEO_FORMAT_BGR:  src = AV_PIX_FMT_BGR24; break;
    default:
        LOGI("ERROR: unhandled spa video format %u", c->vinfo.format);
        return;
    }
    int w = (int)c->vinfo.size.width, h = (int)c->vinfo.size.height;
    if (encoder_open_video(&c->enc, c->out_path, w, h, c->target_fps) < 0) return;
    if (c->enc.audio_wanted)
        encoder_open_audio(&c->enc, 48000, 2);
    c->enc.sws = sws_getContext(w, h, src, w, h, AV_PIX_FMT_YUV420P,
                                SWS_BILINEAR, NULL, NULL, NULL);
    if (encoder_write_header(&c->enc, c->out_path) < 0) return;
    LOGI("muxer open: %s", c->out_path);
}

static void on_video_process(void *data)
{
    Capture *c = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(c->vstream);
    if (!b) { c->dropped++; return; }
    struct spa_buffer *sb = b->buffer;

    if (!sb->datas[0].data || sb->datas[0].chunk->size == 0) {
        pw_stream_queue_buffer(c->vstream, b);
        return;
    }
    int64_t wall = now_monotonic_ns();

    struct spa_meta_header *hdr =
        spa_buffer_find_meta_data(sb, SPA_META_Header, sizeof(*hdr));
    int64_t pts_ns = 0;
    if (hdr && hdr->pts > 0) {
        c->frames_with_header++;
        pts_ns = hdr->pts;
        double lat_ms = (double)(wall - pts_ns) / 1e6;
        c->lat_sum_ms += lat_ms;
        if (c->frames_with_header == 1 || lat_ms < c->lat_min_ms) c->lat_min_ms = lat_ms;
        if (c->frames_with_header == 1 || lat_ms > c->lat_max_ms) c->lat_max_ms = lat_ms;
        if (c->first_pts_ns == 0) c->first_pts_ns = pts_ns;
    }
    if (c->first_wall_ns == 0) c->first_wall_ns = wall;
    c->last_wall_ns = wall;
    c->frames++;

    if (c->enc.header_written && c->enc.sws) {
        const uint8_t *src[4] = { sb->datas[0].data, NULL, NULL, NULL };
        int stride[4] = { (int)sb->datas[0].chunk->stride, 0, 0, 0 };
        if (stride[0] <= 0) stride[0] = (int)c->vinfo.size.width * 4;
        if (av_frame_make_writable(c->enc.vframe) >= 0) {
            sws_scale(c->enc.sws, src, stride, 0, (int)c->vinfo.size.height,
                      c->enc.vframe->data, c->enc.vframe->linesize);
            /* microsecond pts relative to the first frame */
            int64_t base = c->first_pts_ns ? c->first_pts_ns : c->first_wall_ns;
            int64_t t = (pts_ns ? pts_ns : wall) - base;
            if (t < 0) t = 0;
            c->enc.vframe->pts = t / 1000;
            if (avcodec_send_frame(c->enc.vctx, c->enc.vframe) >= 0)
                drain(&c->enc, c->enc.vctx, c->enc.vstream);
        }
    }
    pw_stream_queue_buffer(c->vstream, b);
}

static const struct pw_stream_events video_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_video_state,
    .param_changed = on_video_param_changed,
    .process = on_video_process,
};

static void on_audio_state(void *data, enum pw_stream_state old,
                           enum pw_stream_state state, const char *err)
{
    (void)data; (void)old;
    LOGI("audio stream state: %s%s%s", pw_stream_state_as_string(state),
         err ? " err=" : "", err ? err : "");
}

static void on_audio_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    Capture *c = data;
    if (!param || id != SPA_PARAM_Format) return;
    uint32_t mtype, msubtype;
    if (spa_format_parse(param, &mtype, &msubtype) < 0) return;
    if (mtype != SPA_MEDIA_TYPE_audio || msubtype != SPA_MEDIA_SUBTYPE_raw) return;
    if (spa_format_audio_raw_parse(param, &c->ainfo) < 0) return;
    c->ahave_format = true;
    LOGI("negotiated audio: rate=%u channels=%u", c->ainfo.rate, c->ainfo.channels);
    STAT("audio_negotiated rate=%u channels=%u", c->ainfo.rate, c->ainfo.channels);
}

static void audio_encode_fifo(Capture *c)
{
    Encoder *e = &c->enc;
    size_t bytes_per_frame = (size_t)e->actx->frame_size * e->actx->ch_layout.nb_channels
                             * sizeof(float);
    while (e->afifo_bytes >= bytes_per_frame) {
        const uint8_t *in = e->afifo;
        if (av_frame_make_writable(e->aframe) < 0) break;
        int got = swr_convert(e->swr, e->aframe->data, e->actx->frame_size,
                              &in, e->actx->frame_size);
        if (got > 0) {
            e->aframe->nb_samples = got;
            e->aframe->pts = e->apts;
            e->apts += got;
            if (avcodec_send_frame(e->actx, e->aframe) >= 0)
                drain(e, e->actx, e->astream);
        }
        memmove(e->afifo, e->afifo + bytes_per_frame, e->afifo_bytes - bytes_per_frame);
        e->afifo_bytes -= bytes_per_frame;
    }
}

static void on_audio_process(void *data)
{
    Capture *c = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(c->astream);
    if (!b) return;
    struct spa_buffer *sb = b->buffer;
    if (sb->datas[0].data && sb->datas[0].chunk->size) {
        uint32_t size = sb->datas[0].chunk->size;
        uint32_t off = sb->datas[0].chunk->offset;
        const uint8_t *src = (const uint8_t *)sb->datas[0].data + off;
        c->audio_frames++;
        c->audio_samples += size / (sizeof(float) * (c->ainfo.channels ? c->ainfo.channels : 2));
        Encoder *e = &c->enc;
        if (e->header_written && e->actx && e->afifo) {
            if (e->afifo_bytes + size > e->afifo_cap) {
                size_t want = e->afifo_bytes + size + 65536;
                uint8_t *grown = realloc(e->afifo, want);
                if (!grown) {
                    /* Drop this audio buffer rather than crash the recording. */
                    fprintf(stderr, "audio fifo: out of memory, dropping %u bytes\n", size);
                    pw_stream_queue_buffer(c->astream, b);
                    return;
                }
                e->afifo = grown;
                e->afifo_cap = want;
            }
            memcpy(e->afifo + e->afifo_bytes, src, size);
            e->afifo_bytes += size;
            audio_encode_fifo(c);
        }
    }
    pw_stream_queue_buffer(c->astream, b);
}

static const struct pw_stream_events audio_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_audio_state,
    .param_changed = on_audio_param_changed,
    .process = on_audio_process,
};

/* ------------------------------------------------------------- subcommands */

static int cmd_probe(GDBusConnection *bus)
{
    const char *ifaces[] = { "org.freedesktop.portal.ScreenCast",
                             "org.freedesktop.portal.Screenshot", NULL };
    for (int i = 0; ifaces[i]; i++) {
        GError *err = NULL;
        GVariant *v = g_dbus_connection_call_sync(
            bus, PORTAL_BUS, PORTAL_PATH, "org.freedesktop.DBus.Properties", "Get",
            g_variant_new("(ss)", ifaces[i], "version"), NULL,
            G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
        if (v) {
            GVariant *inner = NULL;
            g_variant_get(v, "(v)", &inner);
            STAT("%s version=%u", ifaces[i], g_variant_get_uint32(inner));
            g_variant_unref(inner); g_variant_unref(v);
        } else {
            STAT("%s version=UNAVAILABLE (%s)", ifaces[i], err->message);
            g_error_free(err);
        }
    }
    GError *err = NULL;
    GVariant *v = g_dbus_connection_call_sync(
        bus, PORTAL_BUS, PORTAL_PATH, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.freedesktop.portal.ScreenCast",
                      "AvailableSourceTypes"), NULL,
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
    if (v) {
        GVariant *inner = NULL;
        g_variant_get(v, "(v)", &inner);
        guint32 t = g_variant_get_uint32(inner);
        STAT("AvailableSourceTypes=%u monitor=%s window=%s virtual=%s",
             t, (t & 1) ? "yes" : "no", (t & 2) ? "yes" : "no", (t & 4) ? "yes" : "no");
        g_variant_unref(inner); g_variant_unref(v);
    } else {
        STAT("AvailableSourceTypes=UNAVAILABLE (%s)", err->message);
        g_error_free(err);
    }
    v = g_dbus_connection_call_sync(
        bus, PORTAL_BUS, PORTAL_PATH, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.freedesktop.portal.ScreenCast",
                      "AvailableCursorModes"), NULL,
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
    if (v) {
        GVariant *inner = NULL;
        g_variant_get(v, "(v)", &inner);
        STAT("AvailableCursorModes=%u", g_variant_get_uint32(inner));
        g_variant_unref(inner); g_variant_unref(v);
    }
    return 0;
}

static int cmd_screenshot(GDBusConnection *bus, const char *out)
{
    char *token = fresh_token("shot");
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&opts, "{sv}", "handle_token", g_variant_new_string(token));
    g_variant_builder_add(&opts, "{sv}", "interactive", g_variant_new_boolean(FALSE));

    int64_t t0 = now_monotonic_ns();
    guint32 resp = 2;
    GVariant *results = portal_call(bus, "org.freedesktop.portal.Screenshot",
                                    "Screenshot",
                                    g_variant_new("(sa{sv})", "", &opts),
                                    token, 30, &resp);
    double ms = (now_monotonic_ns() - t0) / 1e6;
    g_free(token);
    STAT("screenshot_response=%u elapsed_ms=%.1f", resp, ms);
    if (resp != 0 || !results) {
        LOGI("ERROR: Screenshot failed (response=%u)", resp);
        if (results) g_variant_unref(results);
        return 1;
    }
    const char *uri = NULL;
    GVariant *uv = g_variant_lookup_value(results, "uri", G_VARIANT_TYPE_STRING);
    if (uv) uri = g_variant_get_string(uv, NULL);
    if (!uri) { LOGI("ERROR: no uri in Screenshot results"); g_variant_unref(results); return 1; }
    STAT("screenshot_uri=%s", uri);

    GError *err = NULL;
    GFile *src = g_file_new_for_uri(uri);
    GFile *dst = g_file_new_for_path(out);
    gboolean ok = g_file_copy(src, dst, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &err);
    if (!ok) { LOGI("ERROR: copy %s -> %s: %s", uri, out, err->message); g_error_free(err); }
    else     { STAT("screenshot_saved=%s", out); }
    g_object_unref(src); g_object_unref(dst);
    g_variant_unref(uv); g_variant_unref(results);
    return ok ? 0 : 1;
}

typedef struct {
    guint32  source_types;      /* 1 monitor, 2 window, 4 virtual */
    guint32  persist_mode;      /* 0 none, 1 transient, 2 persistent */
    char    *restore_token_in;
    char    *restore_token_out;
    char    *session_handle;
    int      pw_fd;
    guint32  node_id;
} CastSession;

/* CreateSession -> SelectSources -> Start -> OpenPipeWireRemote. */
static int screencast_negotiate(GDBusConnection *bus, CastSession *s)
{
    guint32 resp;
    /* --- CreateSession --- */
    char *tok = fresh_token("cs");
    char *stok = fresh_token("sess");
    GVariantBuilder o;
    g_variant_builder_init(&o, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&o, "{sv}", "handle_token", g_variant_new_string(tok));
    g_variant_builder_add(&o, "{sv}", "session_handle_token", g_variant_new_string(stok));
    GVariant *res = portal_call(bus, "org.freedesktop.portal.ScreenCast",
                                "CreateSession", g_variant_new("(a{sv})", &o),
                                tok, 30, &resp);
    g_free(tok); g_free(stok);
    STAT("CreateSession_response=%u", resp);
    if (resp != 0 || !res) { LOGI("ERROR: CreateSession failed"); return -1; }
    GVariant *sh = g_variant_lookup_value(res, "session_handle", G_VARIANT_TYPE_STRING);
    if (!sh) { LOGI("ERROR: no session_handle"); g_variant_unref(res); return -1; }
    s->session_handle = g_strdup(g_variant_get_string(sh, NULL));
    g_variant_unref(sh); g_variant_unref(res);
    STAT("session_handle=%s", s->session_handle);

    /* --- SelectSources --- */
    tok = fresh_token("ss");
    g_variant_builder_init(&o, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&o, "{sv}", "handle_token", g_variant_new_string(tok));
    g_variant_builder_add(&o, "{sv}", "types", g_variant_new_uint32(s->source_types));
    g_variant_builder_add(&o, "{sv}", "multiple", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&o, "{sv}", "cursor_mode", g_variant_new_uint32(2)); /* embedded */
    g_variant_builder_add(&o, "{sv}", "persist_mode", g_variant_new_uint32(s->persist_mode));
    if (s->restore_token_in && s->restore_token_in[0]) {
        g_variant_builder_add(&o, "{sv}", "restore_token",
                              g_variant_new_string(s->restore_token_in));
        STAT("restore_token_sent=%s", s->restore_token_in);
    } else {
        STAT("restore_token_sent=(none)");
    }
    res = portal_call(bus, "org.freedesktop.portal.ScreenCast", "SelectSources",
                      g_variant_new("(oa{sv})", s->session_handle, &o), tok, 60, &resp);
    g_free(tok);
    STAT("SelectSources_response=%u types_requested=%u persist_mode=%u",
         resp, s->source_types, s->persist_mode);
    if (res) {
        char *dbg = g_variant_print(res, TRUE);
        STAT("SelectSources_results=%s", dbg);
        g_free(dbg);
        g_variant_unref(res);
    }
    if (resp != 0) { LOGI("ERROR: SelectSources refused (response=%u)", resp); return -1; }

    /* --- Start --- */
    tok = fresh_token("st");
    g_variant_builder_init(&o, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&o, "{sv}", "handle_token", g_variant_new_string(tok));
    res = portal_call(bus, "org.freedesktop.portal.ScreenCast", "Start",
                      g_variant_new("(osa{sv})", s->session_handle, "", &o),
                      tok, 60, &resp);
    g_free(tok);
    STAT("Start_response=%u", resp);
    if (resp != 0 || !res) { LOGI("ERROR: Start failed"); if (res) g_variant_unref(res); return -1; }
    {
        char *dbg = g_variant_print(res, TRUE);
        STAT("Start_results=%s", dbg);
        g_free(dbg);
    }
    GVariant *rt = g_variant_lookup_value(res, "restore_token", G_VARIANT_TYPE_STRING);
    if (rt) {
        s->restore_token_out = g_strdup(g_variant_get_string(rt, NULL));
        STAT("restore_token_received=%s", s->restore_token_out);
        g_variant_unref(rt);
    } else {
        STAT("restore_token_received=(absent)");
    }
    GVariant *streams = g_variant_lookup_value(res, "streams", G_VARIANT_TYPE("a(ua{sv})"));
    if (!streams || g_variant_n_children(streams) == 0) {
        LOGI("ERROR: Start returned no streams");
        if (streams) g_variant_unref(streams);
        g_variant_unref(res);
        return -1;
    }
    GVariant *st0 = g_variant_get_child_value(streams, 0);
    GVariant *props = NULL;
    g_variant_get(st0, "(u@a{sv})", &s->node_id, &props);
    {
        char *dbg = g_variant_print(props, TRUE);
        STAT("stream_node=%u props=%s", s->node_id, dbg);
        g_free(dbg);
    }
    g_variant_unref(props); g_variant_unref(st0);
    g_variant_unref(streams); g_variant_unref(res);

    /* --- OpenPipeWireRemote (returns a UnixFD) --- */
    GVariantBuilder empty;
    g_variant_builder_init(&empty, G_VARIANT_TYPE_VARDICT);
    GError *err = NULL;
    GUnixFDList *fds = NULL;
    GVariant *fdret = g_dbus_connection_call_with_unix_fd_list_sync(
        bus, PORTAL_BUS, PORTAL_PATH, "org.freedesktop.portal.ScreenCast",
        "OpenPipeWireRemote",
        g_variant_new("(oa{sv})", s->session_handle, &empty),
        G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &fds, NULL, &err);
    if (!fdret) {
        LOGI("ERROR: OpenPipeWireRemote: %s", err->message);
        g_error_free(err);
        return -1;
    }
    gint32 idx = -1;
    g_variant_get(fdret, "(h)", &idx);
    s->pw_fd = g_unix_fd_list_get(fds, idx, &err);
    g_variant_unref(fdret);
    g_object_unref(fds);
    if (s->pw_fd < 0) { LOGI("ERROR: no pipewire fd: %s", err->message); return -1; }
    STAT("pipewire_fd=%d node_id=%u", s->pw_fd, s->node_id);
    return 0;
}

static void session_close(GDBusConnection *bus, CastSession *s)
{
    if (!s->session_handle) return;
    g_dbus_connection_call_sync(bus, PORTAL_BUS, s->session_handle,
                                "org.freedesktop.portal.Session", "Close",
                                NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
}

static gboolean stop_after(gpointer user)
{
    g_main_loop_quit((GMainLoop *)user);
    return G_SOURCE_REMOVE;
}

static int cmd_screencast(GDBusConnection *bus, const char *out, int seconds,
                          guint32 source_types, const char *audio_sink,
                          const char *token_file, guint32 persist_mode)
{
    CastSession s = { .source_types = source_types, .persist_mode = persist_mode,
                      .pw_fd = -1 };
    if (token_file) {
        gchar *content = NULL;
        if (g_file_get_contents(token_file, &content, NULL, NULL) && content) {
            s.restore_token_in = g_strstrip(content);
        }
    }
    if (screencast_negotiate(bus, &s) < 0) return 1;

    if (s.restore_token_out && token_file)
        g_file_set_contents(token_file, s.restore_token_out, -1, NULL);

    /* ---- PipeWire ---- */
    Capture c = { 0 };
    c.out_path = out;
    c.target_fps = 30;
    c.enc.audio_wanted = (audio_sink != NULL);

    c.loop = pw_thread_loop_new("wp02", NULL);
    pw_thread_loop_lock(c.loop);
    c.context = pw_context_new(pw_thread_loop_get_loop(c.loop), NULL, 0);
    c.core = pw_context_connect_fd(c.context, fcntl(s.pw_fd, F_DUPFD_CLOEXEC, 3), NULL, 0);
    if (!c.core) {
        LOGI("ERROR: pw_context_connect_fd: %s", strerror(errno));
        pw_thread_loop_unlock(c.loop);
        return 1;
    }

    /* video stream on the node the portal gave us */
    struct pw_properties *vp = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        NULL);
    c.vstream = pw_stream_new(c.core, "wp02-screencast", vp);
    pw_stream_add_listener(c.vstream, &c.vlistener, &video_events, &c);

    uint8_t pbuf[2048];
    struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(pbuf, sizeof(pbuf));
    const struct spa_pod *vparams[1];
    vparams[0] = spa_pod_builder_add_object(&pb,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5,
            SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx,
            SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
            &SPA_RECTANGLE(1280, 720), &SPA_RECTANGLE(16, 16),
            &SPA_RECTANGLE(8192, 8192)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
            &SPA_FRACTION(30, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(144, 1)));

    int rc = pw_stream_connect(c.vstream, PW_DIRECTION_INPUT, s.node_id,
                               PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                               vparams, 1);
    if (rc < 0) {
        LOGI("ERROR: pw_stream_connect(video): %s", spa_strerror(rc));
        pw_thread_loop_unlock(c.loop);
        return 1;
    }

    /* Audio stream: capture the monitor of a sink.
     *
     * It must NOT go on the core we got from OpenPipeWireRemote. That fd is a
     * restricted connection whose permissions expose only the screencast node,
     * so an audio stream on it never leaves `paused`. Open a second, ordinary
     * connection to the user's PipeWire daemon for the audio side -- which is
     * also what the shipping app will do, since system-audio capture is not a
     * portal-mediated permission on any of the target desktops. */
    if (audio_sink) {
        c.acore = pw_context_connect(c.context, NULL, 0);
        if (!c.acore) {
            LOGI("WARNING: second pw_context_connect for audio failed: %s",
                 strerror(errno));
            goto audio_done;
        }
        struct pw_properties *ap = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Production",
            PW_KEY_STREAM_CAPTURE_SINK, "true",   /* i.e. take the sink monitor */
            PW_KEY_TARGET_OBJECT, audio_sink,
            PW_KEY_NODE_NAME, "wp02-sysaudio",
            NULL);
        c.astream = pw_stream_new(c.acore, "wp02-sysaudio", ap);
        pw_stream_add_listener(c.astream, &c.alistener, &audio_events, &c);

        uint8_t abuf[1024];
        struct spa_pod_builder ab = SPA_POD_BUILDER_INIT(abuf, sizeof(abuf));
        struct spa_audio_info_raw want = {
            .format = SPA_AUDIO_FORMAT_F32,
            .rate = 48000,
            .channels = 2,
            .position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR },
        };
        const struct spa_pod *aparams[1] = {
            spa_format_audio_raw_build(&ab, SPA_PARAM_EnumFormat, &want)
        };
        rc = pw_stream_connect(c.astream, PW_DIRECTION_INPUT, PW_ID_ANY,
                               PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                               aparams, 1);
        if (rc < 0)
            LOGI("WARNING: pw_stream_connect(audio): %s", spa_strerror(rc));
        else
            STAT("audio_target=%s", audio_sink);
    }
audio_done:

    pw_thread_loop_unlock(c.loop);
    pw_thread_loop_start(c.loop);

    double cpu0 = self_cpu_seconds();
    int64_t t0 = now_monotonic_ns();

    /* Keep the GLib loop running so the portal session stays serviced. */
    GMainLoop *ml = g_main_loop_new(NULL, FALSE);
    g_timeout_add_seconds((guint)seconds, stop_after, ml);
    g_main_loop_run(ml);
    g_main_loop_unref(ml);

    double wall = (now_monotonic_ns() - t0) / 1e9;
    double cpu = self_cpu_seconds() - cpu0;

    pw_thread_loop_lock(c.loop);
    if (c.astream) pw_stream_destroy(c.astream);
    if (c.vstream) pw_stream_destroy(c.vstream);
    pw_thread_loop_unlock(c.loop);
    pw_thread_loop_stop(c.loop);

    encoder_close(&c.enc);
    if (c.acore) pw_core_disconnect(c.acore);
    pw_core_disconnect(c.core);
    pw_context_destroy(c.context);
    pw_thread_loop_destroy(c.loop);
    close(s.pw_fd);
    session_close(bus, &s);

    double span = (c.last_wall_ns > c.first_wall_ns)
                    ? (c.last_wall_ns - c.first_wall_ns) / 1e9 : 0.0;
    STAT("wall_seconds=%.3f", wall);
    STAT("frames=%" PRIu64 " frames_with_header=%" PRIu64 " dropped=%" PRIu64,
         c.frames, c.frames_with_header, c.dropped);
    STAT("fps_over_capture_window=%.2f", wall > 0 ? c.frames / wall : 0.0);
    STAT("fps_first_to_last_frame=%.2f",
         span > 0 ? (double)(c.frames - 1) / span : 0.0);
    if (c.frames_with_header)
        STAT("latency_ms avg=%.3f min=%.3f max=%.3f (wall_monotonic - spa_meta_header.pts)",
             c.lat_sum_ms / (double)c.frames_with_header, c.lat_min_ms, c.lat_max_ms);
    else
        STAT("latency_ms=NO_HEADER_META");
    STAT("audio_buffers=%" PRIu64 " audio_samples=%" PRIu64 " (%.2f s)",
         c.audio_frames, c.audio_samples, c.audio_samples / 48000.0);
    STAT("cpu_seconds=%.3f cpu_percent_of_one_core=%.1f", cpu,
         wall > 0 ? 100.0 * cpu / wall : 0.0);
    STAT("output=%s", out);
    return c.frames > 0 ? 0 : 1;
}

/* -------------------------------------------------------------------- main */

static void usage(void)
{
    fprintf(stderr,
      "usage:\n"
      "  capture probe\n"
      "  capture screenshot -o FILE.png\n"
      "  capture screencast -o FILE.mp4 [-d SECONDS] [--audio-sink NAME]\n"
      "                     [--source-type monitor|window|both]\n"
      "                     [--restore-token FILE] [--no-persist]\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 2; }
    const char *cmd = argv[1];

    const char *out = NULL, *audio_sink = NULL, *token_file = NULL;
    int seconds = 10;
    guint32 source_types = 1;      /* MONITOR */
    guint32 persist_mode = 2;      /* persistent */

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--audio-sink") && i + 1 < argc) audio_sink = argv[++i];
        else if (!strcmp(argv[i], "--restore-token") && i + 1 < argc) token_file = argv[++i];
        else if (!strcmp(argv[i], "--no-persist")) persist_mode = 0;
        else if (!strcmp(argv[i], "-v")) g_verbose = 2;
        else if (!strcmp(argv[i], "--source-type") && i + 1 < argc) {
            const char *t = argv[++i];
            if (!strcmp(t, "monitor")) source_types = 1;
            else if (!strcmp(t, "window")) source_types = 2;
            else if (!strcmp(t, "both")) source_types = 3;
            else if (!strcmp(t, "virtual")) source_types = 4;
            else { fprintf(stderr, "unknown source type %s\n", t); return 2; }
        } else { fprintf(stderr, "unknown arg %s\n", argv[i]); usage(); return 2; }
    }

    pw_init(&argc, &argv);
    STAT("libpipewire=%s headers=%s", pw_get_library_version(), pw_get_headers_version());
    STAT("libavcodec=%u.%u.%u libavformat=%u.%u.%u",
         LIBAVCODEC_VERSION_MAJOR, LIBAVCODEC_VERSION_MINOR, LIBAVCODEC_VERSION_MICRO,
         LIBAVFORMAT_VERSION_MAJOR, LIBAVFORMAT_VERSION_MINOR, LIBAVFORMAT_VERSION_MICRO);

    GError *err = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!bus) { LOGI("ERROR: session bus: %s", err->message); return 1; }

    int rc;
    if (!strcmp(cmd, "probe")) {
        rc = cmd_probe(bus);
    } else if (!strcmp(cmd, "screenshot")) {
        if (!out) { usage(); return 2; }
        rc = cmd_screenshot(bus, out);
    } else if (!strcmp(cmd, "screencast")) {
        if (!out) { usage(); return 2; }
        rc = cmd_screencast(bus, out, seconds, source_types, audio_sink,
                            token_file, persist_mode);
    } else {
        usage(); rc = 2;
    }
    g_object_unref(bus);
    pw_deinit();
    return rc;
}
