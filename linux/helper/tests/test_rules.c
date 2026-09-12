/* Unit tests for the rules engine.
 *
 * Two kinds of test live here and they answer two different questions.
 *
 *  1. The *vector* tests replay, one for one, the assertions
 *     Tests/MetricsTests.swift makes about the macOS Support types, against
 *     the C that is supposed to reproduce them. Each block names the Swift
 *     symbol it comes from. If the C and the Swift ever disagree about a
 *     threshold, an edge or a tie-break, one of these fails. The counts are
 *     tabulated in docs/linux-port/RELAY_RULES.md.
 *
 *  2. The *relay* tests drive the same rules through the fake device backend,
 *     so the read -> rules -> write path the daemon runs is under test and
 *     not just the decision functions.
 *
 * Where a Swift vector has no Linux counterpart (a CoreGraphics event type, a
 * UserDefaults read, a source-code shape assertion about a Service file) it is
 * marked NOT PORTED in RELAY_RULES.md with the reason, rather than quietly
 * dropped. */
#include "device.h"
#include "rules.h"

#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;
static const char *current_case;

#define MS(x) ((uint64_t)(x) * 1000000ULL)

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  FAIL %s: ", current_case);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    failures++;
}

/* One ported Swift assertion. The message is the Swift one, verbatim, so a
 * failure here can be read next to the line it came from. */
static void expect_true(bool ok, const char *what)
{
    checks++;
    current_case = what;
    if (!ok)
        fail("assertion failed");
    else
        printf("  ok   %s\n", what);
}

static bool close_enough(double a, double b, double eps) { return fabs(a - b) < eps; }

/* The loop relay.c's live mode and helper.c's relay thread both run, including
 * the timer. Drains the fake's queue and returns. */
static void pump(input_backend *b, rules_state *st)
{
    for (int guard = 0; guard < 200000; guard++) {
        rules_event in, out[RULES_MAX_OUT];
        uint64_t ts, deadline = rules_deadline_ns(st);
        int rc, m;

        rc = b->read(b, &in, &ts, deadline);
        if (rc == DEV_READ_TIMEOUT) {
            m = rules_timer(st, deadline, out, RULES_MAX_OUT);
            for (int k = 0; k < m; k++)
                b->write(b, &out[k]);
            if (m == 0)
                break; /* a deadline that emits nothing cannot make progress */
            continue;
        }
        if (rc == DEV_READ_HOTPLUG)
            continue; /* the source set changed; nothing to relay this round */
        if (rc != DEV_READ_EVENT)
            break;

        for (;;) {
            uint64_t d = rules_deadline_ns(st);
            if (d == 0 || d > ts)
                break;
            m = rules_timer(st, d, out, RULES_MAX_OUT);
            for (int k = 0; k < m; k++)
                b->write(b, &out[k]);
            if (m == 0)
                break;
        }
        m = rules_process(st, &in, ts, out, RULES_MAX_OUT);
        for (int k = 0; k < m; k++)
            b->write(b, &out[k]);
    }
}

/* What the helper's Enable(true) does: make a backend, then claim. */
static input_backend *enable_cycle(void)
{
    input_backend *b = device_fake_new();
    char err[128];
    b->open(b, true, err, sizeof(err));
    return b;
}

/* What the helper's Enable(false) does: close, which releases every grab. */
static void disable_cycle(input_backend *b) { b->close(b); }

static input_backend *run(const rules_config *cfg, void (*script)(input_backend *, uint64_t),
                          uint64_t base)
{
    input_backend *b = enable_cycle();
    rules_state st;

    rules_init(&st, cfg);
    script(b, base);
    pump(b, &st);
    return b;
}

static void expect(input_backend *b, const char *name, const rules_event *want, size_t n_want)
{
    size_t got = device_fake_sink_count(b);

    checks++;
    current_case = name;
    if (got != n_want) {
        fail("expected %zu output events, got %zu", n_want, got);
        for (size_t i = 0; i < got; i++) {
            const rules_event *g = device_fake_sink(b, i);
            printf("        got[%zu] %s/%s/%d\n", i, libevdev_event_type_get_name(g->type),
                   libevdev_event_code_get_name(g->type, g->code) ?: "?", g->value);
        }
    } else {
        bool bad = false;
        for (size_t i = 0; i < n_want; i++) {
            const rules_event *g = device_fake_sink(b, i);
            if (g->type != want[i].type || g->code != want[i].code || g->value != want[i].value) {
                fail("event %zu: want %s/%s/%d, got %s/%s/%d", i,
                     libevdev_event_type_get_name(want[i].type),
                     libevdev_event_code_get_name(want[i].type, want[i].code) ?: "?",
                     want[i].value, libevdev_event_type_get_name(g->type),
                     libevdev_event_code_get_name(g->type, g->code) ?: "?", g->value);
                bad = true;
            }
        }
        if (!bad)
            printf("  ok   %s\n", name);
    }
    b->close(b);
}

#define E(c, v) {EV_KEY, (c), (v), 0, 0}
#define R(c, v) {EV_REL, (c), (v), 0, 0}

/* ========================================================================== *
 * rule 1: keyboard_debounce -- KeyboardDebounceSupport.swift
 * ========================================================================== */

static rules_state KDB;

static bool kdb_down(uint16_t code, double at_s, bool repeat, const rules_kdb_config *cfg)
{
    return rules_kdb_should_suppress(&KDB, code, repeat, true, (uint64_t)(at_s * 1e9 + 0.5), cfg);
}

static bool kdb_up(uint16_t code, double at_s, const rules_kdb_config *cfg)
{
    return rules_kdb_should_suppress(&KDB, code, false, false, (uint64_t)(at_s * 1e9 + 0.5), cfg);
}

static void test_keyboard_debounce_vectors(void)
{
    rules_kdb_config c50 = {true, 50, {0}, {0}, 0};
    rules_kdb_config c10 = {true, 10, {0}, {0}, 0};
    rules_kdb_config cdef = {true, RULES_KDB_DEFAULT_MS, {0}, {0}, 0};
    rules_kdb_config per = {true, 20, {37, 40}, {100, 0}, 2};

    printf("\nkeyboard_debounce vectors (KeyboardDebounceState.shouldSuppress)\n");
    rules_init(&KDB, NULL);

    expect_true(!kdb_down(37, 10.00, false, &c50), "debounce accepts the first key press");
    expect_true(!kdb_up(37, 10.01, &c50), "debounce accepts key release");
    expect_true(kdb_down(37, 10.03, false, &c50),
                "debounce suppresses same-key bounce after release");
    expect_true(!kdb_down(37, 10.06, false, &c50),
                "debounce accepts same-key press after the release window");
    expect_true(!kdb_down(37, 10.07, true, &c50), "debounce leaves key auto-repeat alone");

    rules_kdb_reset(&KDB);
    expect_true(!kdb_down(0, 40.000, false, &c10), "debounce 10 ms accepts the first fast key press");
    (void)kdb_up(0, 40.004, &c10);
    expect_true(kdb_down(0, 40.009, false, &c10),
                "debounce 10 ms suppresses same-key bounce inside the release window");
    expect_true(!kdb_down(0, 40.014, false, &c10),
                "debounce 10 ms accepts the same key at the release boundary");

    rules_kdb_reset(&KDB);
    expect_true(!kdb_down(0, 45.000, false, &cdef),
                "debounce 5 ms default accepts the first fast key press");
    (void)kdb_up(0, 45.001, &cdef);
    expect_true(kdb_down(0, 45.005, false, &cdef),
                "debounce 5 ms default suppresses same-key bounce inside the release window");
    expect_true(!kdb_down(0, 45.006, false, &cdef),
                "debounce 5 ms default accepts the same key at the release boundary");

    rules_kdb_reset(&KDB);
    expect_true(!kdb_down(37, 50.000, false, &c10), "debounce accepts normal phrase first letter");
    (void)kdb_up(37, 50.020, &c10);
    expect_true(!kdb_down(14, 50.025, false, &c10), "debounce accepts normal phrase next letter");
    (void)kdb_up(14, 50.045, &c10);
    expect_true(!kdb_down(17, 50.050, false, &c10),
                "debounce accepts normal phrase repeated-letter first press");
    (void)kdb_up(17, 50.070, &c10);
    expect_true(!kdb_down(17, 50.110, false, &c10),
                "debounce accepts normal phrase repeated-letter second press");

    rules_kdb_reset(&KDB);
    expect_true(!kdb_down(0, 60.000, false, &c10),
                "debounce accepts the first key in an alternating pattern");
    (void)kdb_up(0, 60.004, &c10);
    expect_true(!kdb_down(11, 60.006, false, &c10),
                "debounce accepts a different key inside another key's window");
    (void)kdb_up(11, 60.009, &c10);
    expect_true(!kdb_down(0, 60.011, false, &c10),
                "debounce accepts a same-key press after another key was accepted");

    rules_kdb_reset(&KDB);
    expect_true(!kdb_down(0, 70.000, false, &c10),
                "debounce accepts the first key before duplicate down");
    expect_true(kdb_down(0, 70.004, false, &c10),
                "debounce suppresses non-repeat duplicate down while the key is still down");
    expect_true(!kdb_up(0, 70.020, &c10), "debounce still passes the release after a duplicate down");

    rules_kdb_reset(&KDB);
    expect_true(!kdb_down(0, 75.000, false, &c10), "debounce accepts a key before a missing release");
    expect_true(!kdb_down(0, 75.020, false, &c10),
                "debounce accepts a same-key press after the window even if release was missed");

    rules_kdb_reset(&KDB);
    expect_true(!kdb_down(0, 80.000, false, &c10),
                "debounce accepts the first key before an out-of-order event");
    (void)kdb_up(0, 80.010, &c10);
    expect_true(!kdb_down(0, 79.990, false, &c10),
                "debounce resets same-key state when event timestamps move backward");

    rules_kdb_reset(&KDB);
    (void)kdb_down(37, 20.00, false, &per);
    (void)kdb_up(37, 20.01, &per);
    expect_true(kdb_down(37, 20.06, false, &per),
                "debounce per-key window overrides the global window");
    (void)kdb_down(40, 30.00, false, &per);
    (void)kdb_up(40, 30.005, &per);
    expect_true(!kdb_down(40, 30.006, false, &per),
                "debounce per-key zero disables filtering for that key");

    {
        rules_kdb_config enc = {true, 20, {37, 40}, {100, 0}, 2};
        char buf[128];
        rules_kdb_encode_key_windows(&enc, buf, sizeof(buf));
        expect_true(strcmp(buf, "37:100,40:0") == 0,
                    "debounce key windows encode in stable key order");
    }
    {
        rules_kdb_config dec;
        memset(&dec, 0, sizeof(dec));
        dec.global_window_ms = RULES_KDB_DEFAULT_MS;
        rules_kdb_decode_key_windows("37:100,bad,40:0,99:999", &dec);
        expect_true(dec.n_key_windows == 3 && dec.key_code[0] == 37 && dec.key_window_ms[0] == 100 &&
                        dec.key_code[1] == 40 && dec.key_window_ms[1] == 0 &&
                        dec.key_code[2] == 99 &&
                        dec.key_window_ms[2] == RULES_KDB_DEFAULT_MS,
                    "debounce key windows decode and sanitize stored values");
    }
    expect_true(rules_kdb_sanitize_window(5) == 5 && rules_kdb_sanitize_window(500) == 500 &&
                    rules_kdb_sanitize_window(501) == RULES_KDB_DEFAULT_MS &&
                    rules_kdb_sanitize_window(-1) == RULES_KDB_DEFAULT_MS,
                "keyboard debounce keeps only its allowed settings range");

    /* Stale state, which the Swift has and the macOS suite does not exercise:
     * a gap longer than five seconds describes a different typing session. */
    rules_kdb_reset(&KDB);
    expect_true(!kdb_down(0, 100.000, false, &c50), "a fresh key press before a long idle");
    (void)kdb_up(0, 100.010, &c50);
    expect_true(!kdb_down(0, 106.000, false, &c50),
                "state older than the five second stale gap is thrown away");
}

