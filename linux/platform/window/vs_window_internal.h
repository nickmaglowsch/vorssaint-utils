/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * Shared between the window backends and the selector. Not installed.
 */

#ifndef VS_WINDOW_INTERNAL_H
#define VS_WINDOW_INTERNAL_H

#include "vorssaint_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Each backend's constructor. Returns NULL when this session cannot use it and
 *  sets `*result_out` to the reason. */
vs_window_system *vs_window_backend_x11_create(int *result_out);
vs_window_system *vs_window_backend_wlr_create(int *result_out);
vs_window_system *vs_window_backend_hyprland_create(int *result_out);
vs_window_system *vs_window_backend_kwin_create(int *result_out);
vs_window_system *vs_window_backend_gnome_create(int *result_out);

/** Copy into a fixed-size field, always NUL-terminating. */
void vs_window_set_string(char *field, size_t capacity, const char *value);

/** Zero a window record to the "nothing known" state: pid -1, workspace -1. */
void vs_window_info_init(vs_window_info *info);

/** `|left - right| <= tolerance` on all four edges. */
bool vs_rect_close(vs_rect left, vs_rect right, int32_t tolerance);

/** Growable window array used by every backend's `list`. */
typedef struct vs_window_vec {
    vs_window_info *items;
    size_t count;
    size_t capacity;
} vs_window_vec;

bool vs_window_vec_push(vs_window_vec *vec, const vs_window_info *info);
void vs_window_vec_free(vs_window_vec *vec);

/** Default `free_list` for backends whose lists are plain malloc'd arrays. */
void vs_window_free_list_default(vs_window_system *self, vs_window_info *windows, size_t count);

/** Log to stderr when VS_WINDOW_DEBUG is set in the environment. */
void vs_window_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif /* VS_WINDOW_INTERNAL_H */
