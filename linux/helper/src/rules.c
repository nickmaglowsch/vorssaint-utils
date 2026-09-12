/* The rules engine's core: defaults, the dispatcher that runs the seven rules
 * in order, the notice ring, the hand-written JSON reader for SetRules and
 * SetContext, and the state serialiser behind the Rules property.
 *
 * The individual rules live in rules_debounce.c, rules_scroll.c,
 * rules_super.c, rules_mouse.c and rules_quit.c, each next to the Swift file
 * it is a port of. */
#include "rules_internal.h"

#include <linux/input-event-codes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- small shared helpers -------------------------------------------------- */

int rules_emit(rules_event *out, int cap, int n, uint16_t type, uint16_t code, int32_t value)
{
    if (n < 0 || n >= cap)
        return -1;
    out[n].type = type;
    out[n].code = code;
    out[n].value = value;
    out[n].dev_id = 0;
    out[n].dev_class = 0;
    return n + 1;
}

/* Press in order, release in reverse: that is what a hand does to a keyboard
 * and what an application's xkb state expects to see. */
int rules_emit_mods(rules_event *out, int cap, int n, const uint16_t *mods, unsigned n_mods,
                    bool down)
{
    if (down) {
        for (unsigned i = 0; i < n_mods; i++) {
            n = rules_emit(out, cap, n, EV_KEY, mods[i], 1);
            if (n < 0)
                return -1;
        }
    } else {
        for (unsigned i = n_mods; i > 0; i--) {
            n = rules_emit(out, cap, n, EV_KEY, mods[i - 1], 0);
            if (n < 0)
                return -1;
        }
    }
    return n;
}

void rules_notice_push(rules_state *st, uint64_t ts, const char *fmt, ...)
{
    size_t next = (st->notice_tail + 1) % RULES_NOTICE_CAP;
    va_list ap;

    if (next == st->notice_head) {
        /* Full. Dropping is the only choice that does not stall the relay,
         * and the count is reported so a dropped notice is never silent. */
        st->stat_notices_dropped++;
        return;
    }
    st->notice[st->notice_tail].ts = ts;
    va_start(ap, fmt);
    vsnprintf(st->notice[st->notice_tail].detail, RULES_NOTICE_MAX, fmt, ap);
    va_end(ap);
    st->notice_tail = next;
}

bool rules_notice_pop(rules_state *st, rules_notice *out)
{
    if (st->notice_head == st->notice_tail)
        return false;
    *out = st->notice[st->notice_head];
    st->notice_head = (st->notice_head + 1) % RULES_NOTICE_CAP;
    return true;
}

/* --- defaults -------------------------------------------------------------- */

void rules_config_defaults(rules_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    /* Every rule ships off. A relay that starts rewriting input the moment it
     * is enabled is not something the capabilities page can honestly
     * describe, and the app turns on exactly what the user asked for. */
    cfg->kdb.global_window_ms = RULES_KDB_DEFAULT_MS;
    cfg->mcd.window_ms = RULES_MCD_DEFAULT_MS;

    cfg->scroll.vertical = true;
    cfg->scroll.mouse = true;

    cfg->smooth.step = RULES_SMOOTH_STEP_DEFAULT;
    cfg->smooth.response = RULES_SMOOTH_RESPONSE_DEFAULT;
    cfg->smooth.mouse = true;

    cfg->sk.source = KEY_CAPSLOCK;
    cfg->sk.mods[0] = KEY_LEFTCTRL;
    cfg->sk.mods[1] = KEY_LEFTALT;
    cfg->sk.mods[2] = KEY_LEFTMETA;
    cfg->sk.n_mods = 3;
    cfg->sk.tap_action = RULES_SK_ESCAPE;
    cfg->sk.hold_threshold_ns = MS_NS(500);
    cfg->sk.led = true;

    cfg->qp.quit.mode = RULES_QP_HOLD;
    cfg->qp.quit.hold_ms = RULES_QP_HOLD_DEFAULT_MS;
    cfg->qp.quit.double_ms = RULES_QP_DOUBLE_DEFAULT_MS;
    cfg->qp.quit.extra_mod = KEY_LEFTSHIFT;
    cfg->qp.quit.scope = RULES_QP_SCOPE_ALL;
    cfg->qp.close = cfg->qp.quit;
}

void rules_init(rules_state *st, const rules_config *cfg)
{
    memset(st, 0, sizeof(*st));
    if (cfg)
        st->cfg = *cfg;
    else
        rules_config_defaults(&st->cfg);
}

