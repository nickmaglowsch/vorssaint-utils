/* vorssaint-relay: the input relay proper.
 *
 * One loop drives both device backends. Modes:
 *   (default)      grab every keyboard/mouse/touchpad, apply the rules, and
 *                  re-emit through one uinput device
 *   --tap          listen only: no EVIOCGRAB, no output device, print events
 *   --replay FILE  run a recorded evdev stream through the rules
 *   --record FILE  write the raw source stream to a file (real backend)
 *   --bench N      latency of the read -> rules -> write path over N events
 */
#include "device.h"
#include "rules.h"

#include <errno.h>
#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static const char *code_name(uint16_t type, uint16_t code)
{
    const char *n = libevdev_event_code_get_name(type, code);
    return n ? n : "?";
}

static void print_event(const char *dir, uint64_t ts_ns, const rules_event *e)
{
    printf("%s %10llu.%09llu %-6s %-20s %d\n", dir,
           (unsigned long long)(ts_ns / 1000000000ULL),
           (unsigned long long)(ts_ns % 1000000000ULL),
           libevdev_event_type_get_name(e->type) ?: "?", code_name(e->type, e->code), e->value);
}

/* ---- latency ------------------------------------------------------------ */

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t pct(uint64_t *s, size_t n, double p)
{
    size_t i = (size_t)(p * (double)(n - 1) + 0.5);
    return s[i];
}

/* The measured quantity is the relay's own cost: the interval from read()
 * returning an event to the matching write()+SYN having been handed to the
 * kernel. Kernel-side evdev and uinput delivery are outside it and are the
 * same for any relay of this design. */
static int run_bench(input_backend *b, size_t n_events, const char *write_to)
{
    rules_state st;
    uint64_t *samples = calloc(n_events, sizeof(uint64_t));
    size_t n = 0;
    uint64_t base = now_monotonic_ns();
    char err[256] = {0};
    int sink_fd = -1;

    if (!samples)
        return 1;

    /* libevdev_uinput_write_event() is a write(2) of one struct input_event to
     * the uinput fd. With --write-to the bench performs that same write to a
     * real descriptor, so the number includes the syscall the fake backend
     * cannot make. It still excludes the kernel's uinput-side work. */
    if (write_to) {
        sink_fd = open(write_to, O_WRONLY | O_CLOEXEC);
        if (sink_fd < 0) {
            fprintf(stderr, "relay: open %s: %s\n", write_to, strerror(errno));
            free(samples);
            return 1;
        }
    }
    rules_init(&st, NULL);
    if (b->open(b, true, err, sizeof(err)) < 0) {
        fprintf(stderr, "relay: backend %s open failed: %s\n", b->name, err);
        free(samples);
        return 1;
    }

    /* A synthetic stream shaped like real typing: press/release pairs 12 ms
     * apart on rotating keycodes, so both rules run on every iteration but
     * neither suppresses anything (the measurement is of the common path). */
    for (size_t i = 0; i < n_events; i++) {
        uint16_t code = (uint16_t)(KEY_A + (i / 2) % 26);
        int32_t val = (i % 2 == 0) ? 1 : 0;
        device_fake_push(b, EV_KEY, code, val, base + (uint64_t)i * 12000000ULL);
    }

    while (n < n_events) {
        rules_event in, out[RULES_MAX_OUT];
        uint64_t ts, t0, t1;
        int rc, m;

        rc = b->read(b, &in, &ts, 0);
        if (rc != DEV_READ_EVENT)
            break;

        t0 = now_monotonic_ns();
        m = rules_process(&st, &in, ts, out, RULES_MAX_OUT);
        for (int k = 0; k < m; k++) {
            b->write(b, &out[k]);
            if (sink_fd >= 0) {
                struct input_event ie = {0};
                ie.type = out[k].type;
                ie.code = out[k].code;
                ie.value = out[k].value;
                if (write(sink_fd, &ie, sizeof(ie)) < 0)
                    break;
            }
        }
        if (m > 0) {
            b->sync(b);
            if (sink_fd >= 0) {
                struct input_event ie = {0};
                ie.type = EV_SYN;
                ie.code = SYN_REPORT;
                if (write(sink_fd, &ie, sizeof(ie)) < 0)
                    break;
            }
        }
        t1 = now_monotonic_ns();

        samples[n++] = t1 - t0;
    }

    if (sink_fd >= 0)
        close(sink_fd);
    qsort(samples, n, sizeof(uint64_t), cmp_u64);
    printf("backend=%s events=%zu write_to=%s\n", b->name, n, write_to ? write_to : "(none)");
    printf("added latency per event (read -> rules -> write+SYN), nanoseconds:\n");
    printf("  min   %8llu\n", (unsigned long long)samples[0]);
    printf("  p50   %8llu\n", (unsigned long long)pct(samples, n, 0.50));
    printf("  p90   %8llu\n", (unsigned long long)pct(samples, n, 0.90));
    printf("  p99   %8llu\n", (unsigned long long)pct(samples, n, 0.99));
    printf("  p99.9 %8llu\n", (unsigned long long)pct(samples, n, 0.999));
    printf("  max   %8llu\n", (unsigned long long)samples[n - 1]);
    printf("  p50   %8.3f us\n", (double)pct(samples, n, 0.50) / 1000.0);
    printf("  p99   %8.3f us\n", (double)pct(samples, n, 0.99) / 1000.0);
    free(samples);
    return 0;
}

