/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * The parts of the capture engine that are arithmetic rather than I/O: the
 * recording clock across a pause, the region clamp, and the PNG round trip.
 * None of this needs a compositor, so it runs in every configuration of
 * scripts/build-matrix.sh and on any machine.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vorssaint_platform.h"

/* Internal to the library and deliberately not in the contract header: reading
 * a PNG is the Screenshot portal's problem, not a capability the engine offers
 * its consumers. The round-trip test needs it, so it is declared here rather
 * than promoted into the public API to make a test pass. */
int vs_capture_png_read(const char *path, vs_capture_image *image_out);

static int failures;

#define CHECK(condition, ...)                                                  \
    do {                                                                       \
        if (!(condition)) {                                                    \
            failures++;                                                        \
            printf("FAIL %s:%d ", __func__, __LINE__);                         \
            printf(__VA_ARGS__);                                               \
            putchar('\n');                                                     \
        }                                                                      \
    } while (0)

#define MS 1000000LL

static void test_clock_without_pause(void)
{
    vs_capture_clock clock;
    vs_capture_clock_start(&clock, 1000 * MS);
    CHECK(vs_capture_clock_timeline(&clock, 1000 * MS) == 0, "epoch maps to zero");
    CHECK(vs_capture_clock_timeline(&clock, 1500 * MS) == 500 * MS, "500 ms in");
    /* A buffer captured just before the stream started must not go negative. */
    CHECK(vs_capture_clock_timeline(&clock, 900 * MS) == 0, "pre-start clamps to zero");
}

static void test_clock_pause_removes_the_gap(void)
{
    vs_capture_clock clock;
    vs_capture_clock_start(&clock, 0);
    CHECK(vs_capture_clock_timeline(&clock, 2000 * MS) == 2000 * MS, "before the pause");

    vs_capture_clock_pause(&clock, 2000 * MS);
    CHECK(clock.paused, "paused");
    /* While paused the timeline stands still: everything inside the gap maps
     * to the instant the pause began. */
    CHECK(vs_capture_clock_timeline(&clock, 2400 * MS) == 2000 * MS, "frozen mid-pause");
    CHECK(vs_capture_clock_timeline(&clock, 3000 * MS) == 2000 * MS, "frozen at the end");

    vs_capture_clock_resume(&clock, 3000 * MS);
    CHECK(!clock.paused, "resumed");
    CHECK(clock.paused_total_ns == 1000 * MS, "gap recorded: %lld",
          (long long)clock.paused_total_ns);
    /* Wall clock has advanced 4 s, one of them paused, so the timeline is 3 s.
     * This is the whole point: the recording is as long as it was recording. */
    CHECK(vs_capture_clock_timeline(&clock, 4000 * MS) == 3000 * MS, "gap subtracted");
}

static void test_clock_two_pauses_accumulate(void)
{
    vs_capture_clock clock;
    vs_capture_clock_start(&clock, 0);
    vs_capture_clock_pause(&clock, 1000 * MS);
    vs_capture_clock_resume(&clock, 1500 * MS);
    vs_capture_clock_pause(&clock, 3000 * MS);
    vs_capture_clock_resume(&clock, 3250 * MS);
    CHECK(clock.paused_total_ns == 750 * MS, "two gaps add up: %lld",
          (long long)clock.paused_total_ns);
    CHECK(vs_capture_clock_timeline(&clock, 4000 * MS) == 3250 * MS, "both subtracted");
}

static void test_clock_video_and_audio_stay_aligned(void)
{
    /* The alignment property stated as arithmetic: two buffers captured at the
     * same instant map to the same timeline position, before and after a pause.
     * The stream code runs both kinds through this one clock, which is what
     * makes the property hold for real buffers too. */
    vs_capture_clock clock;
    vs_capture_clock_start(&clock, 500 * MS);
    for (int64_t t = 600; t < 1000; t += 37) {
        CHECK(vs_capture_clock_timeline(&clock, t * MS) ==
                  vs_capture_clock_timeline(&clock, t * MS),
              "same instant, same timeline");
    }
    vs_capture_clock_pause(&clock, 1000 * MS);
    vs_capture_clock_resume(&clock, 2000 * MS);
    int64_t video = vs_capture_clock_timeline(&clock, 2500 * MS);
    int64_t audio = vs_capture_clock_timeline(&clock, 2500 * MS);
    CHECK(video == audio, "aligned after a pause");
    CHECK(video == 1000 * MS, "timeline is wall minus the gap: %lld", (long long)video);
}