void rules_set_context(rules_state *st, const rules_context *ctx) { st->ctx = *ctx; }

/* Everything the engine is holding down on the user's behalf. Nothing is
 * marked released until its release has actually been queued: a full buffer
 * must leave the state eligible for the next attempt rather than record a
 * clear that did not happen (SuperKeyMappingGuard.mappingMarkerAfterClear). */
int rules_release_held(rules_state *st, rules_event *out, int cap, int n)
{
    if (st->sk.mods_down) {
        int m = rules_emit_mods(out, cap, n, st->cfg.sk.mods, st->cfg.sk.n_mods, false);
        if (m < 0)
            return -1;
        n = m;
        st->sk.mods_down = false;
    }
    for (unsigned i = 0; i < st->cfg.mbs.n_bind && i < RULES_MBS_MAX; i++) {
        if (!st->mbs_held[i])
            continue;
        {
            const rules_mbs_binding *b = &st->cfg.mbs.bind[i];
            int m = rules_emit(out, cap, n, EV_KEY, b->key, 0);
            if (m < 0)
                return -1;
            m = rules_emit_mods(out, cap, m, b->mods, b->n_mods, false);
            if (m < 0)
                return -1;
            n = m;
            st->mbs_held[i] = false;
        }
    }
    return n;
}

int rules_reconfigure(rules_state *st, const rules_config *cfg, rules_event *out, int out_cap)
{
    int n = rules_release_held(st, out, out_cap, 0);

    if (n < 0)
        return -1;
    st->cfg = *cfg;
    rules_sk_reset(&st->sk);
    rules_tilt_reset(&st->tilt);
    memset(&st->gesture, 0, sizeof(st->gesture));
    memset(&st->qp_quit, 0, sizeof(st->qp_quit));
    memset(&st->qp_close, 0, sizeof(st->qp_close));
    /* A glide in flight belongs to the old step and response; dropping it is
     * better than finishing it at a speed nobody asked for. */
    memset(&st->smooth_v, 0, sizeof(st->smooth_v));
    memset(&st->smooth_h, 0, sizeof(st->smooth_h));
    st->smooth_have_frame = false;
    st->stat_out += (uint64_t)n;
    return n;
}

/* --- the dispatcher --------------------------------------------------------- */

/* Rule order, and why it is this order:
 *
 *  1. keyboard_debounce and 2. mouse_click_debounce first, because they model
 *     a hardware defect: an event they reject was never really produced, so
 *     no later rule should ever have seen it.
 *  3. quit_protection before super_key, because it owns a complete chord
 *     (Ctrl+Q) and must not have Ctrl+Q re-synthesised underneath it.
 *  4. super_key before mouse_button_shortcut, so a chord fired by a mouse
 *     button while the super key is held carries the super key's modifiers,
 *     which is what the macOS service does by stamping mouse presses at the
 *     HID stage.
 *  5. scroll_invert before smooth_scroll, so the glide is fed the direction
 *     the user asked for and the two cannot cancel each other out. */
int rules_process(rules_state *st, const rules_event *in, uint64_t now_ns, rules_event *out,
                  int out_cap)
{
    rules_event ev = *in;
    bool handled = false;
    int n = 0;

    if (in->type == EV_SYN)
        return 0; /* the output device re-syncs after every batch */

    st->stat_in++;

    /* Physical modifier state is tracked from the source stream, before any
     * rule runs, because quit_protection and super_key both ask what is held
     * and neither may be told about a modifier a rule swallowed. */
    if (ev.type == EV_KEY && rules_is_modifier(ev.code)) {
        if (ev.value == 1 && !st->mod_down[ev.code]) {
            st->mod_down[ev.code] = true;
            st->mod_held_count++;
        } else if (ev.value == 0 && st->mod_down[ev.code]) {
            st->mod_down[ev.code] = false;
            if (st->mod_held_count)
                st->mod_held_count--;
        }
    }

    n = rules_kdb_apply(st, &ev, now_ns, out, out_cap, n, &handled);
    if (n < 0) return -1;
    if (handled) goto done;

    n = rules_mcd_apply(st, &ev, now_ns, out, out_cap, n, &handled);
    if (n < 0) return -1;
    if (handled) goto done;

    n = rules_qp_apply(st, &ev, now_ns, out, out_cap, n, &handled);
    if (n < 0) return -1;
    if (handled) goto done;

    n = rules_sk_apply(st, &ev, now_ns, out, out_cap, n, &handled);
    if (n < 0) return -1;
    if (handled) goto done;

    n = rules_mbs_apply(st, &ev, now_ns, out, out_cap, n, &handled);
    if (n < 0) return -1;
    if (handled) goto done;

    n = rules_scroll_apply(st, &ev, now_ns, out, out_cap, n, &handled);
    if (n < 0) return -1;
    if (handled) goto done;

    n = rules_smooth_apply(st, &ev, now_ns, out, out_cap, n, &handled);
    if (n < 0) return -1;
    if (handled) goto done;

    n = rules_emit(out, out_cap, n, ev.type, ev.code, ev.value);
    if (n < 0)
        return -1;

done:
    if (n > 0)
        st->stat_out += (uint64_t)n;
    return n;
}

