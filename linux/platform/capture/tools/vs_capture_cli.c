/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * vs-capture: the CLI harness for the capture backend.
 *
 * Every backend under linux/platform has one, for the same reason: the Swift
 * side cannot be built in the team's environment, so the C contract has to be
 * exercisable on its own, by hand and by ctest, on a headless session.
 *
 * Measurements are printed as `STAT key=value` lines so a shell can grep them;
 * the shape is deliberately the same as the WP-02 spike's, so numbers from the
 * two are directly comparable.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "vorssaint_platform.h"

#define STAT(...)                                                              \
    do {                                                                       \
        printf("STAT ");                                                       \
        printf(__VA_ARGS__);                                                   \
        putchar('\n');                                                         \
        fflush(stdout);                                                        \
    } while (0)

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* utime+stime of this process in seconds, so the harness can report its own
 * cost the way the spike did rather than relying on an external sampler. */
static double self_cpu_seconds(void)
{
    FILE *f = fopen("/proc/self/stat", "r");
    if (!f) return -1.0;
    char buffer[4096];
    size_t length = fread(buffer, 1, sizeof(buffer) - 1, f);
    fclose(f);
    if (length == 0) return -1.0;
    buffer[length] = '\0';
    char *after_comm = strrchr(buffer, ')');
    if (!after_comm) return -1.0;
    after_comm += 2;
    unsigned long utime = 0, stime = 0;
    int field = 3;
    char *save = NULL;
    for (char *token = strtok_r(after_comm, " ", &save); token;
         token = strtok_r(NULL, " ", &save), field++) {
        if (field == 14) utime = strtoul(token, NULL, 10);
        else if (field == 15) { stime = strtoul(token, NULL, 10); break; }
    }
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    return (double)(utime + stime) / (double)hz;
}

static void print_capabilities(uint32_t capabilities)
{
    STAT("has_screenshot_portal=%s",
         (capabilities & VS_CAPTURE_HAS_SCREENSHOT_PORTAL) ? "yes" : "no");
    STAT("has_window_source=%s",
         (capabilities & VS_CAPTURE_HAS_WINDOW_SOURCE) ? "yes" : "no");
    STAT("has_region=%s", (capabilities & VS_CAPTURE_HAS_REGION) ? "yes" : "no");
    STAT("has_restore_token=%s",
         (capabilities & VS_CAPTURE_HAS_RESTORE_TOKEN) ? "yes" : "no");
    STAT("has_audio_monitor=%s",
         (capabilities & VS_CAPTURE_HAS_AUDIO_MONITOR) ? "yes" : "no");
    STAT("has_microphone=%s", (capabilities & VS_CAPTURE_HAS_MICROPHONE) ? "yes" : "no");
    STAT("has_virtual_source=%s",
         (capabilities & VS_CAPTURE_HAS_VIRTUAL_SOURCE) ? "yes" : "no");
    STAT("has_cursor_embedded=%s",
         (capabilities & VS_CAPTURE_HAS_CURSOR_EMBEDDED) ? "yes" : "no");
}

static int cmd_probe(vs_capture_engine *engine)
{
    uint32_t types = vs_capture_engine_source_types(engine);
    STAT("backend=%s", vs_capture_engine_name(engine));
    STAT("AvailableSourceTypes=%u monitor=%s window=%s virtual=%s", types,
         (types & VS_CAPTURE_SOURCE_MONITOR) ? "yes" : "no",
         (types & VS_CAPTURE_SOURCE_WINDOW) ? "yes" : "no",
         (types & VS_CAPTURE_SOURCE_VIRTUAL) ? "yes" : "no");
    STAT("AvailableCursorModes=%u", vs_capture_engine_cursor_modes(engine));
    print_capabilities(vs_capture_engine_capabilities(engine));
    return 0;
}

