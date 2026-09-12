/* super_key: tap the chosen key for its solo action, hold it for a modifier
 * combination.
 *
 * A verbatim port of SuperKeySupport.State.decide and .soloEffect, plus the
 * part of SuperKeyMappingGuard that still means something on Linux.
 *
 * What the guard is for on macOS: the feature rewrites the keyboard's HID
 * mapping table with hidutil so Caps Lock arrives as F18 (Caps Lock is a
 * *lock*, so nothing above the driver ever sees it go down and up, and it
 * cannot be held). That table is system state which outlives the process, so
 * a killed app leaves the user's Caps Lock silently rewired -- hence the
 * watchdog shell, the readback confirmation and the write-ahead marker.
 *
 * None of that hazard exists here. The relay *is* the remap: the source
 * device is grabbed with EVIOCGRAB, which the kernel drops when the fd
 * closes, so the mapping cannot outlive the process by construction --
 * including a SIGKILL. What does carry over from the guard is its other two
 * rules, and both are implemented:
 *
 *   hasMappingConflict  -> rules_sk_source_conflict(): a source another rule
 *                          already owns is refused, and the previous rules
 *                          stay in force, rather than silently claiming it.
 *   mappingMarkerAfterClear -> rules_release_held(): the held state is only
 *                          cleared once the releases have actually been
 *                          queued, so a full output buffer leaves the state
 *                          eligible for the next attempt instead of
 *                          recording a clear that did not happen. */
#include "rules_internal.h"

#include <linux/input-event-codes.h>
#include <string.h>

/* SuperKeySupport.State.decide, case for case. */
rules_sk_decision rules_sk_decide(rules_sk_state *st, rules_sk_event_kind kind, bool is_repeat,
                                  bool has_primary_modifiers, uint64_t ts, uint64_t threshold_ns,
                                  bool *out_repeated)
{
    bool was_alone, was_long, repeated;

    if (out_repeated)
        *out_repeated = false;

    switch (kind) {
    case RULES_SKE_TRIGGER_DOWN:
        /* A repeat arriving with nothing held is a repeat for a press this
         * state never saw; swallow it and change nothing. */
        if (!st->is_held && is_repeat)
            return RULES_SK_SWALLOW;
        if (!st->is_held) {
            st->is_alone = !has_primary_modifiers;
            st->have_down_ts = !has_primary_modifiers;
            st->down_ts = has_primary_modifiers ? 0 : ts;
            st->did_repeat = false;
        } else if (is_repeat) {
            st->did_repeat = true;
        }
        st->is_held = true;
        return RULES_SK_SWALLOW;

    case RULES_SKE_TRIGGER_UP:
        was_alone = st->is_held && st->is_alone;
        was_long = was_alone && st->have_down_ts && ts >= st->down_ts &&
                   ts - st->down_ts >= threshold_ns;
        repeated = st->did_repeat;
        st->is_held = false;
        st->is_alone = false;
        st->have_down_ts = false;
        st->down_ts = 0;
        st->did_repeat = false;
        if (out_repeated)
            *out_repeated = repeated;
        if (was_long)
            return RULES_SK_SOLO_HOLD;
        return was_alone ? RULES_SK_SOLO_TAP : RULES_SK_SWALLOW;

    case RULES_SKE_OTHER_KEY:
        if (!st->is_held)
            return RULES_SK_PASS;
        st->is_alone = false;
        st->have_down_ts = false;
        st->did_repeat = false;
        return RULES_SK_ADD_MODIFIERS;

    case RULES_SKE_OTHER_MODIFIER:
        if (st->is_held) {
            st->is_alone = false;
            st->have_down_ts = false;
            st->did_repeat = false;
        }
        return RULES_SK_PASS;
    }
    return RULES_SK_PASS;
}

/* SuperKeySupport.soloEffect. */
rules_sk_action rules_sk_solo_effect(rules_sk_action action, bool long_hold, bool repeated)
{
    switch (action) {
    case RULES_SK_NONE: return RULES_SK_NONE;
    case RULES_SK_ESCAPE: return repeated ? RULES_SK_NONE : RULES_SK_ESCAPE;
    case RULES_SK_CAPSLOCK: return repeated ? RULES_SK_NONE : RULES_SK_CAPSLOCK;
    /* The configured-key action reserves a long hold for Caps Lock, so the
     * key that switches layouts can still turn Caps Lock on. */
    case RULES_SK_KEY: return long_hold ? RULES_SK_CAPSLOCK : RULES_SK_KEY;
    }
    return RULES_SK_NONE;
}

void rules_sk_reset(rules_sk_state *st)
{
    st->is_held = false;
    st->is_alone = false;
    st->have_down_ts = false;
    st->down_ts = 0;
    st->did_repeat = false;
}

const char *rules_sk_state_name(const rules_sk_state *st)
{
    if (!st->is_held)
        return "idle";
    return st->is_alone ? "pending" : "hold";
}

bool rules_is_modifier(uint16_t code)
{
    switch (code) {
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT:
    case KEY_LEFTALT:
    case KEY_RIGHTALT:
    case KEY_LEFTMETA:
    case KEY_RIGHTMETA:
        return true;
    default:
        return false;
    }
}

/* SuperKeySupport.hasMappingConflict, in the terms this engine has: the
 * source key may not be a key another enabled rule already answers, and may
 * not be one of the modifiers this rule itself emits -- either would make the
 * key do two things at once, and the honest answer is to refuse the
 * configuration rather than pick one. */