/* ========================================================================== *
 * rule 2: mouse_click_debounce -- MouseClickDebounceSupport.swift
 * ========================================================================== */

static rules_state MCD;
static rules_mcd_config CLICK25 = {true, 25};

static bool click(int button, rules_click_kind k, uint64_t ms, const rules_mcd_config *cfg)
{
    return rules_mcd_should_suppress(&MCD, button, k, MS(ms), cfg);
}

static void test_mouse_click_debounce_vectors(void)
{
    rules_mcd_config off = {false, 25};

    printf("\nmouse_click_debounce vectors (MouseClickDebounceState.shouldSuppress)\n");
    rules_init(&MCD, NULL);

    expect_true(rules_mcd_button_for(BTN_LEFT) == 0 && rules_mcd_button_for(BTN_RIGHT) == 1 &&
                    rules_mcd_button_for(BTN_MIDDLE) == 2 &&
                    rules_mcd_button_for(BTN_SIDE) == -1 && rules_mcd_button_for(KEY_A) == -1,
                "click debounce owns only primary, secondary and middle button events");
    expect_true(!click(0, RULES_CLICK_DOWN, 100, &CLICK25) &&
                    !click(0, RULES_CLICK_DRAG, 103, &CLICK25) &&
                    !click(0, RULES_CLICK_UP, 105, &CLICK25),
                "a healthy click passes Down, drag and its final Up without delay");
    expect_true(click(0, RULES_CLICK_DOWN, 120, &CLICK25) &&
                    click(0, RULES_CLICK_DRAG, 122, &CLICK25) &&
                    click(0, RULES_CLICK_UP, 125, &CLICK25),
                "a bounce click suppresses its Down, drag and matching Up");
    expect_true(!click(0, RULES_CLICK_DOWN, 130, &CLICK25),
                "a click on the filter boundary starts a new accepted press");
    expect_true(click(0, RULES_CLICK_DOWN, 132, &CLICK25),
                "a duplicate Down cannot create a second accepted press");
    expect_true(!click(0, RULES_CLICK_UP, 140, &CLICK25),
                "the Up after a duplicate Down still releases the accepted press");
    expect_true(!click(1, RULES_CLICK_DOWN, 145, &CLICK25) &&
                    !click(1, RULES_CLICK_UP, 150, &CLICK25),
                "each standard mouse button owns independent debounce state");
    expect_true(click(1, RULES_CLICK_DOWN, 160, &CLICK25),
                "a second button click inside its own release window is filtered");

    rules_mcd_reset(&MCD);
    expect_true(!click(1, RULES_CLICK_UP, 165, &CLICK25),
                "reset passes an unmatched final Up instead of leaving a button stuck");
    expect_true(!click(0, RULES_CLICK_DOWN, 200, &CLICK25) &&
                    !click(0, RULES_CLICK_UP, 205, &CLICK25),
                "a fresh click is accepted before an out-of-order event");
    expect_true(!click(0, RULES_CLICK_DOWN, 190, &CLICK25),
                "a timestamp moving backwards resets stale button ownership");

    rules_mcd_reset(&MCD);
    expect_true(!click(2, RULES_CLICK_DOWN, 250, &CLICK25) &&
                    !click(2, RULES_CLICK_UP, 255, &CLICK25) &&
                    click(2, RULES_CLICK_DOWN, 265, &CLICK25) &&
                    !click(2, RULES_CLICK_UP, 266, &off),
                "turning the filter off preserves the final release of a suppressed click");
    rules_mcd_reset(&MCD);
    expect_true(!click(0, RULES_CLICK_DOWN, 300, &off) && !click(0, RULES_CLICK_UP, 301, &off) &&
                    !click(0, RULES_CLICK_DOWN, 302, &off),
                "disabled click debounce is a complete pass-through");
    expect_true(rules_mcd_sanitize_window(5) == 5 && rules_mcd_sanitize_window(100) == 100 &&
                    rules_mcd_sanitize_window(0) == RULES_MCD_DEFAULT_MS &&
                    rules_mcd_sanitize_window(101) == RULES_MCD_DEFAULT_MS,
                "mouse click debounce keeps only its conservative settings range");
}

/* ========================================================================== *
 * rule 3: scroll_invert -- ScrollWheelSupport.inversionPlan
 * ========================================================================== */

static bool plan_is(rules_inversion_plan p, bool v, bool h)
{
    return p.vertical == v && p.horizontal == h;
}

static void test_scroll_invert_vectors(void)
{
    printf("\nscroll_invert vectors (ScrollWheelSupport.inversionPlan)\n");
    expect_true(plan_is(rules_scroll_inversion_plan(true, false, false, true, false), true, false),
                "vertical wheel movement follows only the vertical direction setting");
    expect_true(plan_is(rules_scroll_inversion_plan(false, true, false, true, false), false, false),
                "a horizontal wheel stays unchanged when only vertical inversion is on");
    expect_true(plan_is(rules_scroll_inversion_plan(true, false, true, true, false), false, false),
                "Shift-directed wheel movement follows the horizontal setting");
    expect_true(plan_is(rules_scroll_inversion_plan(true, false, true, false, true), true, false),
                "horizontal inversion flips the vertical source tick while Shift redirects it");
    expect_true(plan_is(rules_scroll_inversion_plan(true, true, true, true, false), true, false),
                "a genuine two-axis event keeps each axis independent even with Shift held");
    expect_true(plan_is(rules_scroll_inversion_plan(true, false, false, false, true), false, false),
                "a continuous wheel stays vertical because Shift does not redirect that event type");
}

/* ========================================================================== *
 * rule 4: smooth_scroll -- SmoothScrollSupport
 * ========================================================================== */