static int cmd_sources(vs_capture_engine *engine, uint32_t types)
{
    vs_capture_source *sources = NULL;
    size_t count = 0;
    int rc = vs_capture_enumerate_sources(engine, types, &sources, &count);
    if (rc != VS_OK) {
        STAT("sources=ERROR (%s)", vs_result_string(rc));
        return 1;
    }
    STAT("source_count=%zu", count);
    for (size_t i = 0; i < count; i++) {
        const vs_capture_source *source = &sources[i];
        STAT("source id=%" PRIu64 " type=%u name=%s bounds=%dx%d+%d+%d valid=%s "
             "refresh_mhz=%d scale=%.2f description=%s",
             source->id, source->type, source->name[0] ? source->name : "(unnamed)",
             source->bounds.width, source->bounds.height, source->bounds.x,
             source->bounds.y, source->bounds_valid ? "yes" : "no",
             source->refresh_mhz, source->scale,
             source->description[0] ? source->description : "(none)");
    }
    /* The honest part: say why a window list is empty, since "none listed" and
     * "cannot list" are different answers. */
    if (types == 0 || (types & VS_CAPTURE_SOURCE_WINDOW)) {
        uint32_t available = vs_capture_engine_source_types(engine);
        if (!(available & VS_CAPTURE_SOURCE_WINDOW))
            STAT("window_sources=unsupported (AvailableSourceTypes=%u has no WINDOW "
                 "bit; a WINDOW request on this backend is served as a MONITOR)",
                 available);
        else
            STAT("window_sources=chosen-by-portal (the portal's own chooser picks "
                 "the window; the app cannot enumerate them)");
    }
    vs_capture_free_sources(sources, count);
    return 0;
}

static bool parse_rect(const char *text, vs_rect *rect)
{
    int x, y, w, h;
    if (sscanf(text, "%d,%d,%d,%d", &x, &y, &w, &h) != 4) return false;
    rect->x = x;
    rect->y = y;
    rect->width = w;
    rect->height = h;
    return true;
}

static bool parse_source_type(const char *text, uint32_t *out)
{
    if (!strcmp(text, "monitor")) *out = VS_CAPTURE_SOURCE_MONITOR;
    else if (!strcmp(text, "window")) *out = VS_CAPTURE_SOURCE_WINDOW;
    else if (!strcmp(text, "virtual")) *out = VS_CAPTURE_SOURCE_VIRTUAL;
    else if (!strcmp(text, "both"))
        *out = VS_CAPTURE_SOURCE_MONITOR | VS_CAPTURE_SOURCE_WINDOW;
    else return false;
    return true;
}

static char *read_token_file(const char *path)
{
    if (!path) return NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char buffer[512] = { 0 };
    if (!fgets(buffer, sizeof(buffer), f)) buffer[0] = '\0';
    fclose(f);
    size_t length = strlen(buffer);
    while (length && (buffer[length - 1] == '\n' || buffer[length - 1] == ' '))
        buffer[--length] = '\0';
    return length ? strdup(buffer) : NULL;
}

static void write_token_file(const char *path, const char *token)
{
    if (!path || !token) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n", token);
    fclose(f);
}

/* ---------------------------------------------------------------- shot */

static int cmd_shot(vs_capture_engine *engine, const char *out, vs_capture_shot_method method,
                    const vs_rect *region, bool cursor, const char *token_path,
                    uint32_t timeout_ms, uint32_t source_types)
{
    vs_capture_shot_request request = { 0 };
    request.source_types = source_types;
    request.include_cursor = cursor;
    request.method = method;
    request.timeout_ms = timeout_ms;
    if (region) {
        request.region = *region;
        request.region_enabled = true;
    }
    char *token_in = read_token_file(token_path);
    request.restore_token = token_in;
    request.persist = token_path != NULL;

    vs_capture_image image = { 0 };
    vs_capture_shot_method used = VS_CAPTURE_SHOT_AUTO;
    char *token_out = NULL;
    int64_t started = now_ns();
    int rc = vs_capture_screenshot(engine, &request, &image, &used, &token_out);
    double elapsed_ms = (double)(now_ns() - started) / 1e6;
    free(token_in);

    STAT("shot_result=%s elapsed_ms=%.1f", vs_result_string(rc), elapsed_ms);
    if (rc != VS_OK) {
        free(token_out);
        return 1;
    }
    STAT("shot_method=%s", used == VS_CAPTURE_SHOT_PORTAL ? "portal" : "screencast");
    STAT("shot_size=%ux%u stride=%u format=%s bytes=%zu", image.width, image.height,
         image.stride, vs_capture_pixel_format_name(image.format), image.byte_length);
    if (token_out) {
        STAT("restore_token_received=%s", token_out);
        write_token_file(token_path, token_out);
    }
    free(token_out);

    int write_rc = VS_OK;
    if (out) {
        write_rc = vs_capture_image_write_png(&image, out);
        STAT("shot_png=%s write=%s", out, vs_result_string(write_rc));
    }
    vs_capture_image_free(&image);
    return write_rc == VS_OK ? 0 : 1;
}