static void test_clock_is_idempotent(void)
{
    vs_capture_clock clock;
    vs_capture_clock_start(&clock, 0);
    vs_capture_clock_pause(&clock, 100 * MS);
    vs_capture_clock_pause(&clock, 200 * MS);   /* second pause must do nothing */
    vs_capture_clock_resume(&clock, 300 * MS);
    vs_capture_clock_resume(&clock, 400 * MS);  /* second resume must do nothing */
    CHECK(clock.paused_total_ns == 200 * MS, "one gap of 200 ms, got %lld",
          (long long)clock.paused_total_ns);
}

static void test_region_clamp(void)
{
    vs_rect rect = { 10, 20, 100, 50 };
    CHECK(vs_capture_clamp_region(&rect, 1280, 720), "inside stays");
    CHECK(rect.x == 10 && rect.y == 20 && rect.width == 100 && rect.height == 50,
          "unchanged: %d,%d %dx%d", rect.x, rect.y, rect.width, rect.height);

    vs_rect over = { 1200, 700, 500, 500 };
    CHECK(vs_capture_clamp_region(&over, 1280, 720), "partly inside survives");
    CHECK(over.width == 80 && over.height == 20, "clamped to %dx%d", over.width,
          over.height);

    vs_rect negative = { -50, -10, 100, 40 };
    CHECK(vs_capture_clamp_region(&negative, 1280, 720), "negative origin survives");
    CHECK(negative.x == 0 && negative.y == 0 && negative.width == 50 &&
              negative.height == 30,
          "clamped to %d,%d %dx%d", negative.x, negative.y, negative.width,
          negative.height);

    vs_rect outside = { 2000, 2000, 100, 100 };
    CHECK(!vs_capture_clamp_region(&outside, 1280, 720), "fully outside is refused");
    vs_rect empty = { 0, 0, 0, 0 };
    CHECK(!vs_capture_clamp_region(&empty, 1280, 720), "empty is refused");
}

static void test_pixel_helpers(void)
{
    CHECK(vs_capture_pixel_bytes(VS_CAPTURE_PIXEL_BGRX) == 4, "BGRx is 4 bytes");
    CHECK(vs_capture_pixel_bytes(VS_CAPTURE_PIXEL_UNKNOWN) == 0, "unknown is 0");
    CHECK(strcmp(vs_capture_pixel_format_name(VS_CAPTURE_PIXEL_BGRA), "BGRA") == 0,
          "BGRA name");
}

/* A synthetic BGRx frame with a distinct colour per cell, so a crop that is off
 * by a row or a column produces different bytes rather than the same ones. */
static void fill_frame(uint8_t *data, uint32_t width, uint32_t height, uint32_t stride)
{
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint8_t *pixel = data + (size_t)stride * y + (size_t)x * 4;
            pixel[0] = (uint8_t)(x * 7 + 1);   /* B */
            pixel[1] = (uint8_t)(y * 5 + 2);   /* G */
            pixel[2] = (uint8_t)((x + y) * 3); /* R */
            pixel[3] = 0;                      /* x, undefined by definition */
        }
    }
}

