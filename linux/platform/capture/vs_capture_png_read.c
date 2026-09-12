/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * Reading the PNG the Screenshot portal hands back.
 *
 * The portal returns a URI to a file, not pixels, so the screenshot path needs
 * a decoder to meet the same contract as the ScreenCast path (a BGRx/RGBA
 * buffer with a size and a stride). This decodes the one shape screenshot
 * backends actually write -- 8-bit, non-interlaced, truecolour with or without
 * alpha -- and refuses everything else by name rather than guessing, because a
 * decoder that quietly mangles 16-bit or palette data would hand the editor a
 * wrong picture instead of an error the caller can fall back from.
 *
 * Checked against: grim 1.4.1 (what xdg-desktop-portal-wlr and the WP-02 shim
 * use), which writes colour type 6, 8 bits, no interlace.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "vs_capture_internal.h"

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

static int unfilter(uint8_t *rows, uint32_t width, uint32_t height, uint32_t bpp,
                    uint8_t *out, uint32_t out_stride)
{
    size_t raw_stride = (size_t)width * bpp + 1;
    const uint8_t *previous = NULL;
    for (uint32_t y = 0; y < height; y++) {
        uint8_t *row = rows + raw_stride * y;
        uint8_t filter = row[0];
        uint8_t *cur = row + 1;
        size_t length = (size_t)width * bpp;
        for (size_t i = 0; i < length; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0;
            int b = previous ? previous[i] : 0;
            int c = (previous && i >= bpp) ? previous[i - bpp] : 0;
            int value = cur[i];
            switch (filter) {
            case 0: break;
            case 1: value += a; break;
            case 2: value += b; break;
            case 3: value += (a + b) / 2; break;
            case 4: value += paeth(a, b, c); break;
            default: return VS_ERR_BACKEND;
            }
            cur[i] = (uint8_t)value;
        }
        /* Expand RGB to RGBA on the way out; RGBA copies straight across. */
        uint8_t *dst = out + (size_t)out_stride * y;
        if (bpp == 4) {
            memcpy(dst, cur, length);
        } else {
            for (uint32_t x = 0; x < width; x++) {
                dst[x * 4 + 0] = cur[x * 3 + 0];
                dst[x * 4 + 1] = cur[x * 3 + 1];
                dst[x * 4 + 2] = cur[x * 3 + 2];
                dst[x * 4 + 3] = 0xff;
            }
        }
        previous = cur;
    }
    return VS_OK;
}

int vs_capture_png_read(const char *path, vs_capture_image *image_out)
{
    if (!path || !image_out) return VS_ERR_INVALID;
    memset(image_out, 0, sizeof(*image_out));

    FILE *f = fopen(path, "rb");
    if (!f) return VS_ERR_NOT_FOUND;

    uint8_t signature[8];
    if (fread(signature, 1, 8, f) != 8 ||
        memcmp(signature, "\x89PNG\r\n\x1a\n", 8) != 0) {
        fclose(f);
        VS_LOG("screenshot file %s is not a PNG", path);
        return VS_ERR_UNSUPPORTED;
    }

    uint32_t width = 0, height = 0, bpp = 0;
    uint8_t *idat = NULL;
    size_t idat_length = 0;
    int rc = VS_ERR_BACKEND;

    for (;;) {
        uint8_t header[8];
        if (fread(header, 1, 8, f) != 8) break;
        uint32_t length = read_be32(header);
        char type[5] = { 0 };
        memcpy(type, header + 4, 4);
        if (length > (1u << 28)) break;

        if (strcmp(type, "IHDR") == 0) {
            uint8_t ihdr[13];
            if (length != 13 || fread(ihdr, 1, 13, f) != 13) break;
            width = read_be32(ihdr);
            height = read_be32(ihdr + 4);
            uint8_t depth = ihdr[8], colour = ihdr[9], interlace = ihdr[12];
            if (depth != 8 || interlace != 0 || (colour != 2 && colour != 6)) {
                VS_LOG("PNG %s is depth=%u colour=%u interlace=%u; only 8-bit "
                       "non-interlaced RGB/RGBA is decoded here",
                       path, depth, colour, interlace);
                rc = VS_ERR_UNSUPPORTED;
                break;
            }
            if (width == 0 || height == 0 || width > 65535 || height > 65535) break;
            bpp = colour == 6 ? 4 : 3;
        } else if (strcmp(type, "IDAT") == 0) {
            if (bpp == 0) break;
            uint8_t *grown = realloc(idat, idat_length + length);
            if (!grown) { rc = VS_ERR_NO_MEM; break; }
            idat = grown;
            if (length && fread(idat + idat_length, 1, length, f) != length) break;
            idat_length += length;
        } else if (strcmp(type, "IEND") == 0) {
            rc = VS_OK;
            break;
        } else {
            if (fseek(f, (long)length, SEEK_CUR) != 0) break;
        }
        if (fseek(f, 4, SEEK_CUR) != 0) break;   /* the chunk CRC */
    }
    fclose(f);

    if (rc != VS_OK || !idat || bpp == 0) {
        free(idat);
        return rc == VS_OK ? VS_ERR_BACKEND : rc;
    }

    size_t raw_stride = (size_t)width * bpp + 1;
    size_t raw_size = raw_stride * height;
    uint8_t *raw = malloc(raw_size);
    if (!raw) {
        free(idat);
        return VS_ERR_NO_MEM;
    }
    uLongf produced = (uLongf)raw_size;
    int zrc = uncompress(raw, &produced, idat, (uLong)idat_length);
    free(idat);
    if (zrc != Z_OK || produced != raw_size) {
        VS_LOG("PNG %s: inflate returned %d, %lu of %zu bytes",
               path, zrc, (unsigned long)produced, raw_size);
        free(raw);
        return VS_ERR_BACKEND;
    }

    uint32_t stride = width * 4;
    uint8_t *pixels = malloc((size_t)stride * height);
    if (!pixels) {
        free(raw);
        return VS_ERR_NO_MEM;
    }
    rc = unfilter(raw, width, height, bpp, pixels, stride);
    free(raw);
    if (rc != VS_OK) {
        free(pixels);
        return rc;
    }

    image_out->data = pixels;
    image_out->width = width;
    image_out->height = height;
    image_out->stride = stride;
    image_out->format = VS_CAPTURE_PIXEL_RGBA;
    image_out->byte_length = (size_t)stride * height;
    return VS_OK;
}