/* -------------------------------------------------------------- stream */

typedef struct {
    uint64_t frames;
    uint64_t audio_buffers;
    uint64_t non_monotonic;
    int64_t previous_timeline_ns;
    bool have_previous;
    double audio_peak;
    double audio_rms_sum;
    uint64_t audio_sample_count;
    const char *save_path;
    bool saved;
    vs_capture_image saved_image;
} stream_tally;

static void note_frame(stream_tally *tally, const vs_capture_frame *frame)
{
    if (tally->have_previous && frame->timeline_ns < tally->previous_timeline_ns)
        tally->non_monotonic++;
    tally->previous_timeline_ns = frame->timeline_ns;
    tally->have_previous = true;
    tally->frames++;
    if (tally->save_path && !tally->saved) {
        if (vs_capture_image_from_frame(frame->data, frame->width, frame->height,
                                        frame->stride, frame->format, NULL,
                                        &tally->saved_image) == VS_OK)
            tally->saved = true;
    }
}

static void note_audio(stream_tally *tally, const vs_capture_audio_buffer *buffer)
{
    tally->audio_buffers++;
    size_t count = (size_t)buffer->frame_count * buffer->channels;
    for (size_t i = 0; i < count; i++) {
        double value = buffer->samples[i];
        double magnitude = value < 0 ? -value : value;
        if (magnitude > tally->audio_peak) tally->audio_peak = magnitude;
        tally->audio_rms_sum += value * value;
    }
    tally->audio_sample_count += count;
}

static void on_frame(const vs_capture_frame *frame, void *user_data)
{
    note_frame(user_data, frame);
}

static void on_audio(const vs_capture_audio_buffer *buffer, void *user_data)
{
    note_audio(user_data, buffer);
}

typedef struct {
    int seconds;
    bool pull;
    uint32_t max_fps;
    uint32_t source_types;
    const vs_rect *region;
    const char *audio_sink;
    bool system_audio;
    bool microphone;
    const char *audio_source;
    const char *token_path;
    bool persist;
    const char *save_path;
    double pause_at;
    double pause_for;
    uint32_t cursor_mode;
} stream_options;

