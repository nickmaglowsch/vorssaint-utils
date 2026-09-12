/* scroll_invert and smooth_scroll.
 *
 * scroll_invert mirrors ScrollWheelSupport.inversionPlan; smooth_scroll is a
 * verbatim port of SmoothScrollSupport, including the exponential-decay
 * easing, the frame clamp, the whole-pixel carry and the landing frame.
 *
 * Two things are deliberately different from macOS, and only two:
 *
 *  - The budget is counted in REL_*_HI_RES units rather than pixels. The
 *    kernel fixes one wheel notch at 120 units, so the default step (40) has
 *    to travel exactly 120 units or the relay would silently change how far a
 *    notch scrolls. The step then scales that, which is what makes the speed
 *    setting work, exactly as the pixel step does on macOS.
 *  - The Shift redirect is off by default. On macOS the window server turns a
 *    Shift-held vertical tick sideways *above* the tap, so the tap has to
 *    perform the redirect itself once it swallows the tick. On Linux the
 *    application does that, below the relay, so performing it here would
 *    scroll sideways twice. */
#include "rules_internal.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <string.h>

/* --- scroll_invert -------------------------------------------------------- */

/* ScrollWheelSupport.inversionPlan, verbatim. */
rules_inversion_plan rules_scroll_inversion_plan(bool has_vertical, bool has_horizontal,
                                                 bool shift_redirects_vertical,
                                                 bool invert_vertical, bool invert_horizontal)
{
    bool redirects = shift_redirects_vertical && has_vertical && !has_horizontal;
    rules_inversion_plan plan;

    plan.vertical = has_vertical && (redirects ? invert_horizontal : invert_vertical);
    plan.horizontal = has_horizontal && invert_horizontal;
    return plan;
}

static bool scroll_class_enabled(const rules_state *st, uint8_t dev_class, bool mouse,
                                 bool touchpad)
{
    (void)st;
    switch (dev_class) {
    case RULES_DEV_TOUCHPAD: return touchpad;
    case RULES_DEV_MOUSE: return mouse;
    /* A device udev could not classify is treated as a mouse: on a machine
     * with no ID_INPUT_TOUCHPAD property at all, refusing to act would make
     * the feature look broken, and inverting a wheel is reversible. */
    default: return mouse;
    }
}

static bool is_vertical_wheel(uint16_t code)
{
    return code == REL_WHEEL || code == REL_WHEEL_HI_RES;
}

static bool is_horizontal_wheel(uint16_t code)
{
    return code == REL_HWHEEL || code == REL_HWHEEL_HI_RES;
}

/* Runs before smooth_scroll, so the glide is fed the direction the user asked
 * for and the two features cannot cancel each other out -- the same ordering
 * the macOS services keep with the synthetic-event tag. */
int rules_scroll_apply(rules_state *st, rules_event *in, uint64_t now, rules_event *out, int cap,
                       int n, bool *handled)
{
    const rules_scroll_config *cfg = &st->cfg.scroll;
    rules_inversion_plan plan;
    bool shift;

    (void)now;
    (void)out;
    (void)cap;
    (void)handled;
    if (!cfg->enabled || in->type != EV_REL)
        return n;
    if (!is_vertical_wheel(in->code) && !is_horizontal_wheel(in->code))
        return n;
    if (!scroll_class_enabled(st, in->dev_class, cfg->mouse, cfg->touchpad))
        return n;

    shift = st->mod_down[KEY_LEFTSHIFT] || st->mod_down[KEY_RIGHTSHIFT];
    plan = rules_scroll_inversion_plan(is_vertical_wheel(in->code), is_horizontal_wheel(in->code),
                                       cfg->shift_redirects_vertical && shift, cfg->vertical,
                                       cfg->horizontal);
    if ((is_vertical_wheel(in->code) && plan.vertical) ||
        (is_horizontal_wheel(in->code) && plan.horizontal))
        in->value = -in->value;
    return n;
}

/* --- smooth_scroll: the ported math -------------------------------------- */

static const double SLOW_RESPONSE_TIME = 0.16;
static const double FAST_RESPONSE_TIME = 0.04;
static const double MINIMUM_GLIDE_SPEED = 60.0;
/* ScrollWheelSupport.pointsPerLine */
static const double POINTS_PER_LINE = 10.0;

int rules_smooth_sanitize_step(int value)
{
    if (value == 0)
        return RULES_SMOOTH_STEP_DEFAULT;
    if (value < RULES_SMOOTH_STEP_MIN)
        return RULES_SMOOTH_STEP_MIN;
    if (value > RULES_SMOOTH_STEP_MAX)
        return RULES_SMOOTH_STEP_MAX;
    return value;
}

