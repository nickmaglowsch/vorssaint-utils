/* mouse_button_shortcut: the extra mouse buttons and the horizontal tilt
 * press a key chord, and one button can instead be held and dragged for the
 * workspace gestures.
 *
 * Ports MouseButtonShortcutSupport (canMap, SideWheelGestureGate,
 * sideWheelInput) and MouseSpacesGestureSupport (Tracker, resolved, canBind).
 *
 * The gesture does NOT switch workspaces here. Which workspace is next is a
 * compositor question with four different answers (KWin D-Bus, our GNOME
 * extension, Hyprland/Sway IPC, X11 _NET_CURRENT_DESKTOP -- see WP-C1), none
 * of which a root daemon should be talking to. The helper emits an Event
 * signal saying what the hand did and the app acts on it. */
#include "rules_internal.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <string.h>

/* MouseButtonShortcutSupport.canMap. CoreGraphics numbers left/right/middle
 * 0/1/2 and everything from 3 up is an extra button; the evdev equivalent of
 * "3 and up" is the BTN_SIDE..BTN_TASK block, with left, right and middle
 * deliberately excluded because they belong to the click filter. */
bool rules_mbs_can_map(int32_t input)
{
    if (input == RULES_TILT_LEFT || input == RULES_TILT_RIGHT)
        return true;
    return input >= BTN_SIDE && input <= BTN_TASK;
}

/* MouseSpacesGestureSupport.canBind: the same extra buttons minus the tilt --
 * a tilt is a tick, not a press, so there is nothing to hold and nothing to
 * measure. */
bool rules_gesture_can_bind(int32_t input)
{
    return input >= BTN_SIDE && input <= BTN_TASK;
}

/* MouseButtonShortcutSupport.SideWheelGestureGate.shouldFire. A wheel driver
 * emits several packets for one physical tilt; keep the first of each
 * direction in a burst and let a quiet gap begin a new one, with no timer and
 * no work between events. */
bool rules_tilt_should_fire(rules_tilt_gate *gate, int32_t input, uint64_t ts)
{
    uint8_t bit;

    if (input == RULES_TILT_LEFT)
        bit = 1;
    else if (input == RULES_TILT_RIGHT)
        bit = 2;
    else
        return false;

    if (gate->have_last && ts >= gate->last_ts && ts - gate->last_ts <= RULES_TILT_QUIET_NS) {
        gate->last_ts = ts;
    } else {
        gate->last_ts = ts;
        gate->have_last = true;
        gate->fired_directions = 0;
    }

    if (gate->fired_directions & bit)
        return false;
    gate->fired_directions |= bit;
    return true;
}

void rules_tilt_reset(rules_tilt_gate *gate) { memset(gate, 0, sizeof(*gate)); }

/* MouseSpacesGestureSupport.resolved: read the way a trackpad reads a swipe,
 * the desk follows the hand. Only the two directions swap; dragging up opens
 * the overview on both devices. */
rules_gesture_action rules_gesture_resolved(rules_gesture_action a, bool follows_drag)
{
    if (!follows_drag)
        return a;
    if (a == RULES_GESTURE_SPACE_LEFT)
        return RULES_GESTURE_SPACE_RIGHT;
    if (a == RULES_GESTURE_SPACE_RIGHT)
        return RULES_GESTURE_SPACE_LEFT;
    return a;
}

/* Tracker.committedAxis: whichever threshold is crossed first claims the
 * press; a step that crosses both is read as the one that went further past
 * its own. */
static int gesture_committed_axis(const rules_gesture_state *g)
{
    double horizontal = fabs(g->acc_x) / RULES_GESTURE_SPACE_STEP;
    double vertical = fabs(g->acc_y) / RULES_GESTURE_OVERVIEW_STEP;

    if (horizontal >= 1 && horizontal >= vertical)
        return 1;
    if (vertical >= 1)
        return 2;
    return 0;
}

/* Tracker.advance. Positions are accumulated pointer deltas from the press,
 * in the same top-left origin the Swift uses -- evdev's REL_Y is positive
 * downward, which is what that origin means, so the vertical directions carry
 * over unchanged. */
rules_gesture_action rules_gesture_advance(rules_gesture_state *g, double x, double y,
                                           double now_s)
{
    int resolved;

    if (!isfinite(x) || !isfinite(y))
        return RULES_GESTURE_NONE;
    g->acc_x += x;
    g->acc_y += y;

    resolved = g->axis ? g->axis : gesture_committed_axis(g);
    if (resolved == 0)
        return RULES_GESTURE_NONE;

    if (resolved == 2) {
        g->axis = 2;
        /* An overview is a toggle: saying it twice in one press would open it
         * and close it again. */
        if (g->did_fire)
            return RULES_GESTURE_NONE;
        g->did_fire = true;
        return g->acc_y < 0 ? RULES_GESTURE_OVERVIEW : RULES_GESTURE_APP_OVERVIEW;
    }

    g->axis = 1;
    if (g->have_fired_at && now_s - g->last_fired_at_s < RULES_GESTURE_COOLDOWN_S) {
        /* Bank at most one further change while the cooldown runs, so a fast
         * flick does not keep arriving after the hand has stopped. */
        if (g->acc_x > RULES_GESTURE_SPACE_STEP)
            g->acc_x = RULES_GESTURE_SPACE_STEP;
        else if (g->acc_x < -RULES_GESTURE_SPACE_STEP)
            g->acc_x = -RULES_GESTURE_SPACE_STEP;
        return RULES_GESTURE_NONE;
    }
    if (fabs(g->acc_x) < RULES_GESTURE_SPACE_STEP)
        return RULES_GESTURE_NONE;

    {
        bool goes_left = g->acc_x < 0;
        /* The surplus carries over, so an even drag keeps an even rhythm
         * instead of losing whatever overshot the threshold. */
        g->acc_x += goes_left ? RULES_GESTURE_SPACE_STEP : -RULES_GESTURE_SPACE_STEP;
        g->last_fired_at_s = now_s;
        g->have_fired_at = true;
        g->did_fire = true;
        return goes_left ? RULES_GESTURE_SPACE_LEFT : RULES_GESTURE_SPACE_RIGHT;
    }
}

