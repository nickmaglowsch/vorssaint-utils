/* quit_protection: Ctrl+Q and Ctrl+W do not fire on the first quick press.
 *
 * Ports QuitProtectionSupport: the two sanitizers, isWithinDoublePressInterval,
 * scopeAllows, isBaseShortcut and isExtraShortcut. macOS's Command becomes
 * Control, which is what the same shortcut is called on every Linux desktop.
 *
 * The per-app scope needs to know which window has focus, and a grabbed input
 * device says nothing about that. The app pushes it with SetContext; see
 * docs/linux-port/PRIVILEGES.md § 4.1 for why that is its own method rather
 * than a field of the rules document.
 *
 * What the helper does NOT do is macOS's usesNativeQuitRequest: asking an
 * application to terminate itself is not an input-device operation and the
 * helper has no business doing it as root. The confirmed decision goes out as
 * a notice carrying "native_quit", and the app decides. */
#include "rules_internal.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <string.h>

/* QuitProtectionSupport.sanitizedHoldDuration: clamps, and replaces a
 * non-finite value with the default. */
unsigned rules_qp_sanitize_hold(double ms)
{
    if (!isfinite(ms))
        return RULES_QP_HOLD_DEFAULT_MS;
    if (ms < RULES_QP_HOLD_MIN_MS)
        return RULES_QP_HOLD_MIN_MS;
    if (ms > RULES_QP_HOLD_MAX_MS)
        return RULES_QP_HOLD_MAX_MS;
    return (unsigned)ms;
}

unsigned rules_qp_sanitize_double(double ms)
{
    if (!isfinite(ms))
        return RULES_QP_DOUBLE_DEFAULT_MS;
    if (ms < RULES_QP_DOUBLE_MIN_MS)
        return RULES_QP_DOUBLE_MIN_MS;
    if (ms > RULES_QP_DOUBLE_MAX_MS)
        return RULES_QP_DOUBLE_MAX_MS;
    return (unsigned)ms;
}

/* Monotonic nanoseconds throughout, so a busy main loop cannot make a valid
 * second press miss its configured interval. */
bool rules_qp_within_double(uint64_t first_ts, uint64_t second_ts, double interval_ms)
{
    uint64_t allowed;

    if (second_ts < first_ts)
        return false;
    allowed = MS_NS(rules_qp_sanitize_double(interval_ms));
    return second_ts - first_ts <= allowed;
}

bool rules_qp_scope_allows(rules_qp_scope scope, const char *app_id, const rules_qp_shortcut *sc)
{
    bool contains = false;

    if (app_id && app_id[0]) {
        for (unsigned i = 0; i < sc->n_exceptions; i++) {
            if (strcmp(sc->exception[i], app_id) == 0) {
                contains = true;
                break;
            }
        }
    }
    switch (scope) {
    case RULES_QP_SCOPE_ALL: return true;
    case RULES_QP_SCOPE_SELECTED: return contains;
    case RULES_QP_SCOPE_EXCEPT: return !contains;
    }
    return true;
}

/* Only the exact Control shortcut is protected by hold and double press. */
bool rules_qp_is_base_shortcut(uint16_t code, bool ctrl, bool alt, bool shift, bool meta,
                               uint16_t shortcut_key)
{
    if (!ctrl || alt || shift || meta)
        return false;
    return code == shortcut_key;
}

/* Extra-modifier mode deliberately claims the bare shortcut too, so protection
 * cannot be bypassed by pressing plain Ctrl+Q; this only recognises the
 * confirming combination. */
bool rules_qp_is_extra_shortcut(uint16_t code, bool ctrl, bool alt, bool shift, bool meta,
                                uint16_t shortcut_key, uint16_t extra_mod)
{
    bool has_extra;

    if (!ctrl || meta)
        return false;
    switch (extra_mod) {
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT: has_extra = shift && !alt; break;
    case KEY_LEFTALT:
    case KEY_RIGHTALT: has_extra = alt && !shift; break;
    default: return false;
    }
    return has_extra && code == shortcut_key;
}

/* --- the relay half ------------------------------------------------------- */

static uint16_t qp_key_for(rules_state *st, const rules_qp_shortcut **sc, rules_qp_state **state,
                           uint16_t code)
{
    if (code == KEY_Q && st->cfg.qp.quit.enabled) {
        *sc = &st->cfg.qp.quit;
        *state = &st->qp_quit;
        return KEY_Q;
    }
    if (code == KEY_W && st->cfg.qp.close.enabled) {
        *sc = &st->cfg.qp.close;
        *state = &st->qp_close;
        return KEY_W;
    }
    return 0;
}

static int qp_emit_shortcut(rules_state *st, uint16_t key, rules_event *out, int cap, int n)
{
    /* The confirming press may have carried an extra modifier the application
     * must not see; drop it for the duration of the emitted chord and put it
     * back, so the user's physical key state and the stream agree again. */
    bool shift = st->mod_down[KEY_LEFTSHIFT] || st->mod_down[KEY_RIGHTSHIFT];
    bool alt = st->mod_down[KEY_LEFTALT] || st->mod_down[KEY_RIGHTALT];

    if (shift) {
        n = rules_emit(out, cap, n, EV_KEY,
                       st->mod_down[KEY_LEFTSHIFT] ? KEY_LEFTSHIFT : KEY_RIGHTSHIFT, 0);
        if (n < 0) return -1;
    }
    if (alt) {
        n = rules_emit(out, cap, n, EV_KEY, st->mod_down[KEY_LEFTALT] ? KEY_LEFTALT : KEY_RIGHTALT,
                       0);
        if (n < 0) return -1;
    }
    n = rules_emit(out, cap, n, EV_KEY, key, 1);
    if (n < 0) return -1;
    n = rules_emit(out, cap, n, EV_KEY, key, 0);
    if (n < 0) return -1;
    if (alt) {
        n = rules_emit(out, cap, n, EV_KEY, st->mod_down[KEY_LEFTALT] ? KEY_LEFTALT : KEY_RIGHTALT,
                       1);
        if (n < 0) return -1;
    }
    if (shift) {
        n = rules_emit(out, cap, n, EV_KEY,
                       st->mod_down[KEY_LEFTSHIFT] ? KEY_LEFTSHIFT : KEY_RIGHTSHIFT, 1);
        if (n < 0) return -1;
    }
    return n;
}