/* ---- recorded stream ---------------------------------------------------- */

/* The file format is exactly what reading an evdev character device yields:
 * a packed sequence of struct input_event. That makes a capture taken on a
 * real machine (`cat /dev/input/eventN > stream.bin`) replayable here without
 * conversion. */
static int run_replay(input_backend *b, const char *path, bool verbose)
{
    rules_state st;
    struct input_event ie;
    FILE *f = fopen(path, "rb");
    uint64_t base = 0;
    char err[256] = {0};
    size_t n_in = 0;

    if (!f) {
        fprintf(stderr, "relay: open %s: %s\n", path, strerror(errno));
        return 1;
    }
    rules_init(&st, NULL);
    if (b->open(b, true, err, sizeof(err)) < 0) {
        fprintf(stderr, "relay: backend %s open failed: %s\n", b->name, err);
        fclose(f);
        return 1;
    }

    while (fread(&ie, sizeof(ie), 1, f) == 1) {
        uint64_t ts = (uint64_t)ie.input_event_sec * 1000000000ULL +
                      (uint64_t)ie.input_event_usec * 1000ULL;
        rules_event in = {ie.type, ie.code, ie.value};
        rules_event out[RULES_MAX_OUT];
        int m;

        if (base == 0)
            base = ts;
        n_in++;

        /* Run the hold timer that would have fired between the previous event
         * and this one; without it a hold only resolves on the next keypress.
         * It is run at its own deadline, not at this event's timestamp, so the
         * printed time matches what the live loop's poll timeout would give. */
        uint64_t deadline = rules_deadline_ns(&st);
        uint64_t timer_ns = (deadline != 0 && deadline < ts) ? deadline : ts;
        m = rules_timer(&st, timer_ns, out, RULES_MAX_OUT);
        for (int k = 0; k < m; k++) {
            b->write(b, &out[k]);
            if (verbose)
                print_event("TIM>", timer_ns - base, &out[k]);
        }
        if (m > 0)
            b->sync(b);

        if (verbose && in.type != EV_SYN)
            print_event("in  ", ts - base, &in);
        m = rules_process(&st, &in, ts, out, RULES_MAX_OUT);
        for (int k = 0; k < m; k++) {
            b->write(b, &out[k]);
            if (verbose)
                print_event("  >>", ts - base, &out[k]);
        }
        if (m > 0)
            b->sync(b);
    }
    fclose(f);

    {
        char json[512];
        rules_state_to_json(&st, json, sizeof(json));
        printf("replayed %zu raw events\n", n_in);
        printf("emitted  %zu events to the output device\n", device_fake_sink_count(b));
        printf("state: %s\n", json);
    }
    return 0;
}

/* ---- live loop ---------------------------------------------------------- */

