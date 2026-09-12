/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * The recording clock and the region crop: the two pieces of the capture
 * engine that are pure arithmetic, so they are tested without a compositor.
 *
 * The clock is the Linux form of what RecorderSampleTiming does on macOS. There
 * the two capture sources (ScreenCaptureKit and AVCaptureSession) each have
 * their own clock and every buffer is converted onto the host clock, so picture
 * and sound share one timeline. Here both sources already timestamp on
 * CLOCK_MONOTONIC -- the compositor's `spa_meta_header.pts` and PipeWire's own
 * audio pts are the same clock -- so no conversion is needed and the only thing
 * that can pull them apart is a pause. One gap, subtracted from both, keeps
 * them together; two independently accumulated gaps would not.
 */

#include <stdbool.h>
#include <stdint.h>

#include "vorssaint_platform.h"

void vs_capture_clock_start(vs_capture_clock *clock, int64_t now_ns)
{
    if (!clock) return;
    clock->epoch_ns = now_ns;
    clock->paused_total_ns = 0;
    clock->pause_started_ns = 0;
    clock->paused = false;
}

void vs_capture_clock_pause(vs_capture_clock *clock, int64_t now_ns)
{
    if (!clock || clock->paused) return;
    clock->paused = true;
    clock->pause_started_ns = now_ns;
}

void vs_capture_clock_resume(vs_capture_clock *clock, int64_t now_ns)
{
    if (!clock || !clock->paused) return;
    int64_t gap = now_ns - clock->pause_started_ns;
    /* A backwards clock is not possible on CLOCK_MONOTONIC, but a caller may
     * drive this with its own numbers; refusing to subtract a negative gap
     * keeps the timeline monotonic whatever it is fed. */
    if (gap > 0) clock->paused_total_ns += gap;
    clock->paused = false;
    clock->pause_started_ns = 0;
}

int64_t vs_capture_clock_timeline(const vs_capture_clock *clock, int64_t capture_ns)
{
    if (!clock) return 0;
    int64_t paused = clock->paused_total_ns;
    if (clock->paused) {
        /* Asking about a buffer captured during the pause: the timeline stands
         * still, so everything in the gap maps to the instant the pause began. */
        int64_t so_far = capture_ns - clock->pause_started_ns;
        if (so_far > 0) paused += so_far;
    }
    int64_t t = capture_ns - clock->epoch_ns - paused;
    return t > 0 ? t : 0;
}

bool vs_capture_clamp_region(vs_rect *region, uint32_t width, uint32_t height)
{
    if (!region || width == 0 || height == 0) return false;
    int64_t x0 = region->x;
    int64_t y0 = region->y;
    int64_t x1 = x0 + region->width;
    int64_t y1 = y0 + region->height;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int64_t)width) x1 = width;
    if (y1 > (int64_t)height) y1 = height;
    if (x1 <= x0 || y1 <= y0) return false;
    region->x = (int32_t)x0;
    region->y = (int32_t)y0;
    region->width = (int32_t)(x1 - x0);
    region->height = (int32_t)(y1 - y0);
    return true;
}