uint64_t rules_deadline_ns(const rules_state *st)
{
    uint64_t a = rules_qp_deadline(st);
    uint64_t b = rules_smooth_deadline(st);

    if (a == 0)
        return b;
    if (b == 0)
        return a;
    return a < b ? a : b;
}

int rules_timer(rules_state *st, uint64_t now_ns, rules_event *out, int out_cap)
{
    int n = rules_qp_tick(st, now_ns, out, out_cap, 0);

    if (n < 0)
        return -1;
    n = rules_smooth_tick(st, now_ns, out, out_cap, n);
    if (n < 0)
        return -1;
    if (n > 0)
        st->stat_out += (uint64_t)n;
    return n;
}

/* --- the JSON reader --------------------------------------------------------
 * The helper runs as root and parses a string handed to it over D-Bus, so it
 * does not link a JSON library: this reads exactly the keys of the rule
 * schema and rejects anything it cannot account for. It is not a general
 * parser and is not meant to become one. */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

static const char *json_find(const char *json, const char *key)
{
    size_t klen = strlen(key);
    const char *p = json;

    while ((p = strchr(p, '"')) != NULL) {
        if (strncmp(p + 1, key, klen) == 0 && p[1 + klen] == '"') {
            p = skip_ws(p + 1 + klen + 1);
            if (*p != ':')
                return NULL;
            return skip_ws(p + 1);
        }
        p++;
    }
    return NULL;
}

/* Copy the balanced {...} or [...] that `key` names into buf. Returns 1 when
 * the key was present and copied, 0 when absent, -1 when malformed or longer
 * than buf. Strings are skipped over so a brace inside one cannot unbalance
 * the scan. */
static int json_block(const char *json, const char *key, char open, char close, char *buf,
                      size_t cap)
{
    const char *v = json_find(json, key);
    const char *p;
    int depth = 0;
    bool in_string = false;
    size_t used = 0;

    if (!v)
        return 0;
    if (*v != open)
        return -1;
    for (p = v; *p; p++) {
        if (in_string) {
            if (*p == '\\' && p[1])
                p++;
            else if (*p == '"')
                in_string = false;
        } else if (*p == '"') {
            in_string = true;
        } else if (*p == open) {
            depth++;
        } else if (*p == close) {
            depth--;
        }
        if (used + 1 >= cap)
            return -1;
        buf[used++] = *p;
        if (depth == 0 && !in_string) {
            buf[used] = '\0';
            return 1;
        }
    }
    return -1;
}

static int json_bool(const char *json, const char *key, bool *out)
{
    const char *v = json_find(json, key);

    if (!v)
        return 0;
    if (strncmp(v, "true", 4) == 0) { *out = true; return 1; }
    if (strncmp(v, "false", 5) == 0) { *out = false; return 1; }
    return -1;
}

static int json_uint(const char *json, const char *key, unsigned long *out, unsigned long max)
{
    const char *v = json_find(json, key);
    char *end;
    unsigned long n;

    if (!v)
        return 0;
    if (*v < '0' || *v > '9')
        return -1;
    n = strtoul(v, &end, 10);
    if (end == v || n > max)
        return -1;
    *out = n;
    return 1;
}

/* An identifier the helper will later echo back inside a JSON string it
 * builds itself: anything outside this set is refused rather than escaped,
 * because a quote or a backslash arriving from an unprivileged caller and
 * coming back out inside the Rules property would be the helper generating
 * malformed JSON for every client that reads it. */
static bool safe_identifier(const char *s)
{
    if (!*s)
        return true;
    for (const char *p = s; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))
            continue;
        /* Enough for a reverse-DNS app id, a Flatpak id, an AppImage name and
         * the "code:ms,code:ms" per-key window string, and nothing that could
         * end a JSON string or start an escape. */
        if (*p == '.' || *p == '-' || *p == '_' || *p == '+' || *p == ':' || *p == ',')
            continue;
        return false;
    }
    return true;
}

