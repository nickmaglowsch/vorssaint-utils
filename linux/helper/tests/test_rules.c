/* Unit tests for the rules engine, driven through the fake device backend so
 * the same read -> rules -> write path the daemon uses is under test. */
#include "device.h"
#include "rules.h"

#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
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

/* The loop relay.c's live mode and helper.c's relay thread both run, including
 * the hold timer. Drains the fake's queue and returns. */
static void pump(input_backend *b, rules_state *st)
{
    for (;;) {
        rules_event in, out[RULES_MAX_OUT];
        uint64_t ts, deadline = rules_deadline_ns(st);
        int rc, m;

        rc = b->read(b, &in, &ts, deadline);
        if (rc == DEV_READ_TIMEOUT) {
            m = rules_timer(st, deadline, out, RULES_MAX_OUT);
            for (int k = 0; k < m; k++)
                b->write(b, &out[k]);
            continue;
        }
        if (rc == DEV_READ_HOTPLUG)
            continue; /* the source set changed; nothing to relay this round */
        if (rc != DEV_READ_EVENT)
            break;

        m = rules_timer(st, ts, out, RULES_MAX_OUT);
        for (int k = 0; k < m; k++)
            b->write(b, &out[k]);
        m = rules_process(st, &in, ts, out, RULES_MAX_OUT);
        for (int k = 0; k < m; k++)
            b->write(b, &out[k]);
    }
}

/* What the helper's Enable(true) does: make a backend, then claim. */
static input_backend *enable_cycle(const rules_config *cfg)
{
    input_backend *b = device_fake_new();
    char err[128];
    (void)cfg;
    b->open(b, true, err, sizeof(err));
    return b;
}

/* What the helper's Enable(false) does: close, which releases every grab. */
static void disable_cycle(input_backend *b) { b->close(b); }

static input_backend *run(const rules_config *cfg, void (*script)(input_backend *, uint64_t),
                          uint64_t base)
{
    input_backend *b = enable_cycle(cfg);
    rules_state st;

    rules_init(&st, cfg);
    script(b, base);
    pump(b, &st);
    return b;
}

static void expect(input_backend *b, const char *name, const rules_event *want, size_t n_want)
{
    size_t got = device_fake_sink_count(b);
    current_case = name;

    if (got != n_want) {
        fail("expected %zu output events, got %zu", n_want, got);
    } else {
        for (size_t i = 0; i < n_want; i++) {
            const rules_event *g = device_fake_sink(b, i);
            if (g->type != want[i].type || g->code != want[i].code || g->value != want[i].value) {
                fail("event %zu: want %s/%s/%d, got %s/%s/%d", i,
                     libevdev_event_type_get_name(want[i].type),
                     libevdev_event_code_get_name(want[i].type, want[i].code) ?: "?",
                     want[i].value, libevdev_event_type_get_name(g->type),
                     libevdev_event_code_get_name(g->type, g->code) ?: "?", g->value);
            }
        }
    }
    if (failures == 0 || current_case)
        printf("  %-46s %s\n", name, got == n_want ? "ok" : "FAILED");
    b->close(b);
}

/* --- rule 1: tap/hold ---------------------------------------------------- */

static void s_tap_short(input_backend *b, uint64_t t)
{
    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 1, t);
    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 0, t + MS(50));
}

static void s_tap_at_199(input_backend *b, uint64_t t)
{
    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 1, t);
    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 0, t + MS(199));
}

static void s_hold_alone(input_backend *b, uint64_t t)
{
    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 1, t);
    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 0, t + MS(400));
}

static void s_hold_exact_200(input_backend *b, uint64_t t)
{
    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 1, t);
    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 0, t + MS(200));
}

static void s_hold_modifier(input_backend *b, uint64_t t)
{
    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 1, t);
    device_fake_push(b, EV_KEY, KEY_C, 1, t + MS(80));
    device_fake_push(b, EV_KEY, KEY_C, 0, t + MS(140));
    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 0, t + MS(180));
}

static void s_hold_autorepeat(input_backend *b, uint64_t t)
{
    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 1, t);
    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 2, t + MS(500));
    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 2, t + MS(533));
    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 0, t + MS(600));
}