static void test_smooth_scroll_vectors(void)
{
    const double FI = RULES_SMOOTH_FRAME_INTERVAL_S;
    const int RESP = RULES_SMOOTH_RESPONSE_DEFAULT;
    double px, carry;

    printf("\nsmooth_scroll vectors (SmoothScrollSupport)\n");

    expect_true(rules_smooth_ticks(1, 1.0) == 1.0,
                "a classic wheel tick reads the same from either delta field");
    expect_true(rules_smooth_ticks(0, 0.25) == 0.25,
                "high-resolution wheels keep their fractional ticks when the integer field "
                "truncates to zero");
    expect_true(rules_smooth_ticks(-2, 0) == -2,
                "a zero fixed-point field falls back to the integer line delta");

    {
        rules_smooth_axis v = {0, 0, 0}, h = {0, 0, 0};
        double dv, dh;

        rules_smooth_engine_add(&v, 40);
        expect_true(v.remaining == 40, "one wheel tick queues one step of glide");
        rules_smooth_engine_add(&v, 80);
        rules_smooth_engine_add(&h, 20);
        expect_true(v.remaining == 120 && h.remaining == 20,
                    "same-direction input adds to what is left on each axis");
        rules_smooth_engine_add(&v, -40);
        expect_true(v.remaining == -40 && h.remaining == 20,
                    "reversing one axis abandons only that axis's old tail");
        dv = rules_smooth_engine_advance(&v, FI, RESP);
        dh = rules_smooth_engine_advance(&h, FI, RESP);
        expect_true(dv < 0 && dh > 0,
                    "the first frame after a reversal moves in the new direction immediately");
    }

    {
        double v, h;
        rules_smooth_axes(2, 0, true, &v, &h);
        expect_true(v == 0 && h == 2, "Shift routes a vertical wheel tick sideways keeping its sign");
        rules_smooth_axes(-2, 0, true, &v, &h);
        expect_true(v == 0 && h == -2,
                    "the Shift redirect keeps the sign in the other direction too");
        rules_smooth_axes(2, 0, false, &v, &h);
        expect_true(v == 2 && h == 0, "a wheel tick without Shift keeps its vertical axis");
        rules_smooth_axes(2, -1, true, &v, &h);
        expect_true(v == 2 && h == -1,
                    "Shift preserves a wheel event that already carries horizontal movement");
    }

    {
        double def = rules_smooth_frame_delta(100, FI, RESP);
        expect_true(fabs(def - 18) < 0.5,
                    "the registered response keeps the former default's initial movement");
        expect_true(rules_smooth_frame_delta(-100, FI, RESP) < 0,
                    "negative glides emit negative frames");
        expect_true(rules_smooth_frame_delta(0.8, FI, RESP) == 0.8,
                    "small leftovers flush in one final frame");
        expect_true(rules_smooth_frame_delta(3, FI, RESP) == 1 &&
                        rules_smooth_frame_delta(3, FI / 2, RESP) == 0.5,
                    "the time-based tail keeps moving at the former default cadence");
        expect_true(rules_smooth_frame_delta(100, FI, RULES_SMOOTH_RESPONSE_MAX) > def &&
                        rules_smooth_frame_delta(100, FI, RULES_SMOOTH_RESPONSE_MIN) < def,
                    "response changes how quickly the glide follows the wheel");
        expect_true(rules_smooth_frame_delta(100, 1, RESP) ==
                        rules_smooth_frame_delta(100, RULES_SMOOTH_MAX_FRAME_INTERVAL_S, RESP),
                    "a stalled run loop cannot dump the entire tail in one frame");
        expect_true(rules_smooth_frame_delta(0, FI, RESP) == 0,
                    "no remaining distance emits nothing");
        expect_true(rules_smooth_frame_delta(40, NAN, RESP) == 0,
                    "an invalid elapsed time cannot corrupt the glide");
    }

    expect_true(rules_smooth_sanitize_step(0) == 40, "an unset step falls back to the default");
    expect_true(rules_smooth_sanitize_step(500) == 100, "the step clamps to its range");
    expect_true(rules_smooth_sanitize_response(-1) == RULES_SMOOTH_RESPONSE_MIN &&
                    rules_smooth_sanitize_response(500) == RULES_SMOOTH_RESPONSE_MAX,
                "response clamps damaged preferences to its range");

    {
        rules_smooth_axis a = {0, 0, 0}, b = {0, 0, 0};
        double ad_v = 0, bd_v = 0, ad_h = 0, bd_h = 0;
        rules_smooth_axis ah = {0, 0, 0}, bh = {0, 0, 0};

        rules_smooth_engine_add(&a, 80);
        rules_smooth_engine_add(&ah, -80);
        rules_smooth_engine_add(&b, 80);
        rules_smooth_engine_add(&bh, -80);
        for (int i = 0; i < 12; i++) {
            ad_v += rules_smooth_engine_advance(&a, 1.0 / 60.0, RESP);
            ad_h += rules_smooth_engine_advance(&ah, 1.0 / 60.0, RESP);
        }
        for (int i = 0; i < 24; i++) {
            bd_v += rules_smooth_engine_advance(&b, 1.0 / 120.0, RESP);
            bd_h += rules_smooth_engine_advance(&bh, 1.0 / 120.0, RESP);
        }
        expect_true(close_enough(ad_v, bd_v, 1e-6) && close_enough(ad_h, bd_h, 1e-6) &&
                        close_enough(a.remaining, b.remaining, 1e-6) &&
                        close_enough(ah.remaining, bh.remaining, 1e-6),
                    "equal elapsed time produces the same glide at 60 and 120 Hz");
    }

    expect_true(rules_smooth_continuous_distance(4.0, 40, 40) == 40,
                "the default step travels the same distance the event asked for");
    expect_true(rules_smooth_continuous_distance(4.0, 12, 40) == 12,
                "the point field wins, so no assumption about points per line is made");
    expect_true(rules_smooth_continuous_distance(4.0, 40, 20) == 20,
                "a shorter step halves the distance of a continuous wheel");
    expect_true(rules_smooth_continuous_distance(4.0, 40, 100) == 100,
                "a longer step stretches the distance of a continuous wheel");
    expect_true(rules_smooth_continuous_distance(-0.5, -5, 40) == -5,
                "direction survives the conversion");
    expect_true(close_enough(rules_smooth_continuous_distance(0.35, 0, 40), 3.5, 1e-9),
                "a movement below one whole point still glides");
    expect_true(rules_smooth_continuous_distance(0, 12, 40) == 12,
                "a driver that fills in only whole points still glides");
    expect_true(rules_smooth_continuous_distance(0, 0, 40) == 0,
                "an empty event asks for no distance");
    expect_true(rules_smooth_continuous_distance(NAN, 0, 40) == 0,
                "a nonsense delta asks for no distance");

    {
        bool ok = true;
        for (int i = 0; i < 3; i++) {
            double step = (double[]){20, 40, 100}[i];
            double d = rules_smooth_continuous_distance(4.0, 40, step);
            rules_smooth_axis a = {0, 0, 0};
            rules_smooth_engine_add(&a, d);
            if (a.remaining != d)
                ok = false;
        }
        expect_true(ok, "the step scales a continuous wheel exactly once");
    }

    {
        rules_smooth_axis v = {0, 0, 0}, h = {0, 0, 0};
        double sv = 0, sh = 0;
        rules_smooth_engine_add(&v, 40.4);
        rules_smooth_engine_add(&h, -17.3);
        for (int i = 0; i < 600 && (v.remaining != 0 || h.remaining != 0); i++) {
            sv += rules_smooth_engine_advance(&v, 1.0 / 120.0, RULES_SMOOTH_RESPONSE_MIN);
            sh += rules_smooth_engine_advance(&h, 1.0 / 120.0, RULES_SMOOTH_RESPONSE_MIN);
        }
        expect_true(v.remaining == 0 && h.remaining == 0 && close_enough(sv, 40.4, 1e-6) &&
                        close_enough(sh, -17.3, 1e-6),
                    "the engine spends the exact distance on both axes");
    }

    {
        double total = 0;
        carry = 0;
        for (int i = 0; i < 10; i++) {
            rules_smooth_whole_pixels(0.6, carry, &px, &carry);
            total += px;
        }
        expect_true(close_enough(total + carry, 6, 1e-6),
                    "ten six-tenths of a pixel are all still there, posted or waiting");
        expect_true(total >= 5, "never more than one pixel is left waiting");
    }
    rules_smooth_whole_pixels(0.4, 0, &px, &carry);
    expect_true(px == 0, "a fraction alone posts nothing yet");
    expect_true(carry == 0.4, "the fraction is kept for the next frame");
    rules_smooth_whole_pixels(-1.5, 0, &px, &carry);
    expect_true(px == -1, "negative frames keep their whole pixels");
    expect_true(carry == -0.5, "negative frames carry their fraction");
    rules_smooth_whole_pixels(INFINITY, 0, &px, &carry);
    expect_true(px == 0, "an impossible frame posts nothing");
    expect_true(rules_smooth_final_pixels(0.4, 0.3) == 1,
                "the landing frame spends the leftover instead of dropping it");
    expect_true(rules_smooth_final_pixels(-0.4, -0.3) == -1,
                "the landing frame spends it in either direction");
    expect_true(rules_smooth_final_pixels(0.2, 0) == 0,
                "a landing frame with almost nothing left posts nothing");
    expect_true(rules_smooth_final_pixels(INFINITY, 0) == 0,
                "an impossible landing frame posts nothing");
    expect_true(rules_smooth_carry(0.6, 5) == 0.6, "leftovers survive while the direction holds");
    expect_true(rules_smooth_carry(0.6, -5) == 0, "reversing direction drops the leftovers");
    expect_true(rules_smooth_carry(0.6, 0) == 0.6, "an empty event leaves the leftovers alone");
}

/* ========================================================================== *
 * rule 5: super_key -- SuperKeySupport.State / .soloEffect
 * ========================================================================== */

