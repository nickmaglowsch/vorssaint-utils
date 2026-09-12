/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * A PNG writer in about two hundred lines, so the capture engine carries no
 * image library. It writes exactly one thing -- 8-bit RGBA, no interlace, one
 * IDAT -- because that is all a capture buffer ever is. libpng would add a
 * runtime dependency (PLAN.md section 7) to do the same job; zlib is already
 * underneath GLib, PipeWire and ffmpeg, so it costs nothing new.
 *
 * The alpha channel is forced opaque for the x-padded formats (BGRx/RGBx),
 * where the fourth byte is undefined rather than transparent: a compositor that
 * leaves it zero would otherwise produce a fully transparent screenshot.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "vorssaint_platform.h"

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int write_chunk(FILE *f, const char *type, const uint8_t *data, size_t length)
{
    uint8_t header[8];
    put_be32(header, (uint32_t)length);
    memcpy(header + 4, type, 4);
    if (fwrite(header, 1, 8, f) != 8) return -1;
    if (length && fwrite(data, 1, length, f) != length) return -1;

    uLong crc = crc32(0, (const Bytef *)type, 4);
    if (length) crc = crc32(crc, (const Bytef *)data, (uInt)length);
    uint8_t tail[4];
    put_be32(tail, (uint32_t)crc);
    return fwrite(tail, 1, 4, f) == 4 ? 0 : -1;
}

/* One source row into one RGBA row, and the PNG filter byte in front of it. */
static void convert_row(uint8_t *dst, const uint8_t *src, uint32_t width,
                        vs_capture_pixel_format format)
{
    switch (format) {
    case VS_CAPTURE_PIXEL_BGRX:
        for (uint32_t x = 0; x < width; x++) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = 0xff;
        }
        break;
    case VS_CAPTURE_PIXEL_BGRA:
        for (uint32_t x = 0; x < width; x++) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
        }
        break;
    case VS_CAPTURE_PIXEL_RGBX:
        for (uint32_t x = 0; x < width; x++) {
            dst[x * 4 + 0] = src[x * 4 + 0];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 2];
            dst[x * 4 + 3] = 0xff;
        }
        break;
    case VS_CAPTURE_PIXEL_RGBA:
        memcpy(dst, src, (size_t)width * 4);
        break;
    case VS_CAPTURE_PIXEL_UNKNOWN:
    default:
        memset(dst, 0, (size_t)width * 4);
        break;
    }
}

int vs_capture_image_write_png(const vs_capture_image *image, const char *path)
{
    if (!image || !image->data || !path) return VS_ERR_INVALID;
    if (image->width == 0 || image->height == 0) return VS_ERR_INVALID;
    if (vs_capture_pixel_bytes(image->format) != 4) return VS_ERR_INVALID;
    if ((size_t)image->stride * image->height > image->byte_length) return VS_ERR_INVALID;

    /* Filter byte 0 (None) plus RGBA, per row. Screen content compresses well
     * enough that a filter heuristic is not worth the complexity here; the
     * spike's 1280x720 frames land around 90 kB either way. */
    size_t raw_stride = (size_t)image->width * 4 + 1;
    size_t raw_size = raw_stride * image->height;
    uint8_t *raw = malloc(raw_size);
    if (!raw) return VS_ERR_NO_MEM;
    for (uint32_t y = 0; y < image->height; y++) {
        uint8_t *row = raw + raw_stride * y;
        row[0] = 0;
        convert_row(row + 1, image->data + (size_t)image->stride * y,
                    image->width, image->format);
    }

    uLongf compressed_size = compressBound((uLong)raw_size);
    uint8_t *compressed = malloc(compressed_size);
    if (!compressed) {
        free(raw);
        return VS_ERR_NO_MEM;
    }
    int zrc = compress2(compressed, &compressed_size, raw, (uLong)raw_size, 6);
    free(raw);
    if (zrc != Z_OK) {
        free(compressed);
        return VS_ERR_BACKEND;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(compressed);
        return VS_ERR_BACKEND;
    }
    static const uint8_t signature[8] = { 137, 'P', 'N', 'G', '\r', '\n', 26, '\n' };
    int rc = VS_OK;
    if (fwrite(signature, 1, 8, f) != 8) rc = VS_ERR_BACKEND;

    uint8_t ihdr[13];
    put_be32(ihdr, image->width);
    put_be32(ihdr + 4, image->height);
    ihdr[8] = 8;    /* bit depth */
    ihdr[9] = 6;    /* colour type: truecolour with alpha */
    ihdr[10] = 0;   /* deflate */
    ihdr[11] = 0;   /* adaptive filtering */
    ihdr[12] = 0;   /* no interlace */
    if (rc == VS_OK && write_chunk(f, "IHDR", ihdr, sizeof(ihdr)) < 0) rc = VS_ERR_BACKEND;
    if (rc == VS_OK && write_chunk(f, "IDAT", compressed, compressed_size) < 0) rc = VS_ERR_BACKEND;
    if (rc == VS_OK && write_chunk(f, "IEND", NULL, 0) < 0) rc = VS_ERR_BACKEND;

    free(compressed);
    if (fclose(f) != 0 && rc == VS_OK) rc = VS_ERR_BACKEND;
    return rc;
}