int rules_smooth_sanitize_response(int value)
{
    if (value < RULES_SMOOTH_RESPONSE_MIN)
        return RULES_SMOOTH_RESPONSE_MIN;
    if (value > RULES_SMOOTH_RESPONSE_MAX)
        return RULES_SMOOTH_RESPONSE_MAX;
    return value;
}

double rules_smooth_ticks(double line, double fixed_point)
{
    return fixed_point != 0 ? fixed_point : line;
}

double rules_smooth_continuous_distance(double fixed_point_delta, double point_delta, double step)
{
    double pixels;

    if (!isfinite(fixed_point_delta) || !isfinite(point_delta) || !isfinite(step))
        return 0;
    pixels = point_delta != 0 ? point_delta : fixed_point_delta * POINTS_PER_LINE;
    return pixels * (step / (double)RULES_SMOOTH_STEP_DEFAULT);
}

static bool directions_oppose(double lhs, double rhs)
{
    return lhs != 0 && rhs != 0 && ((lhs < 0) != (rhs < 0));
}

/* SmoothScrollSupport.frameDelta. Exponential decay has the composition
 * property the port needs: two short intervals leave the same remainder as
 * one interval of their combined duration, so the glide looks identical at
 * 60 Hz and 120 Hz and does not depend on the timer cadence. */
double rules_smooth_frame_delta(double remaining, double elapsed_s, int response)
{
    double magnitude, clamped, normalized, response_time, eased, emitted;

    if (!isfinite(remaining) || remaining == 0 || !isfinite(elapsed_s) || elapsed_s <= 0)
        return 0;
    magnitude = fabs(remaining);
    if (magnitude <= RULES_SMOOTH_FINISH_THRESHOLD)
        return remaining;
    clamped = elapsed_s < RULES_SMOOTH_MAX_FRAME_INTERVAL_S ? elapsed_s
                                                            : RULES_SMOOTH_MAX_FRAME_INTERVAL_S;
    normalized = (double)(rules_smooth_sanitize_response(response) - RULES_SMOOTH_RESPONSE_MIN) /
                 (double)(RULES_SMOOTH_RESPONSE_MAX - RULES_SMOOTH_RESPONSE_MIN);
    response_time = SLOW_RESPONSE_TIME - normalized * (SLOW_RESPONSE_TIME - FAST_RESPONSE_TIME);
    eased = magnitude * (1.0 - exp(-clamped / response_time));
    emitted = MINIMUM_GLIDE_SPEED * clamped;
    if (eased > emitted)
        emitted = eased;
    if (emitted > magnitude)
        emitted = magnitude;
    return remaining < 0 ? -emitted : emitted;
}

void rules_smooth_whole_pixels(double distance, double carry, double *pixels, double *out_carry)
{
    double total = distance + carry;
    double whole;

    if (!isfinite(total)) {
        *pixels = 0;
        *out_carry = 0;
        return;
    }
    whole = trunc(total);
    *pixels = whole;
    *out_carry = total - whole;
}

double rules_smooth_final_pixels(double distance, double carry)
{
    double total = distance + carry;

    if (!isfinite(total))
        return 0;
    return round(total); /* round-half-away-from-zero, as .toNearestOrAwayFromZero */
}

double rules_smooth_carry(double current, double continuing)
{
    return directions_oppose(current, continuing) ? 0 : current;
}

void rules_smooth_axes(double vertical, double horizontal, bool shift_pressed, double *out_v,
                       double *out_h)
{
    if (!shift_pressed || vertical == 0 || horizontal != 0) {
        *out_v = vertical;
        *out_h = horizontal;
        return;
    }
    *out_v = 0;
    *out_h = vertical; /* the redirect keeps the sign; measured, not assumed */
}

/* SmoothScrollSupport.Engine.add: a reversal replaces the old tail on that
 * axis, so the first opposite tick answers at once instead of first having to
 * unwind what is left. */
void rules_smooth_engine_add(rules_smooth_axis *axis, double distance)
{
    if (!isfinite(distance) || distance == 0)
        return;
    if (directions_oppose(distance, axis->remaining)) {
        axis->remaining = distance;
        axis->carry = 0; /* carry(_:continuing:) drops leftovers on a reversal */
    } else {
        axis->remaining += distance;
    }
}

double rules_smooth_engine_advance(rules_smooth_axis *axis, double elapsed_s, int response)
{
    double delta = rules_smooth_frame_delta(axis->remaining, elapsed_s, response);
    axis->remaining -= delta;
    return delta;
}

/* --- smooth_scroll: the relay half --------------------------------------- */

static bool smooth_active(const rules_state *st)
{
    return st->smooth_v.remaining != 0 || st->smooth_h.remaining != 0;
}

