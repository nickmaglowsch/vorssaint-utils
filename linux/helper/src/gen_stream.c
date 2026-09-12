/* Write a recorded evdev event stream for --replay.
 *
 * The output is byte-for-byte what `cat /dev/input/eventN` produces: packed
 * struct input_event records with CLOCK_REALTIME timestamps. A capture taken
 * on a machine with real hardware drops straight into the same --replay path.
 *
 * The script below is a short session that exercises every rule at least
 * once, in the order the dispatcher runs them: a chattering key, a bouncing
 * mouse button, the super key tapped and held and used as a modifier,
 * Ctrl+Q held and released early, an extra mouse button, a wheel notch
 * through the inverter and the smooth glide, and ordinary typing with kernel
 * autorepeat in the middle so the pass-through path is covered too. */
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *out;
static uint64_t t_us; /* stream clock, microseconds */

static void ev(uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ie;
    memset(&ie, 0, sizeof(ie));
    ie.input_event_sec = (long)(t_us / 1000000ULL);
    ie.input_event_usec = (long)(t_us % 1000000ULL);
    ie.type = type;
    ie.code = code;
    ie.value = value;
    fwrite(&ie, sizeof(ie), 1, out);
}

static void syn(void) { ev(EV_SYN, SYN_REPORT, 0); }
static void advance(uint64_t ms) { t_us += ms * 1000ULL; }

static void key(uint16_t code, int32_t value, uint64_t after_ms)
{
    advance(after_ms);
    ev(EV_KEY, code, value);
    syn();
}

static void rel(uint16_t code, int32_t value, uint64_t after_ms)
{
    advance(after_ms);
    ev(EV_REL, code, value);
    syn();
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "stream.bin";

    out = fopen(path, "wb");
    if (!out) {
        perror("fopen");
        return 1;
    }
    t_us = 1000000ULL; /* start at t=1s so timestamps are never zero */

    /* 1. Ordinary typing, well clear of any filter window. */
    key(KEY_H, 1, 0);
    key(KEY_H, 0, 40);
    key(KEY_I, 1, 90);
    key(KEY_I, 0, 55);

    /* 2. keyboard_debounce: a chattering switch on KEY_E. One real press,
     *    then the contact bounces twice inside the window. The macOS filter
     *    suppresses the bouncing presses and lets every release through. */
    key(KEY_E, 1, 400);
    key(KEY_E, 0, 30);
    key(KEY_E, 1, 8); /* bounce: 8 ms after the release */
    key(KEY_E, 0, 6);
    key(KEY_E, 1, 11); /* bounce: 11 ms after the release */
    key(KEY_E, 0, 9);

    /* 3. A deliberate double tap on KEY_E well outside the window, which must
     *    survive: the filter may not eat intentional repeats. */
    key(KEY_E, 1, 60);
    key(KEY_E, 0, 35);

    /* 4. Kernel autorepeat on KEY_DOWN, which is not switch chatter and must
     *    pass through untouched. */
    key(KEY_DOWN, 1, 500);
    key(KEY_DOWN, 2, 250);
    key(KEY_DOWN, 2, 33);
    key(KEY_DOWN, 0, 33);

    /* 5. mouse_click_debounce: a healthy left click, then a bounce click
     *    inside the 25 ms window whose Down and Up are both suppressed. */
    key(BTN_LEFT, 1, 400);
    key(BTN_LEFT, 0, 60);
    key(BTN_LEFT, 1, 10); /* bounce */
    key(BTN_LEFT, 0, 8);
    key(BTN_LEFT, 1, 300); /* a real second click, well clear */
    key(BTN_LEFT, 0, 60);

    /* 6. super_key: Caps Lock tapped for 45 ms. Under the ported state
     *    machine that is a solo tap, and the configured tap action runs. */
    key(KEY_CAPSLOCK, 1, 400);
    key(KEY_CAPSLOCK, 0, 45);

    /* 7. super_key held alone past the 500 ms threshold: a solo hold. */
    key(KEY_CAPSLOCK, 1, 300);
    key(KEY_CAPSLOCK, 0, 600);

    /* 8. super_key as a modifier: Caps down, C down 80 ms later. The other
     *    key stamps the configured combination and cancels the solo action. */
    key(KEY_CAPSLOCK, 1, 300);
    key(KEY_C, 1, 80);
    key(KEY_C, 0, 60);
    key(KEY_CAPSLOCK, 0, 70);

    /* 9. quit_protection, hold mode: Ctrl+Q released before the 800 ms
     *    deadline is blocked entirely, then a second one held past it is
     *    confirmed by the timer. */
    key(KEY_LEFTCTRL, 1, 400);
    key(KEY_Q, 1, 60);
    key(KEY_Q, 0, 200); /* let go early */
    key(KEY_Q, 1, 300);
    key(KEY_Q, 0, 1200); /* past the deadline; the timer already fired */
    key(KEY_LEFTCTRL, 0, 40);

    /* 10. mouse_button_shortcut: BTN_SIDE mapped to a chord. */
    key(BTN_SIDE, 1, 400);
    key(BTN_SIDE, 0, 90);

    /* 11. scroll_invert and smooth_scroll: two wheel notches. The inverter
     *     flips them and the glide consumes the notch, re-emitting it as
     *     REL_WHEEL_HI_RES steps that the timer drives out over time. */
    rel(REL_WHEEL, 1, 400);
    rel(REL_WHEEL, 1, 20);

    /* 12. Mouse motion, which must reach the output untouched. */
    advance(600);
    ev(EV_REL, REL_X, 7);
    ev(EV_REL, REL_Y, -3);
    syn();

    fclose(out);
    fprintf(stderr, "wrote %s\n", path);
    return 0;
}