static void test_super_key_vectors(void)
{
    const uint64_t TH = MS(500);
    rules_sk_state st;
    bool rep = false;

    printf("\nsuper_key vectors (SuperKeySupport.State.decide, .soloEffect)\n");

    memset(&st, 0, sizeof(st));
    expect_true(rules_sk_decide(&st, RULES_SKE_OTHER_KEY, false, false, 0, TH, NULL) ==
                    RULES_SK_PASS,
                "with the key up, typing is untouched");
    expect_true(rules_sk_decide(&st, RULES_SKE_TRIGGER_DOWN, false, false, 0, TH, NULL) ==
                    RULES_SK_SWALLOW,
                "the key itself never reaches an app");
    expect_true(st.is_held &&
                    rules_sk_decide(&st, RULES_SKE_OTHER_KEY, false, false, 0, TH, NULL) ==
                        RULES_SK_ADD_MODIFIERS &&
                    rules_sk_decide(&st, RULES_SKE_OTHER_KEY, false, false, 0, TH, NULL) ==
                        RULES_SK_ADD_MODIFIERS,
                "every key pressed while it is held carries the configured modifiers");
    expect_true(rules_sk_decide(&st, RULES_SKE_TRIGGER_UP, false, false, 1, TH, NULL) ==
                        RULES_SK_SWALLOW &&
                    !st.is_held,
                "releasing after a combination does nothing on its own");

    memset(&st, 0, sizeof(st));
    (void)rules_sk_decide(&st, RULES_SKE_TRIGGER_DOWN, false, false, 0, TH, NULL);
    expect_true(rules_sk_decide(&st, RULES_SKE_OTHER_KEY, false, false, 0, TH, NULL) ==
                    RULES_SK_ADD_MODIFIERS,
                "a press with no release still carries the modifiers while it stands");
    rules_sk_reset(&st);
    expect_true(rules_sk_decide(&st, RULES_SKE_OTHER_KEY, false, false, 0, TH, NULL) ==
                    RULES_SK_PASS,
                "letting go of a press whose release was lost gives typing back");
    expect_true(rules_sk_decide(&st, RULES_SKE_TRIGGER_UP, false, false, 1, TH, NULL) ==
                    RULES_SK_SWALLOW,
                "a release arriving after the press was let go does nothing");

    memset(&st, 0, sizeof(st));
    (void)rules_sk_decide(&st, RULES_SKE_TRIGGER_DOWN, false, false, 1000000000ULL, TH, NULL);
    expect_true(rules_sk_decide(&st, RULES_SKE_TRIGGER_UP, false, false, 1499999999ULL, TH, &rep) ==
                        RULES_SK_SOLO_TAP &&
                    !rep,
                "a quick no-repeat press is the solo tap");
    (void)rules_sk_decide(&st, RULES_SKE_TRIGGER_DOWN, false, false, 2000000000ULL, TH, NULL);
    expect_true(rules_sk_decide(&st, RULES_SKE_TRIGGER_DOWN, true, false, 2100000000ULL, TH,
                                NULL) == RULES_SK_SWALLOW &&
                    rules_sk_decide(&st, RULES_SKE_TRIGGER_UP, false, false, 2500000000ULL, TH,
                                    &rep) == RULES_SK_SOLO_HOLD &&
                    rep,
                "a repeated press held long enough is a repeated solo hold");
    (void)rules_sk_decide(&st, RULES_SKE_TRIGGER_DOWN, false, false, 3000000000ULL, TH, NULL);
    (void)rules_sk_decide(&st, RULES_SKE_OTHER_MODIFIER, false, false, 3000000000ULL, TH, NULL);
    expect_true(rules_sk_decide(&st, RULES_SKE_TRIGGER_UP, false, false, 4000000000ULL, TH, NULL) ==
                    RULES_SK_SWALLOW,
                "holding it together with another modifier is not a tap either");

    memset(&st, 0, sizeof(st));
    (void)rules_sk_decide(&st, RULES_SKE_TRIGGER_DOWN, false, false, 6000000000ULL, TH, NULL);
    expect_true(rules_sk_decide(&st, RULES_SKE_TRIGGER_UP, false, false, 6500000000ULL, TH, &rep) ==
                        RULES_SK_SOLO_HOLD &&
                    !rep,
                "a no-repeat press at the hold threshold is a solo hold");

    memset(&st, 0, sizeof(st));
    (void)rules_sk_decide(&st, RULES_SKE_TRIGGER_DOWN, false, false, 8000000000ULL, TH, NULL);
    expect_true(rules_sk_decide(&st, RULES_SKE_TRIGGER_DOWN, true, false, 8100000000ULL, TH,
                                NULL) == RULES_SK_SWALLOW &&
                    rules_sk_decide(&st, RULES_SKE_TRIGGER_UP, false, false, 8499999999ULL, TH,
                                    &rep) == RULES_SK_SOLO_TAP &&
                    rep,
                "a fast repeat remains a quick press and records the repeat");

    memset(&st, 0, sizeof(st));
    (void)rules_sk_decide(&st, RULES_SKE_TRIGGER_DOWN, false, true, 7000000000ULL, TH, NULL);
    expect_true(st.is_held && !st.is_alone &&
                    rules_sk_decide(&st, RULES_SKE_TRIGGER_UP, false, false, 7500000000ULL, TH,
                                    NULL) == RULES_SK_SWALLOW,
                "a pre-held modifier cancels solo action while keeping Superkey held");

    expect_true(rules_sk_solo_effect(RULES_SK_NONE, false, false) == RULES_SK_NONE &&
                    rules_sk_solo_effect(RULES_SK_ESCAPE, false, false) == RULES_SK_ESCAPE &&
                    rules_sk_solo_effect(RULES_SK_ESCAPE, true, true) == RULES_SK_NONE &&
                    rules_sk_solo_effect(RULES_SK_CAPSLOCK, true, false) == RULES_SK_CAPSLOCK &&
                    rules_sk_solo_effect(RULES_SK_CAPSLOCK, false, true) == RULES_SK_NONE,
                "existing solo actions keep their tap, hold and repeat behavior");
    expect_true(rules_sk_solo_effect(RULES_SK_KEY, false, false) == RULES_SK_KEY &&
                    rules_sk_solo_effect(RULES_SK_KEY, false, true) == RULES_SK_KEY &&
                    rules_sk_solo_effect(RULES_SK_KEY, true, false) == RULES_SK_CAPSLOCK &&
                    rules_sk_solo_effect(RULES_SK_KEY, true, true) == RULES_SK_CAPSLOCK,
                "the configured-key action switches on a quick press and reserves a hold for "
                "Caps Lock");

    /* SuperKeyMappingGuard.hasMappingConflict, in the terms this engine has. */
    {
        rules_config c;
        rules_config_defaults(&c);
        c.sk.enabled = true;
        expect_true(!rules_sk_source_conflict(&c, KEY_CAPSLOCK),
                    "an unclaimed source key may be taken");
        expect_true(rules_sk_source_conflict(&c, KEY_LEFTCTRL),
                    "a source that is also one of this rule's own modifiers is refused");
        c.qp.enabled = true;
        c.qp.quit.enabled = true;
        expect_true(rules_sk_source_conflict(&c, KEY_Q),
                    "a source quit protection already answers is refused");
        c.qp.enabled = false;
        c.mbs.enabled = true;
        c.mbs.n_bind = 1;
        c.mbs.bind[0].input = BTN_SIDE;
        c.mbs.bind[0].key = KEY_F13;
        expect_true(rules_sk_source_conflict(&c, KEY_F13),
                    "a source a mouse chord already emits is refused");
        expect_true(rules_sk_source_conflict(&c, 0), "keycode zero is never a valid source");
    }
}

/* ========================================================================== *
 * rule 6: mouse_button_shortcut -- MouseButtonShortcutSupport,
 *                                  MouseSpacesGestureSupport
 * ========================================================================== */