static int run_live(input_backend *b, bool tap_mode, const char *record_path)
{
    rules_state st;
    char err[512] = {0};
    char devs[4096];
    FILE *rec = NULL;
    uint64_t base = 0;

    rules_init(&st, NULL);

    if (b->open(b, !tap_mode, err, sizeof(err)) < 0) {
        fprintf(stderr, "relay: cannot start on backend '%s': %s\n", b->name, err);
        return 2;
    }
    if (b->describe(b, devs, sizeof(devs)) > 0)
        fprintf(stderr, "relay: sources %s\n", devs);
    fprintf(stderr, "relay: mode=%s grab=%s\n", tap_mode ? "tap (listen only)" : "relay",
            tap_mode ? "no" : "yes");

    if (record_path) {
        rec = fopen(record_path, "wb");
        if (!rec) {
            fprintf(stderr, "relay: open %s: %s\n", record_path, strerror(errno));
            return 1;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    while (!g_stop) {
        rules_event in, out[RULES_MAX_OUT];
        uint64_t ts, deadline;
        int rc, m;

        deadline = rules_deadline_ns(&st);
        rc = b->read(b, &in, &ts, deadline);

        if (rc == DEV_READ_TIMEOUT) {
            m = rules_timer(&st, now_monotonic_ns(), out, RULES_MAX_OUT);
            for (int k = 0; k < m; k++)
                b->write(b, &out[k]);
            if (m > 0)
                b->sync(b);
            continue;
        }
        if (rc == DEV_READ_HOTPLUG) {
            char devs2[4096];
            fprintf(stderr, "relay: hot-plug: %s\n", b->last_hotplug(b));
            if (b->describe(b, devs2, sizeof(devs2)) > 0)
                fprintf(stderr, "relay: sources %s\n", devs2);
            continue;
        }
        if (rc == DEV_READ_EOF)
            break;
        if (rc == DEV_READ_ERROR) {
            fprintf(stderr, "relay: read error: %s\n", strerror(errno));
            break;
        }

        if (base == 0)
            base = ts;
        if (rec) {
            struct input_event ie = {0};
            ie.input_event_sec = (long)(ts / 1000000000ULL);
            ie.input_event_usec = (long)((ts % 1000000000ULL) / 1000ULL);
            ie.type = in.type;
            ie.code = in.code;
            ie.value = in.value;
            fwrite(&ie, sizeof(ie), 1, rec);
        }

        if (tap_mode) {
            print_event("tap ", ts - base, &in);
            fflush(stdout);
            continue;
        }

        m = rules_timer(&st, ts, out, RULES_MAX_OUT);
        for (int k = 0; k < m; k++)
            b->write(b, &out[k]);
        m = rules_process(&st, &in, ts, out, RULES_MAX_OUT);
        for (int k = 0; k < m; k++)
            b->write(b, &out[k]);
        if (m > 0)
            b->sync(b);
    }

    if (rec)
        fclose(rec);
    b->close(b);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: vorssaint-relay [--backend evdev|fake] [--tap] [--replay FILE]\n"
            "                       [--record FILE] [--bench N] [-v]\n"
            "\n"
            "  --backend evdev   libudev discovery, EVIOCGRAB, uinput output (default)\n"
            "  --backend fake    in-process queues; used where uinput is unavailable\n"
            "  --tap             listen only: no grab, no output device\n"
            "  --replay FILE     feed a recorded evdev stream through the rules\n"
            "  --record FILE     also write the raw source stream to FILE\n"
            "  --bench N         measure added latency over N synthetic events\n"
            "  --write-to PATH   during --bench, also write(2) each output event to PATH,\n"
            "                    which is what libevdev_uinput_write_event does to\n"
            "                    /dev/uinput; use /dev/null to price the syscall\n");
}

int main(int argc, char **argv)
{
    const char *backend = "evdev";
    const char *replay = NULL, *record = NULL, *write_to = NULL;
    bool tap = false, verbose = false;
    size_t bench = 0;
    input_backend *b;
    int rc;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--backend") && i + 1 < argc)
            backend = argv[++i];
        else if (!strcmp(argv[i], "--tap"))
            tap = true;
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc)
            replay = argv[++i];
        else if (!strcmp(argv[i], "--record") && i + 1 < argc)
            record = argv[++i];
        else if (!strcmp(argv[i], "--bench") && i + 1 < argc)
            bench = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--write-to") && i + 1 < argc)
            write_to = argv[++i];
        else if (!strcmp(argv[i], "-v"))
            verbose = true;
        else {
            usage();
            return 1;
        }
    }

    if (!strcmp(backend, "fake"))
        b = device_fake_new();
    else if (!strcmp(backend, "evdev"))
        b = device_evdev_new();
    else {
        usage();
        return 1;
    }
    if (!b)
        return 1;

    if (bench)
        rc = run_bench(b, bench, write_to);
    else if (replay)
        rc = run_replay(b, replay, verbose);
    else
        rc = run_live(b, tap, record);
    return rc;
}
