/* keyboard_debounce and mouse_click_debounce.
 *
 * Both are line-by-line ports of the macOS state machines:
 *   KeyboardDebounceSupport.swift    -> KeyboardDebounceState.shouldSuppress
 *   MouseClickDebounceSupport.swift  -> MouseClickDebounceState.shouldSuppress
 *
 * The one behaviour worth stating out loud, because it differs from the WP-03
 * spike's 40 ms filter this rule replaces: a key *release* is never
 * suppressed. The spike dropped the release that closed a suppressed press so
 * the downstream key state stayed balanced; the macOS filter does not, on the
 * grounds that stopping the tap must never be able to leave a key held. On
 * evdev that costs nothing, because input_handle_event() in the kernel drops
 * a release for a key that is not currently down, so the extra release the
 * relay emits is swallowed one layer below the compositor. */
#include "rules_internal.h"

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- keyboard_debounce ---------------------------------------------------- */

/* Defaults.sanitizedKeyboardDebounceWindow: out of range falls back to the
 * default rather than clamping, exactly as the Swift does -- a stored 900 is
 * a damaged preference, not a request for 500. */
unsigned rules_kdb_sanitize_window(long ms)
{
    if (ms < 0 || ms > RULES_KDB_MAX_MS)
        return RULES_KDB_DEFAULT_MS;
    return (unsigned)ms;
}

unsigned rules_kdb_window_ms(const rules_kdb_config *cfg, uint16_t code)
{
    for (unsigned i = 0; i < cfg->n_key_windows; i++)
        if (cfg->key_code[i] == code)
            return cfg->key_window_ms[i];
    return cfg->global_window_ms;
}

/* KeyboardDebounceConfig.decodeKeyWindows, over the same "code:ms,code:ms"
 * storage string the macOS settings page writes, so a settings backup moves
 * between the two builds unchanged. An entry that does not parse is skipped,
 * never guessed at. */
int rules_kdb_decode_key_windows(const char *raw, rules_kdb_config *cfg)
{
    const char *p = raw;

    cfg->n_key_windows = 0;
    if (!raw)
        return 0;

    while (*p) {
        char *end;
        long code, window;

        while (*p == ',' || *p == ' ')
            p++;
        if (!*p)
            break;
        if (*p < '0' || *p > '9') { /* skip a malformed entry whole */
            while (*p && *p != ',')
                p++;
            continue;
        }
        code = strtol(p, &end, 10);
        if (*end != ':') {
            while (*p && *p != ',')
                p++;
            continue;
        }
        p = end + 1;
        if (*p < '0' || *p > '9') {
            while (*p && *p != ',')
                p++;
            continue;
        }
        window = strtol(p, &end, 10);
        p = end;
        if (code < 0 || code > RULES_KEY_MAX)
            continue;
        if (cfg->n_key_windows >= RULES_KDB_KEYS)
            return -1;
        cfg->key_code[cfg->n_key_windows] = (uint16_t)code;
        cfg->key_window_ms[cfg->n_key_windows] = rules_kdb_sanitize_window(window);
        cfg->n_key_windows++;
    }
    return 0;
}

/* KeyboardDebounceConfig.encodeKeyWindows: sorted by keycode, so the string
 * never reorders itself between writes. */
int rules_kdb_encode_key_windows(const rules_kdb_config *cfg, char *buf, size_t cap)
{
    unsigned order[RULES_KDB_KEYS];
    size_t used = 0;

    for (unsigned i = 0; i < cfg->n_key_windows; i++)
        order[i] = i;
    for (unsigned i = 1; i < cfg->n_key_windows; i++) {
        unsigned v = order[i], j = i;
        while (j > 0 && cfg->key_code[order[j - 1]] > cfg->key_code[v]) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = v;
    }

    if (cap)
        buf[0] = '\0';
    for (unsigned i = 0; i < cfg->n_key_windows; i++) {
        int n = snprintf(buf + used, cap - used, "%s%u:%u", i ? "," : "",
                         cfg->key_code[order[i]], cfg->key_window_ms[order[i]]);
        if (n < 0 || (size_t)n >= cap - used)
            return -1;
        used += (size_t)n;
    }
    return (int)used;
}

void rules_kdb_reset(rules_state *st)
{
    memset(st->kdb, 0, sizeof(st->kdb));
    st->kdb_have_last_accepted = false;
}

/* KeyboardDebounceState.sanitizedState: a timestamp that moved backwards, or
 * a gap longer than five seconds, means the state on file describes a
 * different typing session and is thrown away. */
static rules_kdb_key *kdb_sanitized(rules_state *st, uint16_t code, uint64_t ts)
{
    rules_kdb_key *k = &st->kdb[code];

    if (k->have_last && (ts < k->last_event || ts - k->last_event > RULES_KDB_STALE_NS)) {
        memset(k, 0, sizeof(*k));
        if (st->kdb_have_last_accepted && st->kdb_last_accepted == code)
            st->kdb_have_last_accepted = false;
    }
    return k;
}