/* Copy the string value of `key`. 1 present, 0 absent, -1 malformed. */
static int json_str(const char *json, const char *key, char *buf, size_t cap)
{
    const char *v = json_find(json, key);
    size_t used = 0;

    if (!v)
        return 0;
    if (*v != '"')
        return -1;
    for (const char *p = v + 1; *p; p++) {
        if (*p == '"') {
            buf[used] = '\0';
            return safe_identifier(buf) ? 1 : -1;
        }
        if (*p == '\\' || (unsigned char)*p < 0x20)
            return -1; /* no escapes in this schema; see safe_identifier */
        if (used + 1 >= cap)
            return -1;
        buf[used++] = *p;
    }
    return -1;
}

/* [1, 2, 3] into a bounded array of keycodes. */
static int json_uint_array(const char *block, unsigned long *out, unsigned *n_out, unsigned cap,
                           unsigned long max)
{
    const char *p = block;
    unsigned n = 0;

    if (*p != '[')
        return -1;
    p = skip_ws(p + 1);
    while (*p && *p != ']') {
        char *end;
        unsigned long v;
        if (*p < '0' || *p > '9')
            return -1;
        v = strtoul(p, &end, 10);
        if (end == p || v > max || n >= cap)
            return -1;
        out[n++] = v;
        p = skip_ws(end);
        if (*p == ',')
            p = skip_ws(p + 1);
        else if (*p != ']')
            return -1;
    }
    if (*p != ']')
        return -1;
    *n_out = n;
    return 1;
}

/* ["a", "b"] into a bounded array of identifiers. */
static int json_str_array(const char *block, char out[][RULES_APPID_MAX], unsigned *n_out,
                          unsigned cap)
{
    const char *p = block;
    unsigned n = 0;

    if (*p != '[')
        return -1;
    p = skip_ws(p + 1);
    while (*p && *p != ']') {
        size_t used = 0;
        if (*p != '"' || n >= cap)
            return -1;
        p++;
        while (*p && *p != '"') {
            if (*p == '\\' || (unsigned char)*p < 0x20 || used + 1 >= RULES_APPID_MAX)
                return -1;
            out[n][used++] = *p++;
        }
        if (*p != '"')
            return -1;
        out[n][used] = '\0';
        if (!safe_identifier(out[n]))
            return -1;
        n++;
        p = skip_ws(p + 1);
        if (*p == ',')
            p = skip_ws(p + 1);
        else if (*p != ']')
            return -1;
    }
    if (*p != ']')
        return -1;
    *n_out = n;
    return 1;
}

/* Walk the objects of an array, handing each one to fn. */
static int json_object_array(const char *block, int (*fn)(const char *obj, void *ctx), void *ctx)
{
    const char *p = block;
    char obj[512];

    if (*p != '[')
        return -1;
    p = skip_ws(p + 1);
    while (*p && *p != ']') {
        int depth = 0;
        bool in_string = false;
        size_t used = 0;
        if (*p != '{')
            return -1;
        for (; *p; p++) {
            if (in_string) {
                if (*p == '\\' && p[1]) p++;
                else if (*p == '"') in_string = false;
            } else if (*p == '"') {
                in_string = true;
            } else if (*p == '{') {
                depth++;
            } else if (*p == '}') {
                depth--;
            }
            if (used + 1 >= sizeof(obj))
                return -1;
            obj[used++] = *p;
            if (depth == 0 && !in_string)
                break;
        }
        if (*p != '}')
            return -1;
        obj[used] = '\0';
        if (fn(obj, ctx) < 0)
            return -1;
        p = skip_ws(p + 1);
        if (*p == ',')
            p = skip_ws(p + 1);
        else if (*p != ']')
            return -1;
    }
    return *p == ']' ? 1 : -1;
}

/* One lookup per key, and the result says whether the key was present at all.
 * Doing it in two calls left `n` only conditionally initialised along a path
 * the compiler could not follow (-Wmaybe-uninitialized at -O3) and doubled
 * the scanning over a document that may be 64 KiB. */
#define TAKE_UINT(doc, key, max, assign)                                                           \
    do {                                                                                           \
        if (bad)                                                                                   \
            break;                                                                                 \
        int have_ = json_uint((doc), (key), &n, (max));                                            \
        if (have_ < 0)                                                                             \
            bad = (key);                                                                           \
        else if (have_ > 0)                                                                        \
            assign;                                                                                \
    } while (0)

