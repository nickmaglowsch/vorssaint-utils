/* Write a recorded evdev event stream for --replay.
 *
 * The output is byte-for-byte what `cat /dev/input/eventN` produces: packed
 * struct input_event records with CLOCK_REALTIME timestamps. A capture taken
 * on a machine with real hardware drops straight into the same --replay path.
 *
 * The script below is a short typing session that exercises every branch of
 * both rules: a short Caps Lock tap, a long Caps Lock hold with no other key,
 * Caps Lock used as a modifier (Caps+C), a switch chattering on KEY_E, and
 * ordinary typing with kernel autorepeat in the middle. */
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

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "stream.bin";

    out = fopen(path, "wb");
    if (!out) {
        perror("fopen");
        return 1;
    }
    t_us = 1000000ULL; /* start at t=1s so timestamps are never zero */

    /* 1. Caps Lock tap: 45 ms down. Expect ESC down + ESC up. */
    key(KEY_CAPSLOCK, 1, 0);
    key(KEY_CAPSLOCK, 0, 45);

    /* 2. Ordinary typing, well clear of the chatter window. */
    key(KEY_H, 1, 300);
    key(KEY_H, 0, 40);
    key(KEY_I, 1, 90);
    key(KEY_I, 0, 55);

    /* 3. Caps Lock held for 400 ms with nothing else pressed. Expect the
     *    timer to resolve it at +200 ms: CTRL down, then CTRL up on release. */
    key(KEY_CAPSLOCK, 1, 250);
    key(KEY_CAPSLOCK, 0, 400);

    /* 4. Caps Lock as a modifier: Caps down, C down 80 ms later. The other
     *    key resolves the hold before the 200 ms timer would.
     *    Expect CTRL down, C down, C up, CTRL up. */
    key(KEY_CAPSLOCK, 1, 300);
    key(KEY_C, 1, 80);
    key(KEY_C, 0, 60);
    key(KEY_CAPSLOCK, 0, 70);

    /* 5. A chattering switch on KEY_E: one real press, then the contact
     *    bounces twice within the 40 ms window. Expect one E down/up pair. */
    key(KEY_E, 1, 400);
    key(KEY_E, 0, 30);
    key(KEY_E, 1, 8);  /* bounce: 8 ms after release */
    key(KEY_E, 0, 6);
    key(KEY_E, 1, 11); /* bounce: 11 ms after release */
    key(KEY_E, 0, 9);

    /* 6. A deliberate double tap on KEY_E outside the window (60 ms apart),
     *    which must survive: the filter may not eat intentional repeats. */
    key(KEY_E, 1, 60);
    key(KEY_E, 0, 35);

    /* 7. Kernel autorepeat on KEY_DOWN, which is not switch chatter and must
     *    pass through untouched. */
    key(KEY_DOWN, 1, 500);
    key(KEY_DOWN, 2, 250);
    key(KEY_DOWN, 2, 33);
    key(KEY_DOWN, 2, 33);
    key(KEY_DOWN, 0, 33);

    /* 8. Mouse motion, which must not resolve a pending tap-hold. */
    key(KEY_CAPSLOCK, 1, 400);
    advance(20);
    ev(EV_REL, REL_X, 7);
    ev(EV_REL, REL_Y, -3);
    syn();
    key(KEY_CAPSLOCK, 0, 30); /* still a tap: 50 ms total */

    fclose(out);
    fprintf(stderr, "wrote %s\n", path);
    return 0;
}