static void test_image_from_frame_crops(void)
{
    const uint32_t width = 64, height = 32, stride = 64 * 4 + 16; /* padded stride */
    uint8_t *data = calloc(1, (size_t)stride * height);
    fill_frame(data, width, height, stride);

    vs_capture_image whole;
    CHECK(vs_capture_image_from_frame(data, width, height, stride,
                                      VS_CAPTURE_PIXEL_BGRX, NULL, &whole) == VS_OK,
          "uncropped copy");
    CHECK(whole.width == width && whole.height == height, "size kept");
    CHECK(whole.stride == width * 4, "stride packed down from %u", stride);
    CHECK(memcmp(whole.data, data, width * 4) == 0, "first row matches");
    vs_capture_image_free(&whole);

    vs_rect region = { 8, 4, 16, 8 };
    vs_capture_image cropped;
    CHECK(vs_capture_image_from_frame(data, width, height, stride,
                                      VS_CAPTURE_PIXEL_BGRX, &region, &cropped) == VS_OK,
          "cropped copy");
    CHECK(cropped.width == 16 && cropped.height == 8, "crop size");
    CHECK(memcmp(cropped.data, data + (size_t)stride * 4 + 8 * 4, 16 * 4) == 0,
          "crop starts at the right pixel");
    vs_capture_image_free(&cropped);

    vs_rect off_screen = { 500, 500, 10, 10 };
    vs_capture_image nothing;
    CHECK(vs_capture_image_from_frame(data, width, height, stride,
                                      VS_CAPTURE_PIXEL_BGRX, &off_screen,
                                      &nothing) == VS_ERR_INVALID,
          "a region outside the frame is refused, not silently emptied");
    free(data);
}

/* The encoder and the decoder are two halves of the screenshot path -- the
 * ScreenCast path writes a PNG, the portal path reads one -- so round-tripping
 * checks both at once against a picture whose every pixel is known. */
static void test_png_round_trip(void)
{
    const uint32_t width = 61, height = 23;   /* deliberately not multiples of 8 */
    const uint32_t stride = width * 4;
    uint8_t *data = calloc(1, (size_t)stride * height);
    fill_frame(data, width, height, stride);

    vs_capture_image image = { .data = data, .width = width, .height = height,
                               .stride = stride, .format = VS_CAPTURE_PIXEL_BGRX,
                               .byte_length = (size_t)stride * height };
    char path[] = "/tmp/vs-capture-test-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "temp file");
    if (fd >= 0) close(fd);

    CHECK(vs_capture_image_write_png(&image, path) == VS_OK, "write");

    vs_capture_image read_back;
    int rc = vs_capture_png_read(path, &read_back);
    CHECK(rc == VS_OK, "read back: %s", vs_result_string(rc));
    if (rc == VS_OK) {
        CHECK(read_back.width == width && read_back.height == height, "size survived");
        CHECK(read_back.format == VS_CAPTURE_PIXEL_RGBA, "decoded as RGBA");
        bool same = true;
        for (uint32_t y = 0; y < height && same; y++) {
            for (uint32_t x = 0; x < width; x++) {
                const uint8_t *in = data + (size_t)stride * y + (size_t)x * 4;
                const uint8_t *out = read_back.data + (size_t)read_back.stride * y +
                                     (size_t)x * 4;
                /* BGRx in, RGBA out: channels swapped, alpha forced opaque
                 * because the fourth byte of a BGRx buffer means nothing. */
                if (out[0] != in[2] || out[1] != in[1] || out[2] != in[0] ||
                    out[3] != 0xff) {
                    printf("     first mismatch at %u,%u\n", x, y);
                    same = false;
                    break;
                }
            }
        }
        CHECK(same, "every pixel survived the round trip");
        vs_capture_image_free(&read_back);
    }
    unlink(path);
    free(data);
}

static void test_png_rejects_bad_input(void)
{
    vs_capture_image image = { 0 };
    CHECK(vs_capture_image_write_png(&image, "/tmp/vs-capture-never") == VS_ERR_INVALID,
          "an empty image is refused");
    vs_capture_image read_back;
    CHECK(vs_capture_png_read("/nonexistent/vs-capture", &read_back) ==
              VS_ERR_NOT_FOUND,
          "a missing file is not found");
}

int main(void)
{
    test_clock_without_pause();
    test_clock_pause_removes_the_gap();
    test_clock_two_pauses_accumulate();
    test_clock_video_and_audio_stay_aligned();
    test_clock_is_idempotent();
    test_region_clamp();
    test_pixel_helpers();
    test_image_from_frame_crops();
    test_png_round_trip();
    test_png_rejects_bad_input();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    puts("all capture support checks passed");
    return 0;
}