/* --- the relay half ------------------------------------------------------- */

static const char *gesture_name(rules_gesture_action a)
{
    switch (a) {
    case RULES_GESTURE_SPACE_LEFT: return "workspace_left";
    case RULES_GESTURE_SPACE_RIGHT: return "workspace_right";
    case RULES_GESTURE_OVERVIEW: return "overview";
    case RULES_GESTURE_APP_OVERVIEW: return "app_overview";
    default: return "none";
    }
}

static const rules_mbs_binding *mbs_find(const rules_mbs_config *cfg, int32_t input, unsigned *idx)
{
    for (unsigned i = 0; i < cfg->n_bind; i++) {
        if (cfg->bind[i].input == input) {
            if (idx)
                *idx = i;
            return &cfg->bind[i];
        }
    }
    return NULL;
}

static int mbs_press_chord(rules_state *st, const rules_mbs_binding *b, rules_event *out, int cap,
                           int n)
{
    (void)st;
    n = rules_emit_mods(out, cap, n, b->mods, b->n_mods, true);
    if (n < 0)
        return -1;
    return rules_emit(out, cap, n, EV_KEY, b->key, 1);
}

static int mbs_release_chord(rules_state *st, const rules_mbs_binding *b, rules_event *out, int cap,
                             int n)
{
    (void)st;
    n = rules_emit(out, cap, n, EV_KEY, b->key, 0);
    if (n < 0)
        return -1;
    return rules_emit_mods(out, cap, n, b->mods, b->n_mods, false);
}

int rules_mbs_apply(rules_state *st, const rules_event *in, uint64_t now, rules_event *out, int cap,
                    int n, bool *handled)
{
    const rules_mbs_config *cfg = &st->cfg.mbs;
    const rules_mbs_binding *b;
    unsigned idx = 0;

    if (!cfg->enabled)
        return n;

    /* The bound gesture button: its press is withheld, because a press that
     * never becomes a drag is still an ordinary click and has to be replayed
     * on release. */
    if (in->type == EV_KEY && cfg->gesture_button != 0 && in->code == cfg->gesture_button &&
        rules_gesture_can_bind(cfg->gesture_button)) {
        *handled = true;
        if (in->value == 1) {
            memset(&st->gesture, 0, sizeof(st->gesture));
            st->gesture.active = true;
            st->gesture.down_ts = now;
        } else if (in->value == 0 && st->gesture.active) {
            bool fired = st->gesture.did_fire;
            st->gesture.active = false;
            if (!fired) {
                n = rules_emit(out, cap, n, EV_KEY, in->code, 1);
                if (n < 0)
                    return -1;
                n = rules_emit(out, cap, n, EV_KEY, in->code, 0);
            }
        }
        return n;
    }

    /* Pointer motion while that button is held feeds the tracker. The motion
     * itself is not swallowed: the cursor must keep moving, which is the one
     * place this differs from the macOS drag, where the dragged event IS the
     * motion. */
    if (in->type == EV_REL && st->gesture.active && (in->code == REL_X || in->code == REL_Y)) {
        rules_gesture_action a =
            rules_gesture_advance(&st->gesture, in->code == REL_X ? (double)in->value : 0.0,
                                  in->code == REL_Y ? (double)in->value : 0.0, (double)now / 1e9);
        if (a != RULES_GESTURE_NONE) {
            a = rules_gesture_resolved(a, cfg->gesture_follows_drag);
            rules_notice_push(st, now,
                              "{\"rule\":\"mouse_button_shortcut\",\"action\":\"%s\","
                              "\"button\":%u}",
                              gesture_name(a), cfg->gesture_button);
        }
        return n;
    }

    /* A bound extra button. */
    if (in->type == EV_KEY && rules_mbs_can_map((int32_t)in->code)) {
        b = mbs_find(cfg, (int32_t)in->code, &idx);
        if (!b)
            return n;
        *handled = true;
        if (in->value == 1 && !st->mbs_held[idx]) {
            st->mbs_held[idx] = true;
            return mbs_press_chord(st, b, out, cap, n);
        }
        if (in->value == 0 && st->mbs_held[idx]) {
            st->mbs_held[idx] = false;
            return mbs_release_chord(st, b, out, cap, n);
        }
        return n;
    }

    /* Horizontal tilt, low or high resolution. sideWheelInput's sign rule:
     * AppKit calls a positive horizontal delta movement to the left, and so
     * does evdev's REL_HWHEEL, so the mapping carries over unchanged. */
    if (in->type == EV_REL && (in->code == REL_HWHEEL || in->code == REL_HWHEEL_HI_RES) &&
        in->value != 0) {
        int32_t input = in->value > 0 ? RULES_TILT_LEFT : RULES_TILT_RIGHT;
        b = mbs_find(cfg, input, &idx);
        if (!b)
            return n;
        *handled = true;
        if (!rules_tilt_should_fire(&st->tilt, input, now))
            return n;
        n = mbs_press_chord(st, b, out, cap, n);
        if (n < 0)
            return -1;
        return mbs_release_chord(st, b, out, cap, n);
    }

    return n;
}
