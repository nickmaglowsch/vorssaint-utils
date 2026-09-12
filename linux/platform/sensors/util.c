/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * Rooted paths and the tiny file readers every other file here is built on.
 */

#include "vs_sensors_internal.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

bool vs_path(const vs_sensors_impl *impl, char *out, size_t out_len, const char *path)
{
    if (out_len == 0) {
        return false;
    }
    out[0] = '\0';
    if (path == NULL || path[0] != '/') {
        return false;
    }
    const char *root = (impl != NULL && impl->rooted) ? impl->root : "";
    int written = snprintf(out, out_len, "%s%s", root, path);
    if (written < 0 || (size_t)written >= out_len) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool vs_pathf(const vs_sensors_impl *impl, char *out, size_t out_len, const char *fmt, ...)
{
    char tail[VS_FULLPATH_MAX];
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(tail, sizeof tail, fmt, args);
    va_end(args);
    if (written < 0 || (size_t)written >= sizeof tail) {
        if (out_len > 0) {
            out[0] = '\0';
        }
        return false;
    }
    return vs_path(impl, out, out_len, tail);
}

bool vs_read_text(const char *path, char *buf, size_t buf_len)
{
    if (buf_len == 0) {
        return false;
    }
    buf[0] = '\0';
    FILE *file = fopen(path, "re");
    if (file == NULL) {
        return false;
    }
    size_t read = fread(buf, 1, buf_len - 1, file);
    fclose(file);
    buf[read] = '\0';
    while (read > 0 && (buf[read - 1] == '\n' || buf[read - 1] == '\r')) {
        buf[--read] = '\0';
    }
    return read > 0;
}

bool vs_read_u64(const char *path, uint64_t *out)
{
    char buf[64];
    if (!vs_read_text(path, buf, sizeof buf)) {
        return false;
    }
    char *end = NULL;
    unsigned long long value = strtoull(vs_trim(buf), &end, 10);
    if (end == buf) {
        return false;
    }
    *out = (uint64_t)value;
    return true;
}

bool vs_read_i64(const char *path, int64_t *out)
{
    char buf[64];
    if (!vs_read_text(path, buf, sizeof buf)) {
        return false;
    }
    char *end = NULL;
    long long value = strtoll(vs_trim(buf), &end, 10);
    if (end == buf) {
        return false;
    }
    *out = (int64_t)value;
    return true;
}

bool vs_read_double(const char *path, double *out)
{
    char buf[64];
    if (!vs_read_text(path, buf, sizeof buf)) {
        return false;
    }
    char *end = NULL;
    double value = strtod(vs_trim(buf), &end);
    if (end == buf) {
        return false;
    }
    *out = value;
    return true;
}

bool vs_path_exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0;
}

bool vs_dir_exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

FILE *vs_open_rooted(const vs_sensors_impl *impl, const char *path)
{
    char full[VS_FULLPATH_MAX];
    if (!vs_path(impl, full, sizeof full, path)) {
        return NULL;
    }
    return fopen(full, "re");
}

void vs_copy(char *dst, size_t dst_len, const char *src)
{
    if (dst_len == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t length = strlen(src);
    if (length >= dst_len) {
        length = dst_len - 1;
    }
    memcpy(dst, src, length);
    dst[length] = '\0';
}

char *vs_trim(char *s)
{
    if (s == NULL) {
        return s;
    }
    char *start = s;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }
    size_t length = strlen(start);
    while (length > 0 && isspace((unsigned char)start[length - 1])) {
        start[--length] = '\0';
    }
    if (start != s) {
        memmove(s, start, length + 1);
    }
    return s;
}

bool vs_contains_ci(const char *haystack, const char *needle)
{
    if (haystack == NULL || needle == NULL || needle[0] == '\0') {
        return false;
    }
    size_t needle_length = strlen(needle);
    for (const char *p = haystack; *p != '\0'; p++) {
        size_t i = 0;
        while (i < needle_length &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == needle_length) {
            return true;
        }
    }
    return false;
}

double vs_now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

double vs_rate(uint64_t previous, uint64_t current, double elapsed)
{
    if (elapsed <= 0 || current < previous) {
        return 0;
    }
    return (double)(current - previous) / elapsed;
}