static void s_motion_no_resolve(input_backend *b, uint64_t t)
{
    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 1, t);
    device_fake_push(b, EV_REL, REL_X, 7, t + MS(20));
    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 0, t + MS(50));
}

static void s_other_key_untouched(input_backend *b, uint64_t t)
{
    device_fake_push(b, EV_KEY, KEY_A, 1, t);
    device_fake_push(b, EV_KEY, KEY_A, 0, t + MS(60));
}

/* --- rule 2: chatter ----------------------------------------------------- */

static void s_chatter_bounce(input_backend *b, uint64_t t)
{
    device_fake_push(b, EV_KEY, KEY_E, 1, t);
    device_fake_push(b, EV_KEY, KEY_E, 0, t + MS(30));
    device_fake_push(b, EV_KEY, KEY_E, 1, t + MS(38)); /* 8 ms after release */
    device_fake_push(b, EV_KEY, KEY_E, 0, t + MS(44));
}

static void s_chatter_burst(input_backend *b, uint64_t t)
{
    device_fake_push(b, EV_KEY, KEY_E, 1, t);
    device_fake_push(b, EV_KEY, KEY_E, 0, t + MS(30));
    device_fake_push(b, EV_KEY, KEY_E, 1, t + MS(38));
    device_fake_push(b, EV_KEY, KEY_E, 0, t + MS(44));
    device_fake_push(b, EV_KEY, KEY_E, 1, t + MS(55));
    device_fake_push(b, EV_KEY, KEY_E, 0, t + MS(64));
}

static void s_chatter_intentional(input_backend *b, uint64_t t)
{
    device_fake_push(b, EV_KEY, KEY_E, 1, t);
    device_fake_push(b, EV_KEY, KEY_E, 0, t + MS(30));
    device_fake_push(b, EV_KEY, KEY_E, 1, t + MS(90)); /* 60 ms after release */
    device_fake_push(b, EV_KEY, KEY_E, 0, t + MS(125));
}

static void s_chatter_other_key(input_backend *b, uint64_t t)
{
    /* A fast press of a *different* key inside the window is normal typing. */
    device_fake_push(b, EV_KEY, KEY_E, 1, t);
    device_fake_push(b, EV_KEY, KEY_E, 0, t + MS(30));
    device_fake_push(b, EV_KEY, KEY_R, 1, t + MS(35));
    device_fake_push(b, EV_KEY, KEY_R, 0, t + MS(60));
}

static void s_autorepeat_passthrough(input_backend *b, uint64_t t)
{
    device_fake_push(b, EV_KEY, KEY_DOWN, 1, t);
    device_fake_push(b, EV_KEY, KEY_DOWN, 2, t + MS(250));
    device_fake_push(b, EV_KEY, KEY_DOWN, 2, t + MS(283));
    device_fake_push(b, EV_KEY, KEY_DOWN, 0, t + MS(316));
}

#define E(c, v) {EV_KEY, (c), (v)}

