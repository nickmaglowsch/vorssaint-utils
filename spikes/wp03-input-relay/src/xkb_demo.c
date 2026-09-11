/* Why the relay does not need to know the keyboard layout.
 *
 * The relay works at the evdev keycode level. A keycode names a physical
 * switch position; the keysym it produces is decided much later, by whoever
 * holds the keymap: the compositor (Wayland) or the X server. The virtual
 * uinput device is an ordinary keyboard, so the session applies the very same
 * keymap to it as to the hardware one, and the layout is "mirrored" without
 * the relay doing anything.
 *
 * This program makes that concrete: it maps the same evdev keycodes under
 * several layouts and prints the keysyms. The keycodes are constant down the
 * columns; only the keysyms move. An implementation that remapped keysyms
 * instead would have to be told the layout and would break on every switch.
 *
 * The X11/xkb keycode is the evdev code plus 8 (XKB_KEYCODE_OFFSET). */
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#define XKB_KEYCODE_OFFSET 8

struct probe {
    const char *label;
    uint16_t evdev_code;
};

static const struct probe PROBES[] = {
    {"KEY_Q", KEY_Q},
    {"KEY_W", KEY_W},
    {"KEY_Y", KEY_Y},
    {"KEY_Z", KEY_Z},
    {"KEY_A", KEY_A},
    {"KEY_M", KEY_M},
    {"KEY_SEMICOLON", KEY_SEMICOLON},
    {"KEY_LEFTBRACE", KEY_LEFTBRACE},
    {"KEY_CAPSLOCK", KEY_CAPSLOCK},
    {"KEY_ESC", KEY_ESC},
    {"KEY_LEFTCTRL", KEY_LEFTCTRL},
};
#define N_PROBES (sizeof(PROBES) / sizeof(PROBES[0]))

static const char *const LAYOUTS[] = {"us", "de", "fr", "ru"};
#define N_LAYOUTS (sizeof(LAYOUTS) / sizeof(LAYOUTS[0]))

static int sym_for(struct xkb_context *ctx, const char *layout, uint16_t evdev_code, char *out,
                   size_t cap)
{
    struct xkb_rule_names names = {.rules = "evdev", .model = "pc105", .layout = layout};
    struct xkb_keymap *keymap;
    struct xkb_state *state;
    xkb_keysym_t sym;

    keymap = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap)
        return -1;
    state = xkb_state_new(keymap);
    if (!state) {
        xkb_keymap_unref(keymap);
        return -1;
    }
    sym = xkb_state_key_get_one_sym(state, evdev_code + XKB_KEYCODE_OFFSET);
    xkb_keysym_get_name(sym, out, cap);
    xkb_state_unref(state);
    xkb_keymap_unref(keymap);
    return 0;
}

int main(void)
{
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    char sym[64];

    if (!ctx) {
        fprintf(stderr, "xkb_context_new failed\n");
        return 1;
    }

    printf("The relay forwards keycodes. The session maps them to keysyms.\n");
    printf("The evdev/xkb keycode column is what the relay sees and re-emits;\n");
    printf("everything to its right is the session's keymap, applied equally to\n");
    printf("the hardware keyboard and to the relay's uinput device.\n\n");

    printf("%-15s %7s %6s", "relay keycode", "evdev", "xkb");
    for (size_t l = 0; l < N_LAYOUTS; l++)
        printf(" %12s", LAYOUTS[l]);
    printf("\n");
    printf("--------------- ------- ------");
    for (size_t l = 0; l < N_LAYOUTS; l++)
        printf(" ------------");
    printf("\n");

    for (size_t i = 0; i < N_PROBES; i++) {
        printf("%-15s %7u %6u", PROBES[i].label, PROBES[i].evdev_code,
               PROBES[i].evdev_code + XKB_KEYCODE_OFFSET);
        for (size_t l = 0; l < N_LAYOUTS; l++) {
            if (sym_for(ctx, LAYOUTS[l], PROBES[i].evdev_code, sym, sizeof(sym)) < 0)
                snprintf(sym, sizeof(sym), "<compile-failed>");
            printf(" %12s", sym);
        }
        printf("\n");
    }

    printf("\nWhat the relay's two rules emit, as keycodes:\n");
    printf("  Caps Lock tap  -> KEY_ESC      = evdev %u (xkb %u)\n", KEY_ESC,
           KEY_ESC + XKB_KEYCODE_OFFSET);
    printf("  Caps Lock hold -> KEY_LEFTCTRL = evdev %u (xkb %u)\n", KEY_LEFTCTRL,
           KEY_LEFTCTRL + XKB_KEYCODE_OFFSET);
    printf("Those two numbers do not change with the layout, which is why the\n");
    printf("rules engine never consults libxkbcommon at runtime.\n");

    xkb_context_unref(ctx);
    return 0;
}