#define TAKE_BOOL(doc, key, field)                                                                 \
    do {                                                                                           \
        if (!bad && json_bool((doc), (key), &(field)) < 0)                                          \
            bad = (key);                                                                           \
    } while (0)

static int parse_mode(const char *s, rules_qp_mode *out)
{
    if (!strcmp(s, "hold")) { *out = RULES_QP_HOLD; return 0; }
    if (!strcmp(s, "double_press")) { *out = RULES_QP_DOUBLE; return 0; }
    if (!strcmp(s, "extra_modifier")) { *out = RULES_QP_EXTRA; return 0; }
    return -1;
}

static int parse_scope(const char *s, rules_qp_scope *out)
{
    if (!strcmp(s, "all")) { *out = RULES_QP_SCOPE_ALL; return 0; }
    if (!strcmp(s, "selected_only")) { *out = RULES_QP_SCOPE_SELECTED; return 0; }
    if (!strcmp(s, "all_except_selected")) { *out = RULES_QP_SCOPE_EXCEPT; return 0; }
    return -1;
}

static int parse_extra_mod(const char *s, uint16_t *out)
{
    if (!strcmp(s, "shift")) { *out = KEY_LEFTSHIFT; return 0; }
    if (!strcmp(s, "alt")) { *out = KEY_LEFTALT; return 0; }
    return -1;
}

static int parse_tap_action(const char *s, rules_sk_action *out)
{
    if (!strcmp(s, "none")) { *out = RULES_SK_NONE; return 0; }
    if (!strcmp(s, "escape")) { *out = RULES_SK_ESCAPE; return 0; }
    if (!strcmp(s, "caps_lock")) { *out = RULES_SK_CAPSLOCK; return 0; }
    if (!strcmp(s, "key")) { *out = RULES_SK_KEY; return 0; }
    return -1;
}

struct binding_ctx {
    rules_mbs_config *cfg;
    const char *bad;
};

static int take_binding(const char *obj, void *vctx)
{
    struct binding_ctx *bc = vctx;
    rules_mbs_binding b;
    unsigned long mods[RULES_MAX_MODS];
    unsigned n_mods = 0;
    unsigned long v = 0;
    char block[128];
    const char *sign;

    memset(&b, 0, sizeof(b));
    if (bc->cfg->n_bind >= RULES_MBS_MAX) {
        bc->bad = "mouse_button_shortcut.bindings (too many)";
        return -1;
    }

    /* The input is either a BTN_* code or one of the two negative tilt
     * pseudo-inputs, so it is read as a signed number. */
    sign = json_find(obj, "input");
    if (!sign) {
        bc->bad = "mouse_button_shortcut.bindings.input";
        return -1;
    }
    {
        char *end;
        long raw = strtol(sign, &end, 10);
        if (end == sign || raw < RULES_TILT_LEFT || raw > RULES_KEY_MAX) {
            bc->bad = "mouse_button_shortcut.bindings.input";
            return -1;
        }
        b.input = (int32_t)raw;
    }
    if (!rules_mbs_can_map(b.input)) {
        bc->bad = "mouse_button_shortcut.bindings.input (not a mappable button)";
        return -1;
    }
    if (json_uint(obj, "key", &v, RULES_KEY_MAX) != 1 || v == 0) {
        bc->bad = "mouse_button_shortcut.bindings.key";
        return -1;
    }
    b.key = (uint16_t)v;
    if (json_block(obj, "modifiers", '[', ']', block, sizeof(block)) > 0) {
        if (json_uint_array(block, mods, &n_mods, RULES_MAX_MODS, RULES_KEY_MAX) < 0) {
            bc->bad = "mouse_button_shortcut.bindings.modifiers";
            return -1;
        }
        for (unsigned i = 0; i < n_mods; i++)
            b.mods[i] = (uint16_t)mods[i];
        b.n_mods = n_mods;
    }
    bc->cfg->bind[bc->cfg->n_bind++] = b;
    return 0;
}