int main(void)
{
    rules_config cfg;
    uint64_t T = MS(1000);
    char err[128];

    rules_config_defaults(&cfg);

    printf("rule 1: Caps Lock tap -> Escape, hold -> Control (200 ms)\n");
    {
        rules_event w[] = {E(KEY_ESC, 1), E(KEY_ESC, 0)};
        expect(run(&cfg, s_tap_short, T), "50 ms tap emits Escape", w, 2);
    }
    {
        rules_event w[] = {E(KEY_ESC, 1), E(KEY_ESC, 0)};
        expect(run(&cfg, s_tap_at_199, T), "199 ms is still a tap", w, 2);
    }
    {
        rules_event w[] = {E(KEY_LEFTCTRL, 1), E(KEY_LEFTCTRL, 0)};
        expect(run(&cfg, s_hold_exact_200, T), "200 ms exactly is a hold", w, 2);
    }
    {
        rules_event w[] = {E(KEY_LEFTCTRL, 1), E(KEY_LEFTCTRL, 0)};
        expect(run(&cfg, s_hold_alone, T), "400 ms hold emits Control down then up", w, 2);
    }
    {
        rules_event w[] = {E(KEY_LEFTCTRL, 1), E(KEY_C, 1), E(KEY_C, 0), E(KEY_LEFTCTRL, 0)};
        expect(run(&cfg, s_hold_modifier, T), "Caps+C before 200 ms is Ctrl+C", w, 4);
    }
    {
        rules_event w[] = {E(KEY_LEFTCTRL, 1), E(KEY_LEFTCTRL, 0)};
        expect(run(&cfg, s_hold_autorepeat, T), "source-key autorepeat is swallowed", w, 2);
    }
    {
        rules_event w[] = {{EV_REL, REL_X, 7}, E(KEY_ESC, 1), E(KEY_ESC, 0)};
        expect(run(&cfg, s_motion_no_resolve, T), "mouse motion does not resolve a tap", w, 3);
    }
    {
        rules_event w[] = {E(KEY_A, 1), E(KEY_A, 0)};
        expect(run(&cfg, s_other_key_untouched, T), "unrelated keys pass through", w, 2);
    }

    printf("\nrule 2: 40 ms per-keycode chatter filter\n");
    {
        rules_event w[] = {E(KEY_E, 1), E(KEY_E, 0)};
        expect(run(&cfg, s_chatter_bounce, T), "one bounce inside 40 ms is dropped", w, 2);
    }
    {
        rules_event w[] = {E(KEY_E, 1), E(KEY_E, 0)};
        expect(run(&cfg, s_chatter_burst, T), "a burst of bounces is dropped whole", w, 2);
    }
    {
        rules_event w[] = {E(KEY_E, 1), E(KEY_E, 0), E(KEY_E, 1), E(KEY_E, 0)};
        expect(run(&cfg, s_chatter_intentional, T), "a 60 ms repeat survives", w, 4);
    }
    {
        rules_event w[] = {E(KEY_E, 1), E(KEY_E, 0), E(KEY_R, 1), E(KEY_R, 0)};
        expect(run(&cfg, s_chatter_other_key, T), "the filter is per keycode", w, 4);
    }
    {
        rules_event w[] = {E(KEY_DOWN, 1), E(KEY_DOWN, 2), E(KEY_DOWN, 2), E(KEY_DOWN, 0)};
        expect(run(&cfg, s_autorepeat_passthrough, T), "kernel autorepeat passes through", w, 4);
    }

    printf("\nconfiguration\n");
    {
        rules_config c2;
        current_case = "SetRules JSON is parsed";
        if (rules_config_from_json("{\"tap_hold\":true,\"tap_threshold_ms\":120,"
                                   "\"chatter\":false,\"chatter_ms\":15,\"tap_source\":58,"
                                   "\"tap_output\":1,\"hold_output\":29}",
                                   &c2, err, sizeof(err)) != 0)
            fail("parse failed: %s", err);
        else if (c2.tap_threshold_ns != MS(120) || c2.chatter_enabled ||
                 c2.chatter_window_ns != MS(15) || c2.tap_source != KEY_CAPSLOCK)
            fail("wrong values parsed");
        else
            printf("  %-46s ok\n", current_case);

        current_case = "malformed SetRules JSON is rejected";
        if (rules_config_from_json("{\"tap_threshold_ms\":\"soon\"}", &c2, err, sizeof(err)) == 0)
            fail("accepted a string where a number belongs");
        else
            printf("  %-46s ok (%s)\n", current_case, err);

        current_case = "an oversized SetRules document is rejected";
        {
            /* The reader restarts from the start of the string for each of the
             * seven keys, so an unbounded document is an unbounded amount of a
             * root process's time for one unprivileged call. */
            size_t n = RULES_JSON_MAX + 1;
            char *big = malloc(n + 1);
            if (!big) {
                fail("out of memory");
            } else {
                memset(big, 'a', n);
                big[0] = '{';
                big[n] = '\0';
                if (rules_config_from_json(big, &c2, err, sizeof(err)) == 0)
                    fail("accepted a document of %zu bytes", n);
                else
                    printf("  %-46s ok (%s)\n", current_case, err);
                free(big);
            }
        }

        current_case = "a document at exactly the limit is still parsed";
        {
            /* The bound must be a bound, not an off-by-one that rejects valid
             * input: a document padded to exactly RULES_JSON_MAX must work. */
            size_t n = RULES_JSON_MAX;
            char *big = malloc(n + 1);
            if (!big) {
                fail("out of memory");
            } else {
                int w = snprintf(big, n + 1, "{\"tap_threshold_ms\":120,\"pad\":\"");
                memset(big + w, 'x', n - (size_t)w);
                big[n - 2] = '"';
                big[n - 1] = '}';
                big[n] = '\0';
                if (rules_config_from_json(big, &c2, err, sizeof(err)) == 0 &&
                    c2.tap_threshold_ns == MS(120))
                    printf("  %-46s ok (%d bytes)\n", current_case, RULES_JSON_MAX);
                else
                    fail("rejected a document of exactly %d bytes: %s", RULES_JSON_MAX, err);
                free(big);
            }
        }

        current_case = "non-object SetRules is rejected";
        if (rules_config_from_json("nonsense", &c2, err, sizeof(err)) == 0)
            fail("accepted non-JSON");
        else
            printf("  %-46s ok (%s)\n", current_case, err);
    }

    printf("\nchatter window applies to the remapped key too\n");
    {
        /* Caps Lock chatter must be filtered before the tap machine sees it,
         * or a bouncing Caps Lock produces a spurious Escape. */
        rules_config c3 = cfg;
        input_backend *b = device_fake_new();
        rules_state st;
        rules_event w[] = {E(KEY_ESC, 1), E(KEY_ESC, 0)};

        rules_init(&st, &c3);
        b->open(b, true, err, sizeof(err));
        device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 1, T);
        device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 0, T + MS(40));
        device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 1, T + MS(46)); /* bounce */
        device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 0, T + MS(52));
        for (;;) {
            rules_event in, out[RULES_MAX_OUT];
            uint64_t ts, deadline = rules_deadline_ns(&st);
            int rc, m;
            rc = b->read(b, &in, &ts, deadline);
            if (rc == DEV_READ_TIMEOUT) {
                m = rules_timer(&st, deadline, out, RULES_MAX_OUT);
                for (int k = 0; k < m; k++) b->write(b, &out[k]);
                continue;
            }
            if (rc != DEV_READ_EVENT) break;
            m = rules_process(&st, &in, ts, out, RULES_MAX_OUT);
            for (int k = 0; k < m; k++) b->write(b, &out[k]);
        }
        expect(b, "a bouncing Caps Lock yields one Escape", w, 2);
    }

    printf("\ndevice lifecycle: Enable(false) must actually release the devices\n");
    {
        /* The helper claims devices on Enable(true) and must hand them back on
         * Enable(false). A backend that is merely "stopped" keeps its
         * EVIOCGRAB, which leaves the user unable to type until the daemon
         * exits, so what is asserted here is that close() really runs. */
        unsigned before = device_fake_close_count();
        input_backend *b;

        current_case = "Enable(true) then Enable(false) closes the backend";
        b = enable_cycle(&cfg);
        disable_cycle(b);
        if (device_fake_close_count() != before + 1)
            fail("close() was not called: count %u -> %u", before,
                 device_fake_close_count());
        else
            printf("  %-46s ok\n", current_case);

        current_case = "a second Enable(true) opens a clean backend";
        b = enable_cycle(&cfg);
        if (device_fake_sink_count(b) != 0)
            fail("reopened backend carried %zu stale output events",
                 device_fake_sink_count(b));
        else if (device_fake_pending(b) != 0)
            fail("reopened backend carried %zu stale input events",
                 device_fake_pending(b));
        else
            printf("  %-46s ok\n", current_case);

        current_case = "the reopened backend relays correctly";
        {
            rules_event w[] = {E(KEY_ESC, 1), E(KEY_ESC, 0)};
            rules_state st2;
            rules_init(&st2, &cfg);
            device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 1, T);
            device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 0, T + MS(50));
            pump(b, &st2);
            expect(b, current_case, w, 2); /* expect() closes it */
        }

        current_case = "closing the reopened backend counts too";
        if (device_fake_close_count() != before + 2)
            fail("expected %u closes, saw %u", before + 2, device_fake_close_count());
        else
            printf("  %-46s ok\n", current_case);
    }


    printf("\ndevice hot-plug: a keyboard plugged in after Enable(true) is grabbed\n");
    {
        /* The relay grabs what exists when it starts. A keyboard plugged in
         * afterwards is not covered by any rule until it is claimed too, and
         * on a machine where the relay swallows Caps Lock, one keyboard
         * behaving differently from the rest is the bug report. The daemon
         * gets these events from udev_monitor; here they are injected through
         * the same interface the monitor feeds, and the assertion is on what
         * describe() says afterwards, because that is what GetDevices
         * returns and what the user sees.
         *
         * The real monitor is in device_evdev.c and has never run: this
         * kernel has no input devices at all (see PRIVILEGES.md § 8). */
        input_backend *b = device_fake_new();
        rules_state st3;
        char devs[1024];
        char e[128];

        rules_init(&st3, &cfg);
        b->open(b, true, e, sizeof(e));

        b->describe(b, devs, sizeof(devs));
        current_case = "before the hot-plug there are two grabbed sources";
        if (strstr(devs, "fake:keyboard0") && strstr(devs, "fake:mouse0") &&
            !strstr(devs, "fake:keyboard1"))
            printf("  %-46s ok\n", current_case);
        else
            fail("unexpected initial devices: %s", devs);

        device_fake_inject_add(b, "fake:keyboard1", "hot-plugged keyboard", "keyboard");

        current_case = "read() reports the change rather than swallowing it";
        {
            rules_event in;
            uint64_t ts;
            int rc = b->read(b, &in, &ts, 0);
            if (rc == DEV_READ_HOTPLUG)
                printf("  %-46s ok\n", current_case);
            else
                fail("read() returned %d, expected DEV_READ_HOTPLUG", rc);
        }

        current_case = "the new keyboard is claimed, and grabbed";
        b->describe(b, devs, sizeof(devs));
        if (strstr(devs, "{\"node\":\"fake:keyboard1\",\"name\":\"hot-plugged keyboard\","
                         "\"kind\":\"keyboard\",\"grabbed\":true}"))
            printf("  %-46s ok\n", current_case);
        else
            fail("hot-plugged device not grabbed: %s", devs);

        current_case = "the hot-plug is described for the log";
        if (strstr(b->last_hotplug(b), "added fake:keyboard1") &&
            strstr(b->last_hotplug(b), "grabbed"))
            printf("  %-46s ok\n", current_case);
        else
            fail("last_hotplug said '%s'", b->last_hotplug(b));

        current_case = "an unplug drops the device from the claimed set";
        device_fake_inject_remove(b, "fake:mouse0");
        {
            rules_event in;
            uint64_t ts;
            b->read(b, &in, &ts, 0);
        }
        b->describe(b, devs, sizeof(devs));
        if (!strstr(devs, "fake:mouse0") && strstr(devs, "fake:keyboard1"))
            printf("  %-46s ok\n", current_case);
        else
            fail("after unplug the list is %s", devs);

        current_case = "the relay keeps working on what it still holds";
        {
            rules_event w[] = {E(KEY_ESC, 1), E(KEY_ESC, 0)};
            device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 1, T);
            device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 0, T + MS(50));
            pump(b, &st3);
            expect(b, current_case, w, 2); /* expect() closes it */
        }

        current_case = "a listen-only backend does not grab a hot-plugged device";
        {
            /* Tap mode takes no grab; a device that appears must not acquire
             * one by the back door. */
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
                printf("  %-46s ok\n", current_case);
            else
                fail("tap mode grabbed a hot-plugged device: %s", devs);
            t->close(t);
        }
    }

    printf("\n%s\n", failures ? "FAILURES" : "all rules tests passed");
    return failures ? 1 : 0;
}
