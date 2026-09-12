/* Rules engine for the input relay: pure logic, no syscalls, no device access.
 *
 * The engine is deliberately free of I/O so the two device backends (real
 * evdev/uinput and the in-process fake) drive identical code, and so the
 * unit tests in tests/test_rules.c exercise the rules without a kernel. */
#ifndef VORSSAINT_RULES_H
#define VORSSAINT_RULES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef RULES_KEY_MAX
#define RULES_KEY_MAX 767 /* KEY_MAX from linux/input-event-codes.h */
#endif

/* An event stripped to what the rules care about. Mirrors struct input_event
 * without the timeval: timestamps are carried separately in nanoseconds so the
 * fake backend can drive synthetic clocks. */
typedef struct {
    uint16_t type;
    uint16_t code;
    int32_t value;
} rules_event;

typedef struct {
    uint64_t tap_threshold_ns;  /* tap/hold decision point, default 200 ms */
    uint64_t chatter_window_ns; /* chatter suppression window, default 40 ms */
    uint16_t tap_source;        /* key that is remapped, default KEY_CAPSLOCK */
    uint16_t tap_output;        /* emitted on a tap, default KEY_ESC */
    uint16_t hold_output;       /* emitted on a hold, default KEY_LEFTCTRL */
    bool tap_hold_enabled;
    bool chatter_enabled;
} rules_config;

typedef enum {
    TAP_IDLE = 0,    /* source key is up */
    TAP_PENDING = 1, /* source key is down, tap-or-hold not yet decided */
    TAP_HOLD = 2     /* decided as hold; hold_output is down */
} rules_tap_state;

typedef struct {
    rules_config cfg;

    rules_tap_state tap_state;
    uint64_t tap_press_ns;

    /* Chatter bookkeeping, per keycode. */
    uint64_t last_release_ns[RULES_KEY_MAX + 1];
    bool seen_release[RULES_KEY_MAX + 1];
    bool drop_next_release[RULES_KEY_MAX + 1];

    /* Counters, reported by --stats and over D-Bus. */
    uint64_t stat_in;
    uint64_t stat_out;
    uint64_t stat_chatter_dropped;
    uint64_t stat_taps;
    uint64_t stat_holds;
} rules_state;

/* Fill cfg with the two WP-03 rules at their documented defaults. */
void rules_config_defaults(rules_config *cfg);

void rules_init(rules_state *st, const rules_config *cfg);

/* Replace the configuration without losing in-flight key state. If the source
 * key is currently held the tap machine is reset to idle and any outstanding
 * hold_output release is emitted into out. */
int rules_reconfigure(rules_state *st, const rules_config *cfg, rules_event *out, int out_cap);

/* Feed one input event observed at now_ns. Writes 0..n output events into out
 * and returns the count, or -1 if out_cap is too small (never happens with
 * out_cap >= RULES_MAX_OUT). EV_SYN input is dropped; the caller re-syncs. */
#define RULES_MAX_OUT 8
int rules_process(rules_state *st, const rules_event *in, uint64_t now_ns, rules_event *out, int out_cap);

/* Deadline for the next time-driven transition, in absolute ns, or 0 if the
 * engine is not waiting on the clock. The relay uses this as its poll timeout
 * so a hold resolves while the user is still holding the key down. */
uint64_t rules_deadline_ns(const rules_state *st);

/* Run any transition whose deadline has passed. */
int rules_timer(rules_state *st, uint64_t now_ns, rules_event *out, int out_cap);

/* The largest SetRules document the helper will look at. The schema is seven
 * short keys, so a valid document is well under 200 bytes; 64 KiB is three
 * orders of magnitude of slack and still a bound. It matters because the
 * reader below is not a streaming parser: json_find() restarts from the
 * beginning of the string for every key, so the work is O(keys x length) and
 * an unbounded string is an unbounded amount of a root process's time for one
 * unprivileged D-Bus call. D-Bus already caps a message at 128 MiB, which is
 * not a useful limit here. */
#define RULES_JSON_MAX (64 * 1024)

/* Minimal JSON config parser for the D-Bus SetRules(s) method. Accepts
 *   {"tap_hold":true,"tap_threshold_ms":200,"chatter":true,"chatter_ms":40,
 *    "tap_source":58,"tap_output":1,"hold_output":29}
 * Unknown keys are ignored; missing keys keep the default. Returns 0 on
 * success, -1 on malformed input (err receives a short reason). */
int rules_config_from_json(const char *json, rules_config *cfg, char *err, size_t err_cap);

/* Serialise the active configuration and counters as JSON. */
int rules_state_to_json(const rules_state *st, char *buf, size_t cap);

const char *rules_tap_state_name(rules_tap_state s);

#endif /* VORSSAINT_RULES_H */