static int parse_qp_shortcut(const char *doc, rules_qp_shortcut *sc)
{
    unsigned long n = 0;
    const char *bad = NULL;
    char word[32];
    char block[RULES_QP_EXC_MAX * RULES_APPID_MAX + 64];
    int r;

    TAKE_BOOL(doc, "enabled", sc->enabled);
    TAKE_UINT(doc, "hold_ms", 100000, sc->hold_ms = rules_qp_sanitize_hold((double)n));
    TAKE_UINT(doc, "double_ms", 100000, sc->double_ms = rules_qp_sanitize_double((double)n));

    r = json_str(doc, "mode", word, sizeof(word));
    if (!bad && (r < 0 || (r > 0 && parse_mode(word, &sc->mode) < 0)))
        bad = "mode";
    r = json_str(doc, "scope", word, sizeof(word));
    if (!bad && (r < 0 || (r > 0 && parse_scope(word, &sc->scope) < 0)))
        bad = "scope";
    r = json_str(doc, "extra_modifier", word, sizeof(word));
    if (!bad && (r < 0 || (r > 0 && parse_extra_mod(word, &sc->extra_mod) < 0)))
        bad = "extra_modifier";

    if (!bad) {
        r = json_block(doc, "exceptions", '[', ']', block, sizeof(block));
        if (r < 0 ||
            (r > 0 && json_str_array(block, sc->exception, &sc->n_exceptions, RULES_QP_EXC_MAX) < 0))
            bad = "exceptions";
    }
    return bad ? -1 : 0;
}