static int cmd_stream(vs_capture_engine *engine, const stream_options *options)
{
    vs_capture_stream_config config = { 0 };
    config.source_types = options->source_types;
    config.cursor_mode = options->cursor_mode;
    config.max_fps = options->max_fps;
    config.capture_system_audio = options->system_audio;
    config.audio_sink = options->audio_sink;
    config.capture_microphone = options->microphone;
    config.audio_source = options->audio_source;
    config.persist = options->persist;
    config.queue_depth = 8;
    if (options->region) {
        config.region = *options->region;
        config.region_enabled = true;
    }
    char *token_in = read_token_file(options->token_path);
    config.restore_token = token_in;
    STAT("restore_token_sent=%s", token_in ? token_in : "(none)");

    vs_capture_stream *stream = NULL;
    int rc = vs_capture_stream_start(engine, &config, &stream);
    free(token_in);
    if (rc != VS_OK) {
        STAT("stream_start=%s", vs_result_string(rc));
        return 1;
    }
    STAT("stream_start=ok source_type_granted=%u mismatch=%s",
         vs_capture_stream_source_type(stream),
         vs_capture_stream_source_mismatch(stream) ? "yes" : "no");
    const char *token = vs_capture_stream_restore_token(stream);
    STAT("restore_token_received=%s", token ? token : "(absent)");
    if (token) write_token_file(options->token_path, token);

    stream_tally tally = { 0 };
    tally.save_path = options->save_path;
    if (!options->pull) {
        vs_capture_stream_set_frame_callback(stream, on_frame, &tally);
        vs_capture_stream_set_audio_callback(stream, on_audio, &tally);
    }

    double cpu_before = self_cpu_seconds();
    int64_t started = now_ns();
    int64_t deadline = started + (int64_t)options->seconds * 1000000000LL;
    int64_t pause_at = options->pause_for > 0
                           ? started + (int64_t)(options->pause_at * 1e9)
                           : 0;
    int64_t resume_at = pause_at
                            ? pause_at + (int64_t)(options->pause_for * 1e9)
                            : 0;
    bool paused = false;
    int64_t pause_began = 0, pause_ended = 0;

    while (now_ns() < deadline) {
        int64_t now = now_ns();
        if (pause_at && !paused && now >= pause_at) {
            vs_capture_stream_pause(stream);
            paused = true;
            pause_began = now_ns();
            STAT("paused_at_ms=%.1f", (double)(pause_began - started) / 1e6);
        }
        if (paused && resume_at && now >= resume_at) {
            vs_capture_stream_resume(stream);
            paused = false;
            resume_at = 0;
            pause_ended = now_ns();
            STAT("resumed_at_ms=%.1f gap_ms=%.1f",
                 (double)(pause_ended - started) / 1e6,
                 (double)(pause_ended - pause_began) / 1e6);
        }
        if (options->pull) {
            vs_capture_frame frame;
            if (vs_capture_stream_next_frame(stream, &frame, 5) == VS_OK) {
                note_frame(&tally, &frame);
                vs_capture_stream_release_frame(stream);
            }
            vs_capture_audio_buffer buffer;
            while (vs_capture_stream_next_audio(stream, &buffer, 0) == VS_OK) {
                note_audio(&tally, &buffer);
                vs_capture_stream_release_audio(stream);
            }
        } else {
            struct timespec sleep_for = { .tv_sec = 0, .tv_nsec = 20000000L };
            nanosleep(&sleep_for, NULL);
        }
    }
    double wall_seconds = (double)(now_ns() - started) / 1e9;
    double cpu_seconds = self_cpu_seconds() - cpu_before;

    vs_capture_stats stats;
    vs_capture_stream_stats(stream, &stats);
    uint32_t width = 0, height = 0;
    vs_capture_stream_size(stream, &width, &height);
    vs_capture_pixel_format format = vs_capture_stream_format(stream);
    vs_capture_stream_stop(stream);

    STAT("api=%s", options->pull ? "pull" : "callback");
    STAT("negotiated=%ux%u format=%s", width, height,
         vs_capture_pixel_format_name(format));
    STAT("wall_seconds=%.3f", wall_seconds);
    STAT("frames=%" PRIu64 " frames_with_pts=%" PRIu64, tally.frames,
         stats.frames_with_pts);
    STAT("frames_dropped rate=%" PRIu64 " paused=%" PRIu64 " queue=%" PRIu64
         " buffers_missed=%" PRIu64,
         stats.frames_dropped_rate, stats.frames_dropped_paused,
         stats.frames_dropped_queue, stats.buffers_missed);
    STAT("fps=%.2f", wall_seconds > 0 ? (double)tally.frames / wall_seconds : 0.0);
    STAT("timestamps_monotonic=%s non_monotonic=%" PRIu64,
         tally.non_monotonic == 0 ? "yes" : "no", tally.non_monotonic);
    if (stats.frames_with_pts)
        STAT("latency_ms avg=%.3f min=%.3f max=%.3f", stats.latency_avg_ms,
             stats.latency_min_ms, stats.latency_max_ms);
    else
        STAT("latency_ms=NO_HEADER_META");
    STAT("audio_buffers=%" PRIu64 " system=%" PRIu64 " microphone=%" PRIu64
         " dropped_paused=%" PRIu64,
         tally.audio_buffers, stats.audio_buffers_system,
         stats.audio_buffers_microphone, stats.audio_dropped_paused);
    STAT("audio_frames system=%" PRIu64 " microphone=%" PRIu64,
         stats.audio_frames_system, stats.audio_frames_microphone);
    if (tally.audio_sample_count) {
        double rms = tally.audio_rms_sum / (double)tally.audio_sample_count;
        rms = rms > 0 ? __builtin_sqrt(rms) : 0.0;
        STAT("audio_peak=%.6f audio_rms=%.6f samples=%" PRIu64, tally.audio_peak, rms,
             tally.audio_sample_count);
        STAT("audio_silent=%s", tally.audio_peak < 1e-5 ? "yes" : "no");
    } else {
        STAT("audio_peak=none");
    }

    double video_span_ms =
        (double)(stats.video_timeline_last_ns - stats.video_timeline_first_ns) / 1e6;
    double audio_span_ms =
        (double)(stats.audio_timeline_last_ns - stats.audio_timeline_first_ns) / 1e6;
    STAT("paused_total_ms=%.1f", (double)stats.paused_total_ns / 1e6);
    STAT("video_timeline_ms first=%.1f last=%.1f span=%.1f",
         (double)stats.video_timeline_first_ns / 1e6,
         (double)stats.video_timeline_last_ns / 1e6, video_span_ms);
    STAT("audio_timeline_ms first=%.1f last=%.1f span=%.1f",
         (double)stats.audio_timeline_first_ns / 1e6,
         (double)stats.audio_timeline_last_ns / 1e6, audio_span_ms);
    if (stats.audio_buffers_system || stats.audio_buffers_microphone) {
        double skew_ms = (double)(stats.video_timeline_last_ns -
                                  stats.audio_timeline_last_ns) / 1e6;
        if (skew_ms < 0) skew_ms = -skew_ms;
        double span_skew_ms = video_span_ms - audio_span_ms;
        if (span_skew_ms < 0) span_skew_ms = -span_skew_ms;
        STAT("alignment_end_skew_ms=%.1f alignment_span_skew_ms=%.1f", skew_ms,
             span_skew_ms);
    }
    STAT("cpu_seconds=%.3f cpu_percent_of_one_core=%.1f", cpu_seconds,
         wall_seconds > 0 ? 100.0 * cpu_seconds / wall_seconds : 0.0);

    int exit_code = tally.frames > 0 ? 0 : 1;
    if (tally.saved && options->save_path) {
        int write_rc = vs_capture_image_write_png(&tally.saved_image, options->save_path);
        STAT("saved_frame=%s write=%s size=%ux%u", options->save_path,
             vs_result_string(write_rc), tally.saved_image.width,
             tally.saved_image.height);
        if (write_rc != VS_OK) exit_code = 1;
    }
    vs_capture_image_free(&tally.saved_image);
    return exit_code;
}