static void test_mouse_button_vectors(void)
{
    const double STEP = RULES_GESTURE_SPACE_STEP;
    const double OVER = RULES_GESTURE_OVERVIEW_STEP;
    const double COOL = RULES_GESTURE_COOLDOWN_S;

    printf("\nmouse_button_shortcut vectors (MouseButtonShortcutSupport,"
           " MouseSpacesGestureSupport)\n");

    expect_true(rules_mbs_can_map(BTN_SIDE) && rules_mbs_can_map(BTN_TASK) &&
                    rules_mbs_can_map(RULES_TILT_LEFT) && rules_mbs_can_map(RULES_TILT_RIGHT) &&
                    !rules_mbs_can_map(BTN_LEFT) && !rules_mbs_can_map(BTN_RIGHT) &&
                    !rules_mbs_can_map(BTN_MIDDLE) && !rules_mbs_can_map(BTN_TASK + 1) &&
                    !rules_mbs_can_map(-3),
                "only extra buttons and both side-wheel directions can carry a shortcut");

    {
        rules_tilt_gate g;
        memset(&g, 0, sizeof(g));
        expect_true(rules_tilt_should_fire(&g, RULES_TILT_LEFT, 0) &&
                        !rules_tilt_should_fire(&g, RULES_TILT_LEFT, 10000000ULL) &&
                        rules_tilt_should_fire(&g, RULES_TILT_RIGHT, 20000000ULL) &&
                        !rules_tilt_should_fire(&g, RULES_TILT_LEFT, 30000000ULL) &&
                        !rules_tilt_should_fire(&g, RULES_TILT_RIGHT, 40000000ULL),
                    "one wheel burst fires each deliberate direction exactly once");
        expect_true(rules_tilt_should_fire(&g, RULES_TILT_LEFT, 300000001ULL),
                    "a quiet gap arms the next side-wheel gesture");
        rules_tilt_reset(&g);
        expect_true(rules_tilt_should_fire(&g, RULES_TILT_RIGHT, 1),
                    "stopping the tap clears the side-wheel gesture state");
    }

    {
        rules_gesture_state g;
        memset(&g, 0, sizeof(g));
        expect_true(rules_gesture_advance(&g, STEP - 1, 0, 0) == RULES_GESTURE_NONE &&
                        rules_gesture_advance(&g, 1, 0, 0.1) == RULES_GESTURE_SPACE_RIGHT,
                    "one whole step to the right moves one Space to the right, and not a pixel "
                    "sooner");
        memset(&g, 0, sizeof(g));
        expect_true(rules_gesture_advance(&g, -STEP, 0, 0) == RULES_GESTURE_SPACE_LEFT,
                    "the same step to the left moves the other way");
        memset(&g, 0, sizeof(g));
        expect_true(rules_gesture_advance(&g, 0, -OVER + 1, 0) == RULES_GESTURE_NONE &&
                        rules_gesture_advance(&g, 0, -1, 0.1) == RULES_GESTURE_OVERVIEW,
                    "dragging up opens the overview once the vertical step is behind it");
        memset(&g, 0, sizeof(g));
        expect_true(rules_gesture_advance(&g, 0, OVER, 0) == RULES_GESTURE_APP_OVERVIEW,
                    "dragging down opens the per-app overview, the way the trackpad swipe does");

        memset(&g, 0, sizeof(g));
        {
            bool any = false;
            for (int i = 1; i <= 12; i++)
                if (rules_gesture_advance(&g, 5, 3, i * 0.02) != RULES_GESTURE_NONE)
                    any = true;
            expect_true(!any && !g.did_fire && g.axis == 0,
                        "a drag that stays under both steps does nothing, so the press is still a "
                        "plain click");
        }

        memset(&g, 0, sizeof(g));
        expect_true(rules_gesture_advance(&g, NAN, INFINITY, 0) == RULES_GESTURE_NONE &&
                        !g.did_fire,
                    "a pointer position that is not a number moves nothing");

        memset(&g, 0, sizeof(g));
        expect_true(rules_gesture_advance(&g, STEP, 0, 0) == RULES_GESTURE_SPACE_RIGHT &&
                        rules_gesture_advance(&g, STEP, 0, COOL / 2) == RULES_GESTURE_NONE &&
                        rules_gesture_advance(&g, 1, 0, COOL + 0.01) == RULES_GESTURE_SPACE_RIGHT,
                    "a held drag repeats one Space per step, never faster than the slide "
                    "animation");

        memset(&g, 0, sizeof(g));
        (void)rules_gesture_advance(&g, STEP, 0, 0);
        (void)rules_gesture_advance(&g, STEP * 5, 0, COOL / 2);
        expect_true(rules_gesture_advance(&g, 0, 0, COOL + 0.01) == RULES_GESTURE_SPACE_RIGHT &&
                        rules_gesture_advance(&g, 0, 0, COOL * 2 + 0.02) == RULES_GESTURE_NONE,
                    "a fast flick banks one further Space change, not a burst that outlives the "
                    "hand");

        memset(&g, 0, sizeof(g));
        expect_true(rules_gesture_advance(&g, STEP, 0, 0) == RULES_GESTURE_SPACE_RIGHT &&
                        rules_gesture_advance(&g, 0, -OVER * 3, COOL + 0.01) ==
                            RULES_GESTURE_NONE &&
                        g.axis == 1,
                    "a press that started switching Spaces never throws up the overview halfway "
                    "through");

        memset(&g, 0, sizeof(g));
        expect_true(rules_gesture_advance(&g, 0, -OVER, 0) == RULES_GESTURE_OVERVIEW &&
                        rules_gesture_advance(&g, 0, -OVER * 3, 1) == RULES_GESTURE_NONE &&
                        g.did_fire,
                    "an overview is a toggle, so one press opens it exactly once");

        memset(&g, 0, sizeof(g));
        expect_true(rules_gesture_advance(&g, STEP, OVER * 2, 0) == RULES_GESTURE_APP_OVERVIEW,
                    "a step past both thresholds is read as the axis that went furthest past its "
                    "own");
    }

    expect_true(rules_gesture_resolved(RULES_GESTURE_SPACE_RIGHT, true) ==
                        RULES_GESTURE_SPACE_LEFT &&
                    rules_gesture_resolved(RULES_GESTURE_SPACE_LEFT, true) ==
                        RULES_GESTURE_SPACE_RIGHT &&
                    rules_gesture_resolved(RULES_GESTURE_OVERVIEW, true) ==
                        RULES_GESTURE_OVERVIEW &&
                    rules_gesture_resolved(RULES_GESTURE_APP_OVERVIEW, true) ==
                        RULES_GESTURE_APP_OVERVIEW &&
                    rules_gesture_resolved(RULES_GESTURE_SPACE_RIGHT, false) ==
                        RULES_GESTURE_SPACE_RIGHT &&
                    rules_gesture_resolved(RULES_GESTURE_SPACE_LEFT, false) ==
                        RULES_GESTURE_SPACE_LEFT,
                "the workspace can follow the hand instead of the pointer, and the overviews never "
                "swap");
    expect_true(rules_gesture_can_bind(BTN_SIDE) && rules_gesture_can_bind(BTN_TASK) &&
                    !rules_gesture_can_bind(BTN_MIDDLE) && !rules_gesture_can_bind(BTN_TASK + 1) &&
                    !rules_gesture_can_bind(RULES_TILT_LEFT),
                "the drag lives on an extra button, never on a side-wheel tick there is no way to "
                "hold");
}

/* ========================================================================== *
 * rule 7: quit_protection -- QuitProtectionSupport
 * ========================================================================== */

static void test_quit_protection_vectors(void)
{
    rules_qp_shortcut sc;

    printf("\nquit_protection vectors (QuitProtectionSupport)\n");

    expect_true(rules_qp_sanitize_hold(100) == 250, "quit protection clamps a too-short hold "
                                                    "duration");
    expect_true(rules_qp_sanitize_hold(3000) == 2000,
                "quit protection clamps an overly long hold duration");
    expect_true(rules_qp_sanitize_hold(NAN) == RULES_QP_HOLD_DEFAULT_MS &&
                    rules_qp_sanitize_hold(INFINITY) == RULES_QP_HOLD_DEFAULT_MS,
                "quit protection replaces non-finite hold durations with its default");
    expect_true(rules_qp_sanitize_double(100) == 200,
                "quit protection clamps a too-short double-press interval");
    expect_true(rules_qp_sanitize_double(3000) == 1500,
                "quit protection clamps an overly long double-press interval");
    expect_true(rules_qp_sanitize_double(NAN) == RULES_QP_DOUBLE_DEFAULT_MS &&
                    rules_qp_sanitize_double(-INFINITY) == RULES_QP_DOUBLE_DEFAULT_MS,
                "quit protection replaces non-finite double-press intervals with its default");
    expect_true(rules_qp_within_double(1000000000ULL, 2500000000ULL, 1500),
                "a second press on the interval edge confirms");
    expect_true(!rules_qp_within_double(1000000000ULL, 2500000001ULL, 1500),
                "a second press after the interval starts a new confirmation");

    memset(&sc, 0, sizeof(sc));
    expect_true(rules_qp_scope_allows(RULES_QP_SCOPE_ALL, "", &sc),
                "all-app scope protects even an app with no identifier");
    snprintf(sc.exception[0], RULES_APPID_MAX, "com.example.editor");
    sc.n_exceptions = 1;
    expect_true(rules_qp_scope_allows(RULES_QP_SCOPE_SELECTED, "com.example.editor", &sc),
                "selected-only scope protects a selected bundle");
    expect_true(!rules_qp_scope_allows(RULES_QP_SCOPE_SELECTED, "com.example.other", &sc),
                "selected-only scope leaves an unselected bundle alone");
    expect_true(!rules_qp_scope_allows(RULES_QP_SCOPE_EXCEPT, "com.example.editor", &sc),
                "all-except scope leaves a selected bundle alone");
    expect_true(rules_qp_scope_allows(RULES_QP_SCOPE_EXCEPT, "com.example.other", &sc),
                "all-except scope protects an unselected bundle");

    expect_true(rules_qp_is_base_shortcut(KEY_Q, true, false, false, false, KEY_Q),
                "plain Ctrl+Q is recognized");
    expect_true(!rules_qp_is_base_shortcut(KEY_Q, true, false, true, false, KEY_Q),
                "Shift-Ctrl+Q is not mistaken for plain Ctrl+Q");
    expect_true(!rules_qp_is_base_shortcut(KEY_Q, false, false, false, false, KEY_Q),
                "Q on its own is not the shortcut");
    expect_true(rules_qp_is_extra_shortcut(KEY_Q, true, false, true, false, KEY_Q, KEY_LEFTSHIFT),
                "Shift-Ctrl+Q is recognized as an extra-modifier confirmation");
    expect_true(rules_qp_is_extra_shortcut(KEY_W, true, true, false, false, KEY_W, KEY_LEFTALT),
                "Alt-Ctrl+W is recognized as an extra-modifier confirmation");
    expect_true(!rules_qp_is_extra_shortcut(KEY_Q, true, true, false, false, KEY_Q, KEY_LEFTSHIFT),
                "an unrelated modifier combination is not protected");
}

/* ========================================================================== *
 * The relay path: the same rules through the fake device backend
 * ========================================================================== */

/* rule 1 through the device layer. */
static void s_chatter(input_backend *b, uint64_t t)
{
    device_fake_push(b, EV_KEY, KEY_E, 1, t);
    device_fake_push(b, EV_KEY, KEY_E, 0, t + MS(30));
    device_fake_push(b, EV_KEY, KEY_E, 1, t + MS(38)); /* bounce */
    device_fake_push(b, EV_KEY, KEY_E, 0, t + MS(44));
}