bool rules_kdb_should_suppress(rules_state *st, uint16_t code, bool is_autorepeat, bool is_down,
                               uint64_t ts, const rules_kdb_config *cfg)
{
    rules_kdb_key *k;
    uint64_t window;
    bool suppress = false;

    if (code > RULES_KEY_MAX)
        return false;

    if (!cfg->enabled) {
        memset(&st->kdb[code], 0, sizeof(st->kdb[code]));
        if (st->kdb_have_last_accepted && st->kdb_last_accepted == code)
            st->kdb_have_last_accepted = false;
        return false;
    }

    k = kdb_sanitized(st, code, ts);

    if (!is_down) {
        /* A release is never suppressed. It closes the accepted press if one
         * is open, and is otherwise inert. */
        if (k->is_down) {
            k->is_down = false;
            k->last_release = ts;
            k->have_release = true;
        }
        goto done;
    }

    if (is_autorepeat) {
        /* The kernel generates autorepeat, never the switch. */
        k->is_down = true;
        goto done;
    }

    window = MS_NS(rules_kdb_window_ms(cfg, code));
    if (window == 0) {
        k->is_down = true;
        k->last_press = ts;
        k->have_press = true;
        st->kdb_have_last_accepted = true;
        st->kdb_last_accepted = code;
        goto done;
    }

    if (k->is_down && k->have_press && ts >= k->last_press && ts - k->last_press < window) {
        suppress = true; /* a second press while the accepted one is still held */
        goto done;
    }
    if (k->have_release && st->kdb_have_last_accepted && st->kdb_last_accepted == code &&
        ts >= k->last_release && ts - k->last_release < window) {
        suppress = true; /* the bounce proper: a re-press inside the window */
        goto done;
    }

    k->is_down = true;
    k->last_press = ts;
    k->have_press = true;
    st->kdb_have_last_accepted = true;
    st->kdb_last_accepted = code;

done:
    /* The Swift `defer`: every event stamps the key, suppressed or not. */
    k->last_event = ts;
    k->have_last = true;
    if (suppress)
        st->stat_kdb_dropped++;
    return suppress;
}

int rules_kdb_apply(rules_state *st, const rules_event *in, uint64_t now, rules_event *out, int cap,
                    int n, bool *handled)
{
    (void)out;
    (void)cap;
    /* Buttons are the click filter's business, not this one's. EV_KEY carries
     * both: BTN_* occupies 0x100..0x15f and 0x2c0..0x2ff, and everything else
     * in the type is a key. */
    if (!st->cfg.kdb.enabled || in->type != EV_KEY ||
        (in->code >= BTN_MISC && in->code <= BTN_GEAR_UP) ||
        (in->code >= BTN_TRIGGER_HAPPY && in->code <= BTN_TRIGGER_HAPPY40))
        return n;

    if (rules_kdb_should_suppress(st, in->code, in->value == 2, in->value != 0, now, &st->cfg.kdb))
        *handled = true;
    return n;
}

/* --- mouse_click_debounce ------------------------------------------------- */

/* Defaults.sanitizedMouseClickDebounceWindow: the allowed range is 5..100 and
 * anything else is a damaged preference. */
unsigned rules_mcd_sanitize_window(long ms)
{
    if (ms < RULES_MCD_MIN_MS || ms > RULES_MCD_MAX_MS)
        return RULES_MCD_DEFAULT_MS;
    return (unsigned)ms;
}

/* MouseClickDebounceInput.resolve. Primary, secondary and middle only: the
 * extra buttons keep their navigation, shortcut and gesture ownership. */
int rules_mcd_button_for(uint16_t code)
{
    switch (code) {
    case BTN_LEFT: return 0;
    case BTN_RIGHT: return 1;
    case BTN_MIDDLE: return 2;
    default: return -1;
    }
}

void rules_mcd_reset(rules_state *st) { memset(st->mcd, 0, sizeof(st->mcd)); }

bool rules_mcd_should_suppress(rules_state *st, int button, rules_click_kind kind, uint64_t ts,
                               const rules_mcd_config *cfg)
{
    rules_mcd_button *b;
    uint64_t window;
    bool suppress = false;

    if (button < 0 || button > 2)
        return false;
    if (!cfg->enabled) {
        memset(&st->mcd[button], 0, sizeof(st->mcd[button]));
        return false;
    }

    b = &st->mcd[button];
    /* sanitizedState: only a timestamp moving backwards resets a button --
     * there is no stale-gap rule here, because a button held for an hour is
     * an ordinary thing to do. */
    if (b->have_last_event && ts < b->last_event)
        memset(b, 0, sizeof(*b));

    switch (kind) {
    case RULES_CLICK_DOWN:
        if (b->accepted_down) {
            suppress = true; /* repeated Down while the accepted press stands */
            break;
        }
        if (b->suppressed_down) {
            suppress = true; /* more Downs before the bounce's own Up */
            break;
        }
        window = MS_NS(cfg->window_ms);
        if (b->have_last_up && ts >= b->last_up && ts - b->last_up < window) {
            b->suppressed_down = true;
            suppress = true;
            break;
        }
        b->accepted_down = true;
        break;

    case RULES_CLICK_DRAG:
        /* A suppressed Down owns its drag packets too, or the app would see a
         * drag with no matching press. No evdev event maps here; see rules.h. */
        suppress = b->suppressed_down;
        break;

    case RULES_CLICK_UP:
        if (b->suppressed_down) {
            b->suppressed_down = false;
            suppress = true;
            break;
        }
        if (!b->accepted_down) {
            /* An unmatched Up may be recovery from a relay that was off. Fail
             * open: it cannot stick anything down. */
            break;
        }
        b->accepted_down = false;
        b->last_up = ts;
        b->have_last_up = true;
        break;
    }

    b->last_event = ts;
    b->have_last_event = true;
    if (suppress)
        st->stat_mcd_dropped++;
    return suppress;
}

int rules_mcd_apply(rules_state *st, const rules_event *in, uint64_t now, rules_event *out, int cap,
                    int n, bool *handled)
{
    int button;

    (void)out;
    (void)cap;
    if (!st->cfg.mcd.enabled || in->type != EV_KEY)
        return n;
    button = rules_mcd_button_for(in->code);
    if (button < 0 || in->value == 2)
        return n; /* a mouse button has no autorepeat to reason about */

    if (rules_mcd_should_suppress(st, button, in->value ? RULES_CLICK_DOWN : RULES_CLICK_UP, now,
                                  &st->cfg.mcd))
        *handled = true;
    return n;
}