static void qp_notice(rules_state *st, uint64_t now, uint16_t key, const char *outcome)
{
    rules_notice_push(st, now,
                      "{\"rule\":\"quit_protection\",\"shortcut\":\"%s\",\"outcome\":\"%s\","
                      "\"app_id\":\"%s\",\"native_quit\":%s}",
                      key == KEY_Q ? "quit" : "close", outcome, st->ctx.focused_app_id,
                      key == KEY_Q ? "true" : "false");
}

int rules_qp_apply(rules_state *st, const rules_event *in, uint64_t now, rules_event *out, int cap,
                   int n, bool *handled)
{
    const rules_qp_shortcut *sc = NULL;
    rules_qp_state *qs = NULL;
    uint16_t key;
    bool ctrl, alt, shift, meta;

    if (!st->cfg.qp.enabled || in->type != EV_KEY)
        return n;
    key = qp_key_for(st, &sc, &qs, in->code);
    if (key == 0)
        return n;

    ctrl = st->mod_down[KEY_LEFTCTRL] || st->mod_down[KEY_RIGHTCTRL];
    alt = st->mod_down[KEY_LEFTALT] || st->mod_down[KEY_RIGHTALT];
    shift = st->mod_down[KEY_LEFTSHIFT] || st->mod_down[KEY_RIGHTSHIFT];
    meta = st->mod_down[KEY_LEFTMETA] || st->mod_down[KEY_RIGHTMETA];

    if (!rules_qp_scope_allows(sc->scope, st->ctx.focused_app_id, sc))
        return n; /* this app is not protected; the shortcut is ordinary */

    if (sc->mode == RULES_QP_EXTRA) {
        if (rules_qp_is_extra_shortcut(in->code, ctrl, alt, shift, meta, key, sc->extra_mod)) {
            if (in->value != 1)
                return n;
            *handled = true;
            qp_notice(st, now, key, "confirmed");
            return qp_emit_shortcut(st, key, out, cap, n);
        }
        if (rules_qp_is_base_shortcut(in->code, ctrl, alt, shift, meta, key)) {
            /* The bare shortcut is claimed on purpose, or protection would be
             * one keystroke away from being bypassed. */
            *handled = true;
            if (in->value == 1) {
                st->stat_qp_blocked++;
                qp_notice(st, now, key, "blocked");
            }
        }
        return n;
    }

    if (!rules_qp_is_base_shortcut(in->code, ctrl, alt, shift, meta, key))
        return n;

    *handled = true;

    if (sc->mode == RULES_QP_HOLD) {
        if (in->value == 1) {
            qs->pending = true;
            qs->pending_deadline = now + MS_NS(sc->hold_ms);
            qp_notice(st, now, key, "holding");
        } else if (in->value == 0) {
            if (qs->pending) {
                /* Let go before the deadline: nothing was emitted, so nothing
                 * has to be taken back. */
                qs->pending = false;
                st->stat_qp_blocked++;
                qp_notice(st, now, key, "blocked");
            }
        }
        return n;
    }

    /* Double press. */
    if (in->value != 1)
        return n;
    if (qs->armed && rules_qp_within_double(qs->armed_ts, now, (double)sc->double_ms)) {
        qs->armed = false;
        qp_notice(st, now, key, "confirmed");
        return qp_emit_shortcut(st, key, out, cap, n);
    }
    qs->armed = true;
    qs->armed_ts = now;
    st->stat_qp_blocked++;
    qp_notice(st, now, key, "armed");
    return n;
}

uint64_t rules_qp_deadline(const rules_state *st)
{
    uint64_t d = 0;

    if (!st->cfg.qp.enabled)
        return 0;
    if (st->qp_quit.pending)
        d = st->qp_quit.pending_deadline;
    if (st->qp_close.pending && (d == 0 || st->qp_close.pending_deadline < d))
        d = st->qp_close.pending_deadline;
    return d;
}

int rules_qp_tick(rules_state *st, uint64_t now, rules_event *out, int cap, int n)
{
    if (!st->cfg.qp.enabled)
        return n;

    if (st->qp_quit.pending && now >= st->qp_quit.pending_deadline) {
        st->qp_quit.pending = false;
        qp_notice(st, now, KEY_Q, "confirmed");
        n = qp_emit_shortcut(st, KEY_Q, out, cap, n);
        if (n < 0)
            return -1;
    }
    if (st->qp_close.pending && now >= st->qp_close.pending_deadline) {
        st->qp_close.pending = false;
        qp_notice(st, now, KEY_W, "confirmed");
        n = qp_emit_shortcut(st, KEY_W, out, cap, n);
        if (n < 0)
            return -1;
    }
    return n;
}