uint64_t rules_smooth_deadline(const rules_state *st)
{
    if (!st->cfg.smooth.enabled || !smooth_active(st))
        return 0;
    if (!st->smooth_have_frame)
        return 0;
    return st->smooth_last_frame + (uint64_t)(RULES_SMOOTH_FRAME_INTERVAL_S * 1e9);
}

/* One wheel notch, in hi-res units, at the configured step. */
static double smooth_notch_units(const rules_smooth_config *cfg)
{
    return (double)RULES_HI_RES_PER_NOTCH *
           ((double)rules_smooth_sanitize_step((int)cfg->step) /
            (double)RULES_SMOOTH_STEP_DEFAULT);
}

int rules_smooth_apply(rules_state *st, const rules_event *in, uint64_t now, rules_event *out,
                       int cap, int n, bool *handled)
{
    const rules_smooth_config *cfg = &st->cfg.smooth;
    double ticks, distance, v, h;

    (void)out;
    (void)cap;
    if (!cfg->enabled || in->type != EV_REL)
        return n;
    if (in->code != REL_WHEEL && in->code != REL_HWHEEL)
        return n;
    if (!scroll_class_enabled(st, in->dev_class, cfg->mouse, cfg->touchpad))
        return n;

    /* The notch is consumed: the glide below re-emits it as hi-res steps, and
     * letting the original through as well would scroll twice. */
    *handled = true;

    ticks = rules_smooth_ticks((double)in->value, 0);
    distance = ticks * smooth_notch_units(cfg);
    if (in->code == REL_WHEEL)
        rules_smooth_axes(distance, 0, false, &v, &h);
    else
        rules_smooth_axes(0, distance, false, &v, &h);

    st->smooth_v.carry = rules_smooth_carry(st->smooth_v.carry, v);
    st->smooth_h.carry = rules_smooth_carry(st->smooth_h.carry, h);
    rules_smooth_engine_add(&st->smooth_v, v);
    rules_smooth_engine_add(&st->smooth_h, h);

    if (!st->smooth_have_frame) {
        st->smooth_have_frame = true;
        st->smooth_last_frame = now;
    }
    return n;
}

/* Emit one axis's frame: whole hi-res units now, the fraction next frame, and
 * a whole REL_WHEEL notch every time 120 units have gone out, because a
 * device that reports hi-res is required to report the low-resolution axis
 * too and applications that read only REL_WHEEL would otherwise see nothing. */
static int smooth_emit_axis(rules_smooth_axis *axis, uint16_t hi_code, uint16_t lo_code,
                            double delta, bool finished, rules_event *out, int cap, int n)
{
    double units;

    if (finished) {
        units = rules_smooth_final_pixels(delta, axis->carry);
        axis->carry = 0;
    } else {
        double carry;
        rules_smooth_whole_pixels(delta, axis->carry, &units, &carry);
        axis->carry = carry;
    }
    if (units == 0)
        return n;

    n = rules_emit(out, cap, n, EV_REL, hi_code, (int32_t)units);
    if (n < 0)
        return -1;

    axis->emitted_units += (int32_t)units;
    while (axis->emitted_units >= RULES_HI_RES_PER_NOTCH) {
        axis->emitted_units -= RULES_HI_RES_PER_NOTCH;
        n = rules_emit(out, cap, n, EV_REL, lo_code, 1);
        if (n < 0)
            return -1;
    }
    while (axis->emitted_units <= -RULES_HI_RES_PER_NOTCH) {
        axis->emitted_units += RULES_HI_RES_PER_NOTCH;
        n = rules_emit(out, cap, n, EV_REL, lo_code, -1);
        if (n < 0)
            return -1;
    }
    return n;
}

int rules_smooth_tick(rules_state *st, uint64_t now, rules_event *out, int cap, int n)
{
    double elapsed_s, dv, dh;
    bool finished;

    if (!st->cfg.smooth.enabled || !smooth_active(st) || !st->smooth_have_frame)
        return n;
    if (now <= st->smooth_last_frame)
        return n;

    elapsed_s = (double)(now - st->smooth_last_frame) / 1e9;
    st->smooth_last_frame = now;

    dv = rules_smooth_engine_advance(&st->smooth_v, elapsed_s, (int)st->cfg.smooth.response);
    dh = rules_smooth_engine_advance(&st->smooth_h, elapsed_s, (int)st->cfg.smooth.response);
    finished = !smooth_active(st);

    n = smooth_emit_axis(&st->smooth_v, REL_WHEEL_HI_RES, REL_WHEEL, dv, finished, out, cap, n);
    if (n < 0)
        return -1;
    n = smooth_emit_axis(&st->smooth_h, REL_HWHEEL_HI_RES, REL_HWHEEL, dh, finished, out, cap, n);
    if (n < 0)
        return -1;

    st->stat_smooth_frames++;
    if (finished)
        st->smooth_have_frame = false;
    return n;
}
