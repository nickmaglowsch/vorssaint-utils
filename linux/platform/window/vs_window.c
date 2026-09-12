/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * Backend selection and the helpers every backend shares.
 */

#include "vs_window_internal.h"

#include <stdarg.h>

/* vs_result_string moved to linux/platform/vs_result.c when the audio
 * section became its second caller; it belongs to every concern, not to this
 * one. */

void vs_window_log(const char *format, ...)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("VS_WINDOW_DEBUG");
        enabled = (value && *value && strcmp(value, "0") != 0) ? 1 : 0;
    }
    if (!enabled) return;

    va_list args;
    va_start(args, format);
    fputs("vs-window: ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

void vs_window_set_string(char *field, size_t capacity, const char *value)
{
    if (capacity == 0) return;
    if (!value) {
        field[0] = '\0';
        return;
    }
    size_t length = strnlen(value, capacity - 1);
    memcpy(field, value, length);
    field[length] = '\0';
}

void vs_window_info_init(vs_window_info *info)
{
    memset(info, 0, sizeof(*info));
    info->pid = -1;
    info->workspace = -1;
}

bool vs_rect_close(vs_rect left, vs_rect right, int32_t tolerance)
{
    int32_t dx = left.x - right.x;
    int32_t dy = left.y - right.y;
    int32_t dw = left.width - right.width;
    int32_t dh = left.height - right.height;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dw < 0) dw = -dw;
    if (dh < 0) dh = -dh;
    return dx <= tolerance && dy <= tolerance && dw <= tolerance && dh <= tolerance;
}

bool vs_window_vec_push(vs_window_vec *vec, const vs_window_info *info)
{
    if (vec->count == vec->capacity) {
        size_t capacity = vec->capacity ? vec->capacity * 2 : 16;
        vs_window_info *items = realloc(vec->items, capacity * sizeof(*items));
        if (!items) return false;
        vec->items = items;
        vec->capacity = capacity;
    }
    vec->items[vec->count++] = *info;
    return true;
}

void vs_window_vec_free(vs_window_vec *vec)
{
    free(vec->items);
    vec->items = NULL;
    vec->count = 0;
    vec->capacity = 0;
}

uint32_t vs_window_capabilities_without_control(uint32_t capabilities)
{
    return capabilities & (uint32_t)(VS_WINDOW_CAN_LIST | VS_WINDOW_HAS_PREVIEWS);
}

void vs_window_free_list_default(vs_window_system *self, vs_window_info *windows, size_t count)
{
    (void)self;
    (void)count;
    free(windows);
}

/* ------------------------------------------------------------- selection */

typedef vs_window_system *(*vs_window_backend_ctor)(int *result_out);

typedef struct {
    const char *name;
    vs_window_backend_ctor create;
} vs_window_backend_entry;

/* Probe order. Hyprland and the two bridges come first because a session that
 * runs them also advertises WAYLAND_DISPLAY (and often DISPLAY, through
 * Xwayland), so the generic backends would otherwise win a session whose
 * compositor has a better channel. */
static const vs_window_backend_entry backends[] = {
    { "hyprland", vs_window_backend_hyprland_create },
    { "kwin", vs_window_backend_kwin_create },
    { "gnome", vs_window_backend_gnome_create },
    { "wlr", vs_window_backend_wlr_create },
    { "x11", vs_window_backend_x11_create },
};

static const size_t backend_count = sizeof(backends) / sizeof(backends[0]);

const char *const *vs_window_backend_names(void)
{
    static const char *names[sizeof(backends) / sizeof(backends[0]) + 1];
    for (size_t i = 0; i < backend_count; i++) names[i] = backends[i].name;
    names[backend_count] = NULL;
    return names;
}

vs_window_system *vs_window_system_create(const char *preferred, int *result_out)
{
    int result = VS_ERR_NO_BACKEND;

    if (!preferred || !*preferred) preferred = getenv("VS_WINDOW_BACKEND");

    if (preferred && *preferred) {
        for (size_t i = 0; i < backend_count; i++) {
            if (strcmp(backends[i].name, preferred) != 0) continue;
            int reason = VS_ERR_NO_BACKEND;
            vs_window_system *system = backends[i].create(&reason);
            if (!system) vs_window_log("backend %s refused: %s", preferred, vs_result_string(reason));
            if (result_out) *result_out = system ? VS_OK : reason;
            return system;
        }
        vs_window_log("no backend named %s", preferred);
        if (result_out) *result_out = VS_ERR_INVALID;
        return NULL;
    }

    for (size_t i = 0; i < backend_count; i++) {
        int reason = VS_ERR_NO_BACKEND;
        vs_window_system *system = backends[i].create(&reason);
        if (system) {
            vs_window_log("selected backend %s", system->name);
            if (result_out) *result_out = VS_OK;
            return system;
        }
        vs_window_log("backend %s not usable: %s", backends[i].name, vs_result_string(reason));
    }

    if (result_out) *result_out = result;
    return NULL;
}