static void s_autorepeat(input_backend *b, uint64_t t)
{
    device_fake_push(b, EV_KEY, KEY_DOWN, 1, t);
    device_fake_push(b, EV_KEY, KEY_DOWN, 2, t + MS(250));
    device_fake_push(b, EV_KEY, KEY_DOWN, 2, t + MS(283));
    device_fake_push(b, EV_KEY, KEY_DOWN, 0, t + MS(316));
}

static void s_click_bounce(input_backend *b, uint64_t t)
{
    device_fake_push_dev(b, RULES_DEV_MOUSE, 1, EV_KEY, BTN_LEFT, 1, t);
    device_fake_push_dev(b, RULES_DEV_MOUSE, 1, EV_KEY, BTN_LEFT, 0, t + MS(30));
    device_fake_push_dev(b, RULES_DEV_MOUSE, 1, EV_KEY, BTN_LEFT, 1, t + MS(40)); /* bounce */
    device_fake_push_dev(b, RULES_DEV_MOUSE, 1, EV_KEY, BTN_LEFT, 0, t + MS(48));
    device_fake_push_dev(b, RULES_DEV_MOUSE, 1, EV_KEY, BTN_LEFT, 1, t + MS(400));
    device_fake_push_dev(b, RULES_DEV_MOUSE, 1, EV_KEY, BTN_LEFT, 0, t + MS(460));
}

static void s_sk_tap(input_backend *b, uint64_t t)
{
    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 1, t);
    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 0, t + MS(50));
}

static void s_sk_hold_modifier(input_backend *b, uint64_t t)
{
    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 1, t);
    device_fake_push(b, EV_KEY, KEY_C, 1, t + MS(80));
    device_fake_push(b, EV_KEY, KEY_C, 0, t + MS(140));
    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 0, t + MS(180));
}

static void s_sk_solo_hold(input_backend *b, uint64_t t)
{
    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 1, t);
    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 0, t + MS(600));
}

static void s_unrelated(input_backend *b, uint64_t t)
{
    device_fake_push(b, EV_KEY, KEY_A, 1, t);
    device_fake_push(b, EV_KEY, KEY_A, 0, t + MS(60));
}

static void test_relay_path(void)
{
    rules_config cfg;
    uint64_t T = MS(1000);

    printf("\nthe relay path: the same rules through the fake device backend\n");

    /* keyboard_debounce. The suppressed press's own release still goes out,
     * which is the macOS behaviour; the kernel drops a release for a key that
     * is not down, so it changes nothing downstream. */
    rules_config_defaults(&cfg);
    cfg.kdb.enabled = true;
    cfg.kdb.global_window_ms = 40;
    {
        rules_event w[] = {E(KEY_E, 1), E(KEY_E, 0), E(KEY_E, 0)};
        expect(run(&cfg, s_chatter, T), "a bounce inside the window is dropped, its release is not",
               w, 3);
    }
    {
        rules_event w[] = {E(KEY_DOWN, 1), E(KEY_DOWN, 2), E(KEY_DOWN, 2), E(KEY_DOWN, 0)};
        expect(run(&cfg, s_autorepeat, T), "kernel autorepeat passes through", w, 4);
    }
    {
        rules_event w[] = {E(KEY_A, 1), E(KEY_A, 0)};
        expect(run(&cfg, s_unrelated, T), "unrelated keys pass through", w, 2);
    }

    /* mouse_click_debounce. */
    rules_config_defaults(&cfg);
    cfg.mcd.enabled = true;
    cfg.mcd.window_ms = 25;
    {
        rules_event w[] = {E(BTN_LEFT, 1), E(BTN_LEFT, 0), E(BTN_LEFT, 1), E(BTN_LEFT, 0)};
        expect(run(&cfg, s_click_bounce, T), "a bounce click loses both its Down and its Up", w, 4);
    }

    /* super_key. */
    rules_config_defaults(&cfg);
    cfg.sk.enabled = true;
    cfg.sk.led = false;
    cfg.sk.n_mods = 2;
    cfg.sk.mods[0] = KEY_LEFTCTRL;
    cfg.sk.mods[1] = KEY_LEFTALT;
    cfg.sk.tap_action = RULES_SK_ESCAPE;
    {
        rules_event w[] = {E(KEY_ESC, 1), E(KEY_ESC, 0)};
        expect(run(&cfg, s_sk_tap, T), "a 50 ms tap runs the solo action", w, 2);
    }
    {
        rules_event w[] = {E(KEY_ESC, 1), E(KEY_ESC, 0)};
        expect(run(&cfg, s_sk_solo_hold, T),
               "a 600 ms solo hold runs the same action for the escape setting", w, 2);
    }
    {
        rules_event w[] = {E(KEY_LEFTCTRL, 1), E(KEY_LEFTALT, 1), E(KEY_C, 1),
                           E(KEY_C, 0),       E(KEY_LEFTALT, 0), E(KEY_LEFTCTRL, 0)};
        expect(run(&cfg, s_sk_hold_modifier, T),
               "a key pressed while it is held carries the whole combination", w, 6);
    }
    /* The caps-lock action drives the LED on the source device. */
    cfg.sk.tap_action = RULES_SK_CAPSLOCK;
    cfg.sk.led = true;
    {
        rules_event w[] = {E(KEY_CAPSLOCK, 1), E(KEY_CAPSLOCK, 0), {EV_LED, LED_CAPSL, 1, 0, 0}};
        expect(run(&cfg, s_sk_tap, T), "the caps lock action lights the source keyboard's lamp", w,
               3);
    }

    /* scroll_invert, and that it is per device class. */
    rules_config_defaults(&cfg);
    cfg.scroll.enabled = true;
    cfg.scroll.vertical = true;
    cfg.scroll.mouse = true;
    cfg.scroll.touchpad = false;
    {
        input_backend *b = enable_cycle();
        rules_state st;
        rules_event w[] = {R(REL_WHEEL, -1), R(REL_WHEEL, 1), R(REL_HWHEEL, 1)};

        rules_init(&st, &cfg);
        device_fake_push_dev(b, RULES_DEV_MOUSE, 1, EV_REL, REL_WHEEL, 1, T);
        device_fake_push_dev(b, RULES_DEV_TOUCHPAD, 2, EV_REL, REL_WHEEL, 1, T + MS(10));
        device_fake_push_dev(b, RULES_DEV_MOUSE, 1, EV_REL, REL_HWHEEL, 1, T + MS(20));
        pump(b, &st);
        expect(b, "the mouse wheel is inverted, the touchpad and the unset axis are not", w, 3);
    }

    /* smooth_scroll: one notch in, a glide of hi-res steps out that adds up to
     * exactly one notch, with the low-resolution axis reported alongside. */
    rules_config_defaults(&cfg);
    cfg.smooth.enabled = true;
    cfg.smooth.mouse = true;
    {
        input_backend *b = enable_cycle();
        rules_state st;
        int32_t hi_total = 0, lo_total = 0;
        size_t n, frames = 0;
        bool only_wheel = true;

        rules_init(&st, &cfg);
        device_fake_push_dev(b, RULES_DEV_MOUSE, 1, EV_REL, REL_WHEEL, 1, T);
        pump(b, &st);
        n = device_fake_sink_count(b);
        for (size_t i = 0; i < n; i++) {
            const rules_event *e = device_fake_sink(b, i);
            if (e->type != EV_REL)
                only_wheel = false;
            else if (e->code == REL_WHEEL_HI_RES) {
                hi_total += e->value;
                frames++;
            } else if (e->code == REL_WHEEL) {
                lo_total += e->value;
            } else {
                only_wheel = false;
            }
        }
        checks++;
        current_case = "one notch glides out as hi-res steps totalling exactly one notch";
        if (only_wheel && hi_total == RULES_HI_RES_PER_NOTCH && lo_total == 1 && frames > 3)
            printf("  ok   %s (%zu frames, %d hi-res units, %d notch)\n", current_case, frames,
                   hi_total, lo_total);
        else
            fail("only_wheel=%d hi=%d lo=%d frames=%zu", only_wheel, hi_total, lo_total, frames);
        b->close(b);
    }

    /* mouse_button_shortcut: a bound button presses a chord. */
    rules_config_defaults(&cfg);
    cfg.mbs.enabled = true;
    cfg.mbs.n_bind = 1;
    cfg.mbs.bind[0].input = BTN_SIDE;
    cfg.mbs.bind[0].n_mods = 1;
    cfg.mbs.bind[0].mods[0] = KEY_LEFTCTRL;
    cfg.mbs.bind[0].key = KEY_T;
    {
        input_backend *b = enable_cycle();
        rules_state st;
        rules_event w[] = {E(KEY_LEFTCTRL, 1), E(KEY_T, 1), E(KEY_T, 0), E(KEY_LEFTCTRL, 0)};

        rules_init(&st, &cfg);
        device_fake_push_dev(b, RULES_DEV_MOUSE, 1, EV_KEY, BTN_SIDE, 1, T);
        device_fake_push_dev(b, RULES_DEV_MOUSE, 1, EV_KEY, BTN_SIDE, 0, T + MS(90));
        pump(b, &st);
        expect(b, "a bound extra button presses and releases its chord", w, 4);
    }

    /* The gesture button: a press that never becomes a drag is still a click. */
    rules_config_defaults(&cfg);
    cfg.mbs.enabled = true;
    cfg.mbs.gesture_button = BTN_EXTRA;
    {
        input_backend *b = enable_cycle();
        rules_state st;
        /* The motion is not swallowed -- the cursor has to keep moving -- so
         * it reaches the output before the replayed click. */
        rules_event w[] = {R(REL_X, 3), E(BTN_EXTRA, 1), E(BTN_EXTRA, 0)};

        rules_init(&st, &cfg);
        device_fake_push_dev(b, RULES_DEV_MOUSE, 1, EV_KEY, BTN_EXTRA, 1, T);
        device_fake_push_dev(b, RULES_DEV_MOUSE, 1, EV_REL, REL_X, 3, T + MS(20));
        device_fake_push_dev(b, RULES_DEV_MOUSE, 1, EV_KEY, BTN_EXTRA, 0, T + MS(60));
        pump(b, &st);
        expect(b, "a held button that never travels far enough replays as a plain click", w, 3);
    }
    {
        input_backend *b = enable_cycle();
        rules_state st;
        rules_notice nt;
        bool got_left = false;
        size_t emitted;

        rules_init(&st, &cfg);
        device_fake_push_dev(b, RULES_DEV_MOUSE, 1, EV_KEY, BTN_EXTRA, 1, T);
        device_fake_push_dev(b, RULES_DEV_MOUSE, 1, EV_REL, REL_X, -250, T + MS(40));
        device_fake_push_dev(b, RULES_DEV_MOUSE, 1, EV_KEY, BTN_EXTRA, 0, T + MS(90));
        pump(b, &st);
        emitted = device_fake_sink_count(b);
        while (rules_notice_pop(&st, &nt))
            if (strstr(nt.detail, "\"workspace_left\""))
                got_left = true;
        checks++;
        current_case = "a drag past the step raises a workspace notice and eats the click";
        /* The motion itself still reaches the output: the cursor must keep
         * moving. What must not appear is the button press. */
        if (got_left && emitted == 1 && device_fake_sink(b, 0)->type == EV_REL)
            printf("  ok   %s\n", current_case);
        else
            fail("notice=%d emitted=%zu", got_left, emitted);
        b->close(b);
    }

    /* quit_protection, hold mode, through the relay and its timer. */
    rules_config_defaults(&cfg);
    cfg.qp.enabled = true;
    cfg.qp.quit.enabled = true;
    cfg.qp.quit.mode = RULES_QP_HOLD;
    cfg.qp.quit.hold_ms = 800;
    {
        input_backend *b = enable_cycle();
        rules_state st;
        rules_event w[] = {E(KEY_LEFTCTRL, 1), E(KEY_LEFTCTRL, 0)};

        rules_init(&st, &cfg);
        device_fake_push(b, EV_KEY, KEY_LEFTCTRL, 1, T);
        device_fake_push(b, EV_KEY, KEY_Q, 1, T + MS(20));
        device_fake_push(b, EV_KEY, KEY_Q, 0, T + MS(300)); /* let go early */
        device_fake_push(b, EV_KEY, KEY_LEFTCTRL, 0, T + MS(320));
        pump(b, &st);
        expect(b, "Ctrl+Q released before the deadline never reaches the application", w, 2);
    }
    {
        input_backend *b = enable_cycle();
        rules_state st;
        rules_event w[] = {E(KEY_LEFTCTRL, 1), E(KEY_Q, 1), E(KEY_Q, 0), E(KEY_LEFTCTRL, 0)};

        rules_init(&st, &cfg);
        device_fake_push(b, EV_KEY, KEY_LEFTCTRL, 1, T);
        device_fake_push(b, EV_KEY, KEY_Q, 1, T + MS(20));
        device_fake_push(b, EV_KEY, KEY_Q, 0, T + MS(1200)); /* held past 800 ms */
        device_fake_push(b, EV_KEY, KEY_LEFTCTRL, 0, T + MS(1220));
        pump(b, &st);
        expect(b, "Ctrl+Q held past the deadline is emitted by the timer", w, 4);
    }
    /* The per-app scope, which is what SetContext exists for. */
    cfg.qp.quit.scope = RULES_QP_SCOPE_EXCEPT;
    snprintf(cfg.qp.quit.exception[0], RULES_APPID_MAX, "org.gnome.Terminal");
    cfg.qp.quit.n_exceptions = 1;
    {
        input_backend *b = enable_cycle();
        rules_state st;
        rules_context ctx;
        rules_event w[] = {E(KEY_LEFTCTRL, 1), E(KEY_Q, 1), E(KEY_Q, 0), E(KEY_LEFTCTRL, 0)};

        rules_init(&st, &cfg);
        memset(&ctx, 0, sizeof(ctx));
        snprintf(ctx.focused_app_id, sizeof(ctx.focused_app_id), "org.gnome.Terminal");
        rules_set_context(&st, &ctx);
        device_fake_push(b, EV_KEY, KEY_LEFTCTRL, 1, T);
        device_fake_push(b, EV_KEY, KEY_Q, 1, T + MS(20));
        device_fake_push(b, EV_KEY, KEY_Q, 0, T + MS(60));
        device_fake_push(b, EV_KEY, KEY_LEFTCTRL, 0, T + MS(80));
        pump(b, &st);
        expect(b, "an excepted focused app keeps its ordinary Ctrl+Q", w, 4);
    }

    /* Everything off is a pure pass-through, which is the shipped default. */
    rules_config_defaults(&cfg);
    {
        rules_event w[] = {E(KEY_CAPSLOCK, 1), E(KEY_CAPSLOCK, 0)};
        expect(run(&cfg, s_sk_tap, T), "with every rule off the relay changes nothing", w, 2);
    }

    /* A reconfigure must not strand a modifier the engine is holding. */
    {
        input_backend *b = enable_cycle();
        rules_state st;
        rules_config on, off;
        rules_event out[RULES_MAX_OUT];
        int n;

        rules_config_defaults(&on);
        on.sk.enabled = true;
        on.sk.n_mods = 1;
        on.sk.mods[0] = KEY_LEFTCTRL;
        rules_config_defaults(&off);

        rules_init(&st, &on);
        device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 1, T);
        device_fake_push(b, EV_KEY, KEY_C, 1, T + MS(50));
        pump(b, &st);
        n = rules_reconfigure(&st, &off, out, RULES_MAX_OUT);
        checks++;
        current_case = "SetRules while the super key is held releases its modifier";
        if (n == 1 && out[0].type == EV_KEY && out[0].code == KEY_LEFTCTRL && out[0].value == 0)
            printf("  ok   %s\n", current_case);
        else
            fail("reconfigure emitted %d events", n);
        b->close(b);
    }
}