int rules_config_from_json(const char *json, rules_config *cfg, char *err, size_t err_cap)
{
    unsigned long n = 0;
    const char *bad = NULL;
    /* One scratch buffer, reused section by section. Sized for the largest
     * section: quit_protection with two shortcuts and their exception lists. */
    char sec[2 * (RULES_QP_EXC_MAX * RULES_APPID_MAX + 256) + 256];
    char sub[RULES_QP_EXC_MAX * RULES_APPID_MAX + 256];
    char word[32];
    int r;

    if (!json) {
        snprintf(err, err_cap, "not a JSON object");
        return -1;
    }
    /* Length before content: the reader walks the string once per key, so the
     * bound has to be applied before any of that work is done. */
    if (strnlen(json, RULES_JSON_MAX + 1) > RULES_JSON_MAX) {
        snprintf(err, err_cap, "document is longer than the %d byte limit", RULES_JSON_MAX);
        return -1;
    }
    if (strchr(json, '{') == NULL) {
        snprintf(err, err_cap, "not a JSON object");
        return -1;
    }
    rules_config_defaults(cfg);

    /* keyboard_debounce */
    r = json_block(json, "keyboard_debounce", '{', '}', sec, sizeof(sec));
    if (r < 0)
        bad = "keyboard_debounce";
    else if (r > 0) {
        TAKE_BOOL(sec, "enabled", cfg->kdb.enabled);
        TAKE_UINT(sec, "window_ms", 100000,
                  cfg->kdb.global_window_ms = rules_kdb_sanitize_window((long)n));
        if (!bad) {
            int have = json_str(sec, "key_windows", sub, sizeof(sub));
            if (have < 0 || (have > 0 && rules_kdb_decode_key_windows(sub, &cfg->kdb) < 0))
                bad = "keyboard_debounce.key_windows";
        }
    }

    /* mouse_click_debounce */
    if (!bad) {
        r = json_block(json, "mouse_click_debounce", '{', '}', sec, sizeof(sec));
        if (r < 0)
            bad = "mouse_click_debounce";
        else if (r > 0) {
            TAKE_BOOL(sec, "enabled", cfg->mcd.enabled);
            TAKE_UINT(sec, "window_ms", 100000,
                      cfg->mcd.window_ms = rules_mcd_sanitize_window((long)n));
        }
    }

    /* scroll_invert */
    if (!bad) {
        r = json_block(json, "scroll_invert", '{', '}', sec, sizeof(sec));
        if (r < 0)
            bad = "scroll_invert";
        else if (r > 0) {
            TAKE_BOOL(sec, "enabled", cfg->scroll.enabled);
            TAKE_BOOL(sec, "vertical", cfg->scroll.vertical);
            TAKE_BOOL(sec, "horizontal", cfg->scroll.horizontal);
            TAKE_BOOL(sec, "mouse", cfg->scroll.mouse);
            TAKE_BOOL(sec, "touchpad", cfg->scroll.touchpad);
            TAKE_BOOL(sec, "shift_redirects_vertical", cfg->scroll.shift_redirects_vertical);
        }
    }

    /* smooth_scroll */
    if (!bad) {
        r = json_block(json, "smooth_scroll", '{', '}', sec, sizeof(sec));
        if (r < 0)
            bad = "smooth_scroll";
        else if (r > 0) {
            TAKE_BOOL(sec, "enabled", cfg->smooth.enabled);
            TAKE_UINT(sec, "step", 100000,
                      cfg->smooth.step = (unsigned)rules_smooth_sanitize_step((int)n));
            TAKE_UINT(sec, "response", 100000,
                      cfg->smooth.response = (unsigned)rules_smooth_sanitize_response((int)n));
            TAKE_BOOL(sec, "mouse", cfg->smooth.mouse);
            TAKE_BOOL(sec, "touchpad", cfg->smooth.touchpad);
        }
    }

    /* super_key */
    if (!bad) {
        r = json_block(json, "super_key", '{', '}', sec, sizeof(sec));
        if (r < 0)
            bad = "super_key";
        else if (r > 0) {
            unsigned long mods[RULES_MAX_MODS];
            unsigned n_mods = 0;

            TAKE_BOOL(sec, "enabled", cfg->sk.enabled);
            TAKE_BOOL(sec, "led", cfg->sk.led);
            TAKE_UINT(sec, "source", RULES_KEY_MAX, cfg->sk.source = (uint16_t)n);
            TAKE_UINT(sec, "tap_key", RULES_KEY_MAX, cfg->sk.tap_key = (uint16_t)n);
            TAKE_UINT(sec, "hold_threshold_ms", 5000, cfg->sk.hold_threshold_ns = MS_NS(n));
            if (!bad) {
                int have = json_str(sec, "tap_action", word, sizeof(word));
                if (have < 0 || (have > 0 && parse_tap_action(word, &cfg->sk.tap_action) < 0))
                    bad = "super_key.tap_action";
            }
            if (!bad) {
                int have = json_block(sec, "modifiers", '[', ']', sub, sizeof(sub));
                if (have < 0 ||
                    (have > 0 &&
                     json_uint_array(sub, mods, &n_mods, RULES_MAX_MODS, RULES_KEY_MAX) < 0)) {
                    bad = "super_key.modifiers";
                } else if (have > 0) {
                    for (unsigned i = 0; i < n_mods; i++)
                        cfg->sk.mods[i] = (uint16_t)mods[i];
                    cfg->sk.n_mods = n_mods;
                }
            }
        }
    }

    /* mouse_button_shortcut */
    if (!bad) {
        r = json_block(json, "mouse_button_shortcut", '{', '}', sec, sizeof(sec));
        if (r < 0)
            bad = "mouse_button_shortcut";
        else if (r > 0) {
            struct binding_ctx bc = {&cfg->mbs, NULL};

            TAKE_BOOL(sec, "enabled", cfg->mbs.enabled);
            TAKE_BOOL(sec, "gesture_follows_drag", cfg->mbs.gesture_follows_drag);
            TAKE_UINT(sec, "gesture_button", RULES_KEY_MAX, cfg->mbs.gesture_button = (uint16_t)n);
            if (!bad && cfg->mbs.gesture_button != 0 &&
                !rules_gesture_can_bind((int32_t)cfg->mbs.gesture_button))
                bad = "mouse_button_shortcut.gesture_button";
            if (!bad) {
                int have = json_block(sec, "bindings", '[', ']', sub, sizeof(sub));
                if (have < 0 || (have > 0 && json_object_array(sub, take_binding, &bc) < 0))
                    bad = bc.bad ? bc.bad : "mouse_button_shortcut.bindings";
            }
        }
    }

    /* quit_protection */
    if (!bad) {
        r = json_block(json, "quit_protection", '{', '}', sec, sizeof(sec));
        if (r < 0)
            bad = "quit_protection";
        else if (r > 0) {
            TAKE_BOOL(sec, "enabled", cfg->qp.enabled);
            if (!bad && json_block(sec, "quit", '{', '}', sub, sizeof(sub)) > 0 &&
                parse_qp_shortcut(sub, &cfg->qp.quit) < 0)
                bad = "quit_protection.quit";
            if (!bad && json_block(sec, "close", '{', '}', sub, sizeof(sub)) > 0 &&
                parse_qp_shortcut(sub, &cfg->qp.close) < 0)
                bad = "quit_protection.close";
        }
    }

    /* Cross-rule validation, last, because it needs the whole document:
     * SuperKeyMappingGuard's refusal to claim a key another feature owns. */
    if (!bad && cfg->sk.enabled && rules_sk_source_conflict(cfg, cfg->sk.source))
        bad = "super_key.source (already claimed by another rule)";

    if (bad) {
        snprintf(err, err_cap, "bad value for \"%s\"", bad);
        return -1;
    }
    return 0;
}