bool rules_sk_source_conflict(const rules_config *cfg, uint16_t source)
{
    if (source == 0 || source > RULES_KEY_MAX)
        return true;

    for (unsigned i = 0; i < cfg->sk.n_mods; i++)
        if (cfg->sk.mods[i] == source)
            return true;

    if (cfg->mbs.enabled) {
        for (unsigned i = 0; i < cfg->mbs.n_bind; i++) {
            if (cfg->mbs.bind[i].key == source)
                return true;
            for (unsigned m = 0; m < cfg->mbs.bind[i].n_mods; m++)
                if (cfg->mbs.bind[i].mods[m] == source)
                    return true;
        }
    }

    if (cfg->qp.enabled) {
        if (cfg->qp.quit.enabled && (source == KEY_Q || source == cfg->qp.quit.extra_mod))
            return true;
        if (cfg->qp.close.enabled && (source == KEY_W || source == cfg->qp.close.extra_mod))
            return true;
        if ((cfg->qp.quit.enabled || cfg->qp.close.enabled) &&
            (source == KEY_LEFTCTRL || source == KEY_RIGHTCTRL))
            return true;
    }
    return false;
}

/* --- the relay half ------------------------------------------------------- */

/* LED_CAPSL is driven on the *source* devices, not on the relay's output: the
 * lamp the user looks at is on the keyboard the relay grabbed, and a grabbed
 * device still takes EV_LED writes. device_evdev.c routes EV_LED there
 * instead of to uinput; the fake records it so the tests can see it. */
static int sk_emit_caps_led(rules_state *st, rules_event *out, int cap, int n)
{
    if (!st->cfg.sk.led)
        return n;
    st->sk.caps_led = !st->sk.caps_led;
    return rules_emit(out, cap, n, EV_LED, LED_CAPSL, st->sk.caps_led ? 1 : 0);
}

static int sk_run_solo(rules_state *st, rules_sk_action effect, uint64_t now, rules_event *out,
                       int cap, int n)
{
    switch (effect) {
    case RULES_SK_NONE:
        return n;
    case RULES_SK_ESCAPE:
        n = rules_emit(out, cap, n, EV_KEY, KEY_ESC, 1);
        if (n < 0) return -1;
        return rules_emit(out, cap, n, EV_KEY, KEY_ESC, 0);
    case RULES_SK_CAPSLOCK:
        n = rules_emit(out, cap, n, EV_KEY, KEY_CAPSLOCK, 1);
        if (n < 0) return -1;
        n = rules_emit(out, cap, n, EV_KEY, KEY_CAPSLOCK, 0);
        if (n < 0) return -1;
        return sk_emit_caps_led(st, out, cap, n);
    case RULES_SK_KEY:
        /* The app maps this key to switching the input source: choosing a
         * keyboard layout is a desktop setting and the helper has no business
         * calling gsettings or kded as root. */
        rules_notice_push(st, now, "{\"rule\":\"super_key\",\"action\":\"tap_key\",\"key\":%u}",
                          st->cfg.sk.tap_key);
        if (st->cfg.sk.tap_key == 0)
            return n;
        n = rules_emit(out, cap, n, EV_KEY, st->cfg.sk.tap_key, 1);
        if (n < 0) return -1;
        return rules_emit(out, cap, n, EV_KEY, st->cfg.sk.tap_key, 0);
    }
    return n;
}

int rules_sk_apply(rules_state *st, const rules_event *in, uint64_t now, rules_event *out, int cap,
                   int n, bool *handled)
{
    const rules_sk_config *cfg = &st->cfg.sk;
    rules_sk_decision d;
    bool repeated = false;

    if (!cfg->enabled || in->type != EV_KEY)
        return n;

    if (in->code == cfg->source) {
        *handled = true; /* the key itself never reaches an application */
        if (in->value != 0) {
            rules_sk_decide(&st->sk, RULES_SKE_TRIGGER_DOWN, in->value == 2,
                            st->mod_held_count > 0, now, cfg->hold_threshold_ns, NULL);
            return n;
        }

        d = rules_sk_decide(&st->sk, RULES_SKE_TRIGGER_UP, false, false, now,
                            cfg->hold_threshold_ns, &repeated);
        if (st->sk.mods_down) {
            n = rules_emit_mods(out, cap, n, cfg->mods, cfg->n_mods, false);
            if (n < 0)
                return -1;
            st->sk.mods_down = false;
        }
        if (d == RULES_SK_SOLO_TAP || d == RULES_SK_SOLO_HOLD) {
            rules_sk_action effect =
                rules_sk_solo_effect(cfg->tap_action, d == RULES_SK_SOLO_HOLD, repeated);
            if (d == RULES_SK_SOLO_TAP)
                st->stat_taps++;
            else
                st->stat_holds++;
            return sk_run_solo(st, effect, now, out, cap, n);
        }
        return n;
    }

    /* Anything else. A modifier changing state cancels the solo action but is
     * passed through; an ordinary key press while the source is held stamps
     * the combination on it. */
    if (rules_is_modifier(in->code)) {
        rules_sk_decide(&st->sk, RULES_SKE_OTHER_MODIFIER, false, false, now,
                        cfg->hold_threshold_ns, NULL);
        return n;
    }
    if (in->value != 1)
        return n; /* a release or a repeat resolves nothing */

    d = rules_sk_decide(&st->sk, RULES_SKE_OTHER_KEY, false, false, now, cfg->hold_threshold_ns,
                        NULL);
    if (d == RULES_SK_ADD_MODIFIERS && !st->sk.mods_down) {
        n = rules_emit_mods(out, cap, n, cfg->mods, cfg->n_mods, true);
        if (n < 0)
            return -1;
        st->sk.mods_down = true;
        st->stat_holds++;
    }
    return n;
}