/* ========================================================================== *
 * The SetRules and SetContext documents
 * ========================================================================== */

static void test_documents(void)
{
    rules_config c;
    rules_context ctx;
    char err[256];

    printf("\nthe SetRules and SetContext documents\n");

    expect_true(rules_config_from_json(
                    "{\"keyboard_debounce\":{\"enabled\":true,\"window_ms\":12,"
                    "\"key_windows\":\"30:20,18:0\"},"
                    "\"mouse_click_debounce\":{\"enabled\":true,\"window_ms\":30},"
                    "\"scroll_invert\":{\"enabled\":true,\"horizontal\":true,\"touchpad\":true},"
                    "\"smooth_scroll\":{\"enabled\":true,\"step\":80,\"response\":20},"
                    "\"super_key\":{\"enabled\":true,\"source\":58,\"modifiers\":[29,56],"
                    "\"tap_action\":\"key\",\"tap_key\":88,\"hold_threshold_ms\":400},"
                    "\"mouse_button_shortcut\":{\"enabled\":true,\"gesture_button\":276,"
                    "\"bindings\":[{\"input\":275,\"modifiers\":[29],\"key\":15}]},"
                    "\"quit_protection\":{\"enabled\":true,\"quit\":{\"enabled\":true,"
                    "\"mode\":\"double_press\",\"double_ms\":900,\"scope\":\"selected_only\","
                    "\"exceptions\":[\"org.gnome.Terminal\",\"firefox\"]}}}",
                    &c, err, sizeof(err)) == 0 &&
                    c.kdb.enabled && c.kdb.global_window_ms == 12 && c.kdb.n_key_windows == 2 &&
                    c.mcd.window_ms == 30 && c.scroll.horizontal && c.scroll.touchpad &&
                    c.smooth.step == 80 && c.smooth.response == 20 && c.sk.n_mods == 2 &&
                    c.sk.tap_action == RULES_SK_KEY && c.sk.tap_key == 88 &&
                    c.sk.hold_threshold_ns == MS(400) && c.mbs.n_bind == 1 &&
                    c.mbs.bind[0].input == BTN_SIDE && c.mbs.bind[0].key == 15 &&
                    c.mbs.gesture_button == BTN_EXTRA && c.qp.quit.mode == RULES_QP_DOUBLE &&
                    c.qp.quit.double_ms == 900 && c.qp.quit.scope == RULES_QP_SCOPE_SELECTED &&
                    c.qp.quit.n_exceptions == 2,
                "a full SetRules document is parsed into every rule");

    expect_true(rules_config_from_json("{}", &c, err, sizeof(err)) == 0 && !c.kdb.enabled &&
                    !c.mcd.enabled && !c.scroll.enabled && !c.smooth.enabled && !c.sk.enabled &&
                    !c.mbs.enabled && !c.qp.enabled,
                "an empty document leaves every rule off, which is the shipped default");

    expect_true(rules_config_from_json("{\"smooth_scroll\":{\"step\":\"fast\"}}", &c, err,
                                       sizeof(err)) < 0,
                "a string where a number belongs is refused");
    expect_true(rules_config_from_json("nonsense", &c, err, sizeof(err)) < 0,
                "a document that is not an object is refused");
    expect_true(rules_config_from_json("{\"quit_protection\":{\"quit\":{\"mode\":\"maybe\"}}}", &c,
                                       err, sizeof(err)) < 0,
                "an unknown enumeration value is refused rather than defaulted");
    expect_true(rules_config_from_json(
                    "{\"mouse_button_shortcut\":{\"bindings\":[{\"input\":272,\"key\":15}]}}", &c,
                    err, sizeof(err)) < 0,
                "a binding on the left mouse button is refused");
    expect_true(rules_config_from_json(
                    "{\"super_key\":{\"enabled\":true,\"source\":16},"
                    "\"quit_protection\":{\"enabled\":true,\"quit\":{\"enabled\":true}}}",
                    &c, err, sizeof(err)) < 0,
                "a super key source another rule already owns is refused");

    {
        size_t n = RULES_JSON_MAX + 1;
        char *big = malloc(n + 1);
        if (!big) {
            expect_true(false, "an oversized SetRules document is rejected");
        } else {
            memset(big, 'a', n);
            big[0] = '{';
            big[n] = '\0';
            expect_true(rules_config_from_json(big, &c, err, sizeof(err)) < 0,
                        "an oversized SetRules document is rejected");
            free(big);
        }
    }
    {
        size_t n = RULES_JSON_MAX;
        char *big = malloc(n + 1);
        if (!big) {
            expect_true(false, "a document at exactly the limit is still parsed");
        } else {
            int w = snprintf(big, n + 1, "{\"smooth_scroll\":{\"step\":80},\"pad\":\"");
            memset(big + w, 'x', n - (size_t)w);
            big[n - 2] = '"';
            big[n - 1] = '}';
            big[n] = '\0';
            expect_true(rules_config_from_json(big, &c, err, sizeof(err)) == 0 &&
                            c.smooth.step == 80,
                        "a document at exactly the limit is still parsed");
            free(big);
        }
    }

    expect_true(rules_context_from_json("{\"focused_app_id\":\"org.gnome.TextEditor\"}", &ctx, err,
                                        sizeof(err)) == 0 &&
                    strcmp(ctx.focused_app_id, "org.gnome.TextEditor") == 0,
                "SetContext carries the focused application id");
    expect_true(rules_context_from_json("{}", &ctx, err, sizeof(err)) == 0 &&
                    ctx.focused_app_id[0] == '\0',
                "SetContext with nothing in it clears the focused application");
    /* An app id is echoed back inside the Rules property, so a quote arriving
     * from an unprivileged caller would make the helper emit malformed JSON
     * to every client that reads it. Refused, never escaped. */
    expect_true(rules_context_from_json("{\"focused_app_id\":\"a\\\"b\"}", &ctx, err,
                                        sizeof(err)) < 0,
                "an application id containing a quote is refused, not escaped");

    {
        rules_state st;
        char buf[2048];
        rules_config_defaults(&c);
        rules_init(&st, &c);
        expect_true(rules_state_to_json(&st, buf, sizeof(buf)) > 0 &&
                        strstr(buf, "\"keyboard_debounce\"") &&
                        strstr(buf, "\"quit_protection\"") && strstr(buf, "\"counters\""),
                    "the Rules property reports every rule and the counters");
    }
}