int rules_context_from_json(const char *json, rules_context *ctx, char *err, size_t err_cap)
{
    int r;

    memset(ctx, 0, sizeof(*ctx));
    if (!json) {
        snprintf(err, err_cap, "not a JSON object");
        return -1;
    }
    if (strnlen(json, RULES_JSON_MAX + 1) > RULES_JSON_MAX) {
        snprintf(err, err_cap, "document is longer than the %d byte limit", RULES_JSON_MAX);
        return -1;
    }
    if (strchr(json, '{') == NULL) {
        snprintf(err, err_cap, "not a JSON object");
        return -1;
    }
    r = json_str(json, "focused_app_id", ctx->focused_app_id, sizeof(ctx->focused_app_id));
    if (r < 0) {
        snprintf(err, err_cap,
                 "bad value for \"focused_app_id\" (letters, digits and . - _ + : only)");
        return -1;
    }
    return 0;
}

/* --- state serialiser -------------------------------------------------------- */

static const char *qp_mode_name(rules_qp_mode m)
{
    switch (m) {
    case RULES_QP_HOLD: return "hold";
    case RULES_QP_DOUBLE: return "double_press";
    case RULES_QP_EXTRA: return "extra_modifier";
    }
    return "hold";
}

static const char *sk_action_name(rules_sk_action a)
{
    switch (a) {
    case RULES_SK_NONE: return "none";
    case RULES_SK_ESCAPE: return "escape";
    case RULES_SK_CAPSLOCK: return "caps_lock";
    case RULES_SK_KEY: return "key";
    }
    return "none";
}

int rules_state_to_json(const rules_state *st, char *buf, size_t cap)
{
    char key_windows[RULES_KDB_KEYS * 10 + 2] = "";

    rules_kdb_encode_key_windows(&st->cfg.kdb, key_windows, sizeof(key_windows));
    return snprintf(
        buf, cap,
        "{\"keyboard_debounce\":{\"enabled\":%s,\"window_ms\":%u,\"key_windows\":\"%s\"},"
        "\"mouse_click_debounce\":{\"enabled\":%s,\"window_ms\":%u},"
        "\"scroll_invert\":{\"enabled\":%s,\"vertical\":%s,\"horizontal\":%s,\"mouse\":%s,"
        "\"touchpad\":%s},"
        "\"smooth_scroll\":{\"enabled\":%s,\"step\":%u,\"response\":%u},"
        "\"super_key\":{\"enabled\":%s,\"source\":%u,\"tap_action\":\"%s\",\"state\":\"%s\"},"
        "\"mouse_button_shortcut\":{\"enabled\":%s,\"bindings\":%u,\"gesture_button\":%u},"
        "\"quit_protection\":{\"enabled\":%s,\"quit_mode\":\"%s\",\"close_mode\":\"%s\"},"
        "\"context\":{\"focused_app_id\":\"%s\"},"
        "\"counters\":{\"in\":%llu,\"out\":%llu,\"keyboard_debounce_dropped\":%llu,"
        "\"click_debounce_dropped\":%llu,\"taps\":%llu,\"holds\":%llu,\"smooth_frames\":%llu,"
        "\"quit_blocked\":%llu,\"notices_dropped\":%llu}}",
        st->cfg.kdb.enabled ? "true" : "false", st->cfg.kdb.global_window_ms, key_windows,
        st->cfg.mcd.enabled ? "true" : "false", st->cfg.mcd.window_ms,
        st->cfg.scroll.enabled ? "true" : "false", st->cfg.scroll.vertical ? "true" : "false",
        st->cfg.scroll.horizontal ? "true" : "false", st->cfg.scroll.mouse ? "true" : "false",
        st->cfg.scroll.touchpad ? "true" : "false", st->cfg.smooth.enabled ? "true" : "false",
        st->cfg.smooth.step, st->cfg.smooth.response, st->cfg.sk.enabled ? "true" : "false",
        st->cfg.sk.source, sk_action_name(st->cfg.sk.tap_action), rules_sk_state_name(&st->sk),
        st->cfg.mbs.enabled ? "true" : "false", st->cfg.mbs.n_bind, st->cfg.mbs.gesture_button,
        st->cfg.qp.enabled ? "true" : "false", qp_mode_name(st->cfg.qp.quit.mode),
        qp_mode_name(st->cfg.qp.close.mode), st->ctx.focused_app_id,
        (unsigned long long)st->stat_in, (unsigned long long)st->stat_out,
        (unsigned long long)st->stat_kdb_dropped, (unsigned long long)st->stat_mcd_dropped,
        (unsigned long long)st->stat_taps, (unsigned long long)st->stat_holds,
        (unsigned long long)st->stat_smooth_frames, (unsigned long long)st->stat_qp_blocked,
        (unsigned long long)st->stat_notices_dropped);
}