static void usage(void)
{
    fprintf(stderr,
      "usage:\n"
      "  vs-capture probe\n"
      "  vs-capture sources [--type monitor|window|both]\n"
      "  vs-capture shot -o FILE.png [--method auto|portal|screencast]\n"
      "                  [--region X,Y,W,H] [--cursor] [--restore-token FILE]\n"
      "                  [--timeout-ms N] [--source-type monitor|window|both]\n"
      "  vs-capture stream -d SECONDS [--api callback|pull] [--max-fps N]\n"
      "                    [--region X,Y,W,H] [--source-type ...] [--cursor]\n"
      "                    [--audio-sink NAME] [--system-audio] [--mic [NAME]]\n"
      "                    [--restore-token FILE] [--no-persist]\n"
      "                    [--pause-at SECONDS --pause-for SECONDS]\n"
      "                    [--save-frame FILE.png]\n"
      "\n"
      "Set VS_CAPTURE_DEBUG=1 for the engine's own log.\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    const char *command = argv[1];

    const char *out = NULL, *token_path = NULL, *save_path = NULL;
    const char *audio_sink = NULL, *audio_source = NULL;
    vs_capture_shot_method method = VS_CAPTURE_SHOT_AUTO;
    vs_rect region = { 0, 0, 0, 0 };
    bool has_region = false, cursor = false, pull = false, persist = true;
    bool system_audio = false, microphone = false;
    int seconds = 5;
    uint32_t max_fps = 0, source_types = 0, timeout_ms = 0;
    double pause_at = 0, pause_for = 0;

    for (int i = 2; i < argc; i++) {
        const char *arg = argv[i];
        bool has_next = i + 1 < argc;
        if (!strcmp(arg, "-o") && has_next) out = argv[++i];
        else if (!strcmp(arg, "-d") && has_next) seconds = atoi(argv[++i]);
        else if (!strcmp(arg, "--max-fps") && has_next) max_fps = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(arg, "--timeout-ms") && has_next) timeout_ms = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(arg, "--restore-token") && has_next) token_path = argv[++i];
        else if (!strcmp(arg, "--save-frame") && has_next) save_path = argv[++i];
        else if (!strcmp(arg, "--audio-sink") && has_next) {
            audio_sink = argv[++i];
            system_audio = true;
        } else if (!strcmp(arg, "--system-audio")) system_audio = true;
        else if (!strcmp(arg, "--mic")) {
            microphone = true;
            if (has_next && argv[i + 1][0] != '-') audio_source = argv[++i];
        } else if (!strcmp(arg, "--cursor")) cursor = true;
        else if (!strcmp(arg, "--no-persist")) persist = false;
        else if (!strcmp(arg, "--pause-at") && has_next) pause_at = atof(argv[++i]);
        else if (!strcmp(arg, "--pause-for") && has_next) pause_for = atof(argv[++i]);
        else if (!strcmp(arg, "--api") && has_next) {
            const char *value = argv[++i];
            if (!strcmp(value, "pull")) pull = true;
            else if (strcmp(value, "callback") != 0) {
                fprintf(stderr, "unknown api %s\n", value);
                return 2;
            }
        } else if (!strcmp(arg, "--region") && has_next) {
            if (!parse_rect(argv[++i], &region)) {
                fprintf(stderr, "bad region, want X,Y,W,H\n");
                return 2;
            }
            has_region = true;
        } else if ((!strcmp(arg, "--source-type") || !strcmp(arg, "--type")) && has_next) {
            if (!parse_source_type(argv[++i], &source_types)) {
                fprintf(stderr, "unknown source type %s\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(arg, "--method") && has_next) {
            const char *value = argv[++i];
            if (!strcmp(value, "auto")) method = VS_CAPTURE_SHOT_AUTO;
            else if (!strcmp(value, "portal")) method = VS_CAPTURE_SHOT_PORTAL;
            else if (!strcmp(value, "screencast")) method = VS_CAPTURE_SHOT_SCREENCAST;
            else {
                fprintf(stderr, "unknown method %s\n", value);
                return 2;
            }
        } else {
            fprintf(stderr, "unknown argument %s\n", arg);
            usage();
            return 2;
        }
    }

    int reason = VS_OK;
    vs_capture_engine *engine = vs_capture_engine_create(NULL, &reason);
    if (!engine) {
        STAT("engine=unavailable (%s)", vs_result_string(reason));
        return 1;
    }

    int rc;
    if (!strcmp(command, "probe")) {
        rc = cmd_probe(engine);
    } else if (!strcmp(command, "sources")) {
        rc = cmd_sources(engine, source_types);
    } else if (!strcmp(command, "shot")) {
        rc = cmd_shot(engine, out, method, has_region ? &region : NULL, cursor,
                      token_path, timeout_ms, source_types);
    } else if (!strcmp(command, "stream")) {
        stream_options options = {
            .seconds = seconds,
            .pull = pull,
            .max_fps = max_fps,
            .source_types = source_types,
            .region = has_region ? &region : NULL,
            .audio_sink = audio_sink,
            .system_audio = system_audio,
            .microphone = microphone,
            .audio_source = audio_source,
            .token_path = token_path,
            .persist = persist,
            .save_path = save_path,
            .pause_at = pause_at,
            .pause_for = pause_for,
            .cursor_mode = cursor ? VS_CAPTURE_CURSOR_EMBEDDED : VS_CAPTURE_CURSOR_HIDDEN,
        };
        rc = cmd_stream(engine, &options);
    } else {
        usage();
        rc = 2;
    }
    vs_capture_engine_destroy(engine);
    return rc;
}
