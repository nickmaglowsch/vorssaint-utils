/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * The parts of the window layer that need neither a display server nor a bus.
 */

#include "vs_window_internal.h"

#include <assert.h>
#include <stdio.h>

static int failures;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition);                    \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void test_result_strings(void)
{
    CHECK(strcmp(vs_result_string(VS_OK), "ok") == 0);
    /* Every documented code has its own sentence; a caller logging one must
     * never print "unknown error" for a code the header defines. */
    const int codes[] = { VS_ERR_UNSUPPORTED, VS_ERR_NOT_FOUND, VS_ERR_BACKEND, VS_ERR_TIMEOUT,
                          VS_ERR_NO_MEM,      VS_ERR_INVALID,   VS_ERR_NO_BACKEND,
                          VS_ERR_NOT_APPLIED };
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        CHECK(strcmp(vs_result_string(codes[i]), "unknown error") != 0);
    }
    CHECK(strcmp(vs_result_string(42), "unknown error") == 0);
}

static void test_string_field(void)
{
    char field[8];
    vs_window_set_string(field, sizeof(field), "abc");
    CHECK(strcmp(field, "abc") == 0);

    vs_window_set_string(field, sizeof(field), "0123456789");
    CHECK(strlen(field) == 7);
    CHECK(strcmp(field, "0123456") == 0);

    vs_window_set_string(field, sizeof(field), NULL);
    CHECK(field[0] == '\0');
}

static void test_info_init(void)
{
    vs_window_info info;
    memset(&info, 0xAA, sizeof(info));
    vs_window_info_init(&info);
    /* "Unknown" must not read as pid 0 (the kernel) or workspace 0 (the first
     * one); the switcher and autoQuit both branch on these. */
    CHECK(info.pid == -1);
    CHECK(info.workspace == -1);
    CHECK(info.flags == 0);
    CHECK(info.title[0] == '\0');
    CHECK(info.stacking_valid == false);
}

static void test_rect_tolerance(void)
{
    vs_rect wanted = { .x = 10, .y = 20, .width = 300, .height = 400 };
    CHECK(vs_rect_close(wanted, wanted, 0));

    vs_rect off_by_two = { .x = 12, .y = 18, .width = 302, .height = 398 };
    CHECK(vs_rect_close(off_by_two, wanted, 2));
    CHECK(!vs_rect_close(off_by_two, wanted, 1));

    /* One edge out of tolerance fails the whole rectangle: WindowLayoutService
     * treats a window that moved but did not resize as a refusal. */
    vs_rect wrong_height = { .x = 10, .y = 20, .width = 300, .height = 500 };
    CHECK(!vs_rect_close(wrong_height, wanted, 4));
}

static void test_vector(void)
{
    vs_window_vec vec = { 0 };
    for (int i = 0; i < 100; i++) {
        vs_window_info info;
        vs_window_info_init(&info);
        info.id = (vs_window_id)i;
        CHECK(vs_window_vec_push(&vec, &info));
    }
    CHECK(vec.count == 100);
    CHECK(vec.items[0].id == 0);
    CHECK(vec.items[99].id == 99);
    vs_window_vec_free(&vec);
    CHECK(vec.items == NULL);
    CHECK(vec.count == 0);
}

static void test_backend_names(void)
{
    const char *const *names = vs_window_backend_names();
    size_t count = 0;
    bool has_x11 = false, has_wlr = false, has_hyprland = false, has_kwin = false, has_gnome = false;
    for (; names[count]; count++) {
        if (strcmp(names[count], "x11") == 0) has_x11 = true;
        if (strcmp(names[count], "wlr") == 0) has_wlr = true;
        if (strcmp(names[count], "hyprland") == 0) has_hyprland = true;
        if (strcmp(names[count], "kwin") == 0) has_kwin = true;
        if (strcmp(names[count], "gnome") == 0) has_gnome = true;
    }
    CHECK(count == 5);
    CHECK(has_x11 && has_wlr && has_hyprland && has_kwin && has_gnome);
}

static void test_unknown_backend_is_rejected(void)
{
    int result = VS_OK;
    CHECK(vs_window_system_create("no-such-backend", &result) == NULL);
    CHECK(result == VS_ERR_INVALID);
}

int main(void)
{
    test_result_strings();
    test_string_field();
    test_info_init();
    test_rect_tolerance();
    test_vector();
    test_backend_names();
    test_unknown_backend_is_rejected();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all window support checks passed\n");
    return 0;
}