/* ========================================================================== *
 * Device lifecycle and hot-plug, unchanged from WP-S1
 * ========================================================================== */

static void test_device_lifecycle(void)
{
    rules_config cfg;
    uint64_t T = MS(1000);
    char e[128];

    rules_config_defaults(&cfg);
    cfg.sk.enabled = true;
    cfg.sk.led = false;
    cfg.sk.tap_action = RULES_SK_ESCAPE;

    printf("\ndevice lifecycle: Enable(false) must actually release the devices\n");
    {
        unsigned before = device_fake_close_count();
        input_backend *b;

        checks++;
        current_case = "Enable(true) then Enable(false) closes the backend";
        b = enable_cycle();
        disable_cycle(b);
        if (device_fake_close_count() != before + 1)
            fail("close() was not called: count %u -> %u", before, device_fake_close_count());
        else
            printf("  ok   %s\n", current_case);

        checks++;
        current_case = "a second Enable(true) opens a clean backend";
        b = enable_cycle();
        if (device_fake_sink_count(b) != 0)
            fail("reopened backend carried %zu stale output events", device_fake_sink_count(b));
        else if (device_fake_pending(b) != 0)
            fail("reopened backend carried %zu stale input events", device_fake_pending(b));
        else
            printf("  ok   %s\n", current_case);

        {
            rules_event w[] = {E(KEY_ESC, 1), E(KEY_ESC, 0)};
            rules_state st2;
            rules_init(&st2, &cfg);
            device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 1, T);
            device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 0, T + MS(50));
            pump(b, &st2);
            expect(b, "the reopened backend relays correctly", w, 2); /* expect() closes it */
        }

        checks++;
        current_case = "closing the reopened backend counts too";
        if (device_fake_close_count() != before + 2)
            fail("expected %u closes, saw %u", before + 2, device_fake_close_count());
        else
            printf("  ok   %s\n", current_case);
    }

    printf("\ndevice hot-plug: a keyboard plugged in after Enable(true) is grabbed\n");
    {
        /* The relay grabs what exists when it starts. A keyboard plugged in
         * afterwards is not covered by any rule until it is claimed too, and
         * on a machine where the relay swallows Caps Lock, one keyboard
         * behaving differently from the rest is the bug report. The daemon
         * gets these events from udev_monitor; here they are injected through
         * the same interface the monitor feeds.
         *
         * The real monitor is in device_evdev.c and has never run: this
         * kernel has no input devices at all (see PRIVILEGES.md § 8). */
        input_backend *b = device_fake_new();
        rules_state st3;
        char devs[1024];

        rules_init(&st3, &cfg);
        b->open(b, true, e, sizeof(e));

        b->describe(b, devs, sizeof(devs));
        checks++;
        current_case = "before the hot-plug there are two grabbed sources";
        if (strstr(devs, "fake:keyboard0") && strstr(devs, "fake:mouse0") &&
            !strstr(devs, "fake:keyboard1"))
            printf("  ok   %s\n", current_case);
        else
            fail("unexpected initial devices: %s", devs);

        device_fake_inject_add(b, "fake:keyboard1", "hot-plugged keyboard", "keyboard");

        checks++;
        current_case = "read() reports the change rather than swallowing it";
        {
            rules_event in;
            uint64_t ts;
            int rc = b->read(b, &in, &ts, 0);
            if (rc == DEV_READ_HOTPLUG)
                printf("  ok   %s\n", current_case);
            else
                fail("read() returned %d, expected DEV_READ_HOTPLUG", rc);
        }

        checks++;
        current_case = "the new keyboard is claimed, and grabbed";
        b->describe(b, devs, sizeof(devs));
        if (strstr(devs, "{\"node\":\"fake:keyboard1\",\"name\":\"hot-plugged keyboard\","
                         "\"kind\":\"keyboard\",\"grabbed\":true}"))
            printf("  ok   %s\n", current_case);
        else
            fail("hot-plugged device not grabbed: %s", devs);

        checks++;
        current_case = "the hot-plug is described for the log";
        if (strstr(b->last_hotplug(b), "added fake:keyboard1") &&
            strstr(b->last_hotplug(b), "grabbed"))
            printf("  ok   %s\n", current_case);
        else
            fail("last_hotplug said '%s'", b->last_hotplug(b));

        checks++;
        current_case = "an unplug drops the device from the claimed set";
        device_fake_inject_remove(b, "fake:mouse0");
        {
            rules_event in;
            uint64_t ts;
            b->read(b, &in, &ts, 0);
        }
        b->describe(b, devs, sizeof(devs));
        if (!strstr(devs, "fake:mouse0") && strstr(devs, "fake:keyboard1"))
            printf("  ok   %s\n", current_case);
        else
            fail("after unplug the list is %s", devs);

        {
            rules_event w[] = {E(KEY_ESC, 1), E(KEY_ESC, 0)};
            device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 1, T);
            device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 0, T + MS(50));
            pump(b, &st3);
            expect(b, "the relay keeps working on what it still holds", w, 2);
        }

        checks++;
        current_case = "a listen-only backend does not grab a hot-plugged device";
        {
            input_backend *t = device_fake_new();
            t->open(t, false, e, sizeof(e));
            device_fake_inject_add(t, "fake:keyboard1", "hot-plugged keyboard", "keyboard");
            {
                rules_event in;
                uint64_t ts;
                t->read(t, &in, &ts, 0);
            }
            t->describe(t, devs, sizeof(devs));
            if (strstr(devs, "fake:keyboard1") && !strstr(devs, "\"grabbed\":true"))
                printf("  ok   %s\n", current_case);
            else
                fail("tap mode grabbed a hot-plugged device: %s", devs);
            t->close(t);
        }
    }
}

int main(void)
{
    test_keyboard_debounce_vectors();
    test_mouse_click_debounce_vectors();
    test_scroll_invert_vectors();
    test_smooth_scroll_vectors();
    test_super_key_vectors();
    test_mouse_button_vectors();
    test_quit_protection_vectors();
    test_relay_path();
    test_documents();
    test_device_lifecycle();

    printf("\n%d assertions, %d failed -- %s\n", checks, failures,
           failures ? "FAILURES" : "all rules tests passed");
    return failures ? 1 : 0;
}
