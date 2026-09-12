/* Rules engine for the input relay: pure logic, no syscalls, no device access.
 *
 * The engine is deliberately free of I/O so the two device backends (real
 * evdev/uinput and the in-process fake) drive identical code, and so the
 * unit tests in tests/test_rules.c exercise the rules without a kernel.
 *
 * Every rule here is a port of a macOS `…Support` type from
 * Sources/Vorssaint(Core). Those Swift files are the specification of the
 * timing and the state machines; this C must reach the same decisions, and
 * tests/test_rules.c proves it by replaying their test vectors from
 * Tests/MetricsTests.swift. docs/linux-port/RELAY_RULES.md maps each rule to
 * the file it mirrors and counts the vectors ported. */
#ifndef VORSSAINT_RULES_H
#define VORSSAINT_RULES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef RULES_KEY_MAX
#define RULES_KEY_MAX 767 /* KEY_MAX from linux/input-event-codes.h */
#endif

/* How a source device was classified by udev, so a rule can apply to mice and
 * not to touchpads. Scroll direction is the case that needs it: someone who
 * inverts the wheel almost never wants the touchpad inverted too. */
typedef enum {
    RULES_DEV_UNKNOWN = 0,
    RULES_DEV_KEYBOARD = 1,
    RULES_DEV_MOUSE = 2,
    RULES_DEV_TOUCHPAD = 3
} rules_dev_class;

/* An event stripped to what the rules care about. Mirrors struct input_event
 * without the timeval: timestamps are carried separately in nanoseconds so the
 * fake backend can drive synthetic clocks. dev_id and dev_class describe the
 * source and are zero on everything the engine emits. */
typedef struct {
    uint16_t type;
    uint16_t code;
    int32_t value;
    uint16_t dev_id;
    uint8_t dev_class;
} rules_event;

/* A chord is at most four modifiers plus one key. */
#define RULES_MAX_MODS 4

/* --- rule 1: keyboard_debounce ------------------------------------------- */
/* Mirrors Sources/Vorssaint/Services/KeyboardDebounce/KeyboardDebounceSupport.swift */

#define RULES_KDB_DEFAULT_MS 5
#define RULES_KDB_MAX_MS 500
#define RULES_KDB_KEYS 32 /* per-key overrides the settings page can produce */
/* KeyboardDebounceState.staleStateGapNanoseconds */
#define RULES_KDB_STALE_NS 5000000000ULL

typedef struct {
    bool enabled;
    unsigned global_window_ms;
    uint16_t key_code[RULES_KDB_KEYS];
    unsigned key_window_ms[RULES_KDB_KEYS];
    unsigned n_key_windows;
} rules_kdb_config;

/* --- rule 2: mouse_click_debounce ---------------------------------------- */
/* Mirrors Sources/Vorssaint/Services/MouseClickDebounce/MouseClickDebounceSupport.swift */

#define RULES_MCD_DEFAULT_MS 25
#define RULES_MCD_MIN_MS 5
#define RULES_MCD_MAX_MS 100

typedef struct {
    bool enabled;
    unsigned window_ms;
} rules_mcd_config;

/* The three kinds MouseClickDebounceEvent has. DRAG has no evdev counterpart
 * -- pointer motion on Linux is its own event stream and is not owned by a
 * button -- so the relay never produces one; the decision function still
 * implements it so the Swift vectors port unchanged. */
typedef enum { RULES_CLICK_DOWN = 0, RULES_CLICK_DRAG = 1, RULES_CLICK_UP = 2 } rules_click_kind;

/* --- rule 3: scroll_invert ----------------------------------------------- */
/* Mirrors Sources/VorssaintCore/Services/ScrollWheelSupport.swift */

typedef struct {
    bool enabled;
    bool vertical;   /* invert REL_WHEEL / REL_WHEEL_HI_RES */
    bool horizontal; /* invert REL_HWHEEL / REL_HWHEEL_HI_RES */
    bool mouse;      /* apply on devices udev called a mouse */
    bool touchpad;   /* apply on devices udev called a touchpad */
    /* On macOS the window server turns a Shift-held vertical tick sideways
     * above the tap, so that tick has to follow the horizontal setting. On
     * Linux the redirect is done by the application, below the relay, so this
     * is off by default and exists only for a desktop that does it in the
     * input stack. */
    bool shift_redirects_vertical;
} rules_scroll_config;

typedef struct {
    bool vertical;
    bool horizontal;
} rules_inversion_plan;

/* --- rule 4: smooth_scroll ----------------------------------------------- */
/* Mirrors Sources/VorssaintCore/Services/SmoothScrollSupport.swift */

#define RULES_SMOOTH_STEP_MIN 20
#define RULES_SMOOTH_STEP_MAX 100
#define RULES_SMOOTH_STEP_DEFAULT 40
#define RULES_SMOOTH_RESPONSE_MIN 0
#define RULES_SMOOTH_RESPONSE_MAX 100
#define RULES_SMOOTH_RESPONSE_DEFAULT 65
/* One wheel notch in REL_*_HI_RES units, fixed by the kernel's hi-res wheel
 * convention. The macOS step is pixels per tick; the same ratio scales this,
 * so the default step travels exactly one notch and the setting still makes
 * the wheel faster or slower. */
#define RULES_HI_RES_PER_NOTCH 120

typedef struct {
    bool enabled;
    unsigned step;     /* 20..100, default 40 */
    unsigned response; /* 0..100, default 65 */
    bool mouse;
    bool touchpad;
} rules_smooth_config;

/* --- rule 5: super_key --------------------------------------------------- */
/* Mirrors Sources/Vorssaint/Services/SuperKey/SuperKeySupport.swift and
 * SuperKeyMappingGuard.swift */

typedef enum {
    RULES_SK_NONE = 0,
    RULES_SK_ESCAPE = 1,
    RULES_SK_CAPSLOCK = 2,
    /* "emit configured key": the app binds it to layout switching, which the
     * helper must not do itself -- that is a desktop setting, not a device. */
    RULES_SK_KEY = 3
} rules_sk_action;

typedef struct {
    bool enabled;
    uint16_t source; /* KEY_CAPSLOCK or a right-side modifier */
    uint16_t mods[RULES_MAX_MODS];
    unsigned n_mods;
    rules_sk_action tap_action;
    uint16_t tap_key;           /* used by RULES_SK_KEY */
    uint64_t hold_threshold_ns; /* SuperKeySupport.State, 500 ms */
    bool led;                   /* drive LED_CAPSL through EV_LED */
} rules_sk_config;

/* SuperKeySupport.Decision, for the ported vectors. */
typedef enum {
    RULES_SK_SWALLOW = 0,
    RULES_SK_ADD_MODIFIERS = 1,
    RULES_SK_PASS = 2,
    RULES_SK_SOLO_TAP = 3,
    RULES_SK_SOLO_HOLD = 4
} rules_sk_decision;

/* SuperKeySupport.Event, minus .sourceKey: on Linux the relay *is* the remap,
 * so a raw unmapped source key cannot reach the engine. */
typedef enum {
    RULES_SKE_TRIGGER_DOWN = 0,
    RULES_SKE_TRIGGER_UP = 1,
    RULES_SKE_OTHER_KEY = 2,
    RULES_SKE_OTHER_MODIFIER = 3
} rules_sk_event_kind;

/* --- rule 6: mouse_button_shortcut --------------------------------------- */
/* Mirrors Sources/Vorssaint/Services/MouseButtons/MouseButtonShortcutSupport.swift
 * and MouseSpacesGestureSupport.swift */

#define RULES_MBS_MAX 16
/* MouseButtonShortcutSupport's two negative side-wheel inputs, kept negative
 * for the same reason: they cannot collide with a real button code. */
#define RULES_TILT_LEFT (-2)
#define RULES_TILT_RIGHT (-1)
#define RULES_TILT_QUIET_NS 250000000ULL

/* MouseSpacesGestureSupport calibration, in the same units. */
#define RULES_GESTURE_SPACE_STEP 220.0
#define RULES_GESTURE_OVERVIEW_STEP 150.0
#define RULES_GESTURE_COOLDOWN_S 0.35

typedef struct {
    int32_t input; /* a BTN_* code, or RULES_TILT_LEFT / RULES_TILT_RIGHT */
    uint16_t mods[RULES_MAX_MODS];
    unsigned n_mods;
    uint16_t key;
} rules_mbs_binding;

typedef struct {
    bool enabled;
    rules_mbs_binding bind[RULES_MBS_MAX];
    unsigned n_bind;
    uint16_t gesture_button;   /* 0 = the hold-and-drag gesture is off */
    bool gesture_follows_drag; /* MouseSpacesGestureSupport.resolved */
} rules_mbs_config;

typedef enum {
    RULES_GESTURE_NONE = 0,
    RULES_GESTURE_SPACE_LEFT = 1,
    RULES_GESTURE_SPACE_RIGHT = 2,
    RULES_GESTURE_OVERVIEW = 3,    /* .missionControl */
    RULES_GESTURE_APP_OVERVIEW = 4 /* .appExpose */
} rules_gesture_action;

/* --- rule 7: quit_protection --------------------------------------------- */
/* Mirrors Sources/VorssaintCore/Core/QuitProtectionSupport.swift */

#define RULES_QP_HOLD_MIN_MS 250
#define RULES_QP_HOLD_MAX_MS 2000
#define RULES_QP_HOLD_DEFAULT_MS 800
#define RULES_QP_DOUBLE_MIN_MS 200
#define RULES_QP_DOUBLE_MAX_MS 1500
#define RULES_QP_DOUBLE_DEFAULT_MS 600
#define RULES_QP_EXC_MAX 16
#define RULES_APPID_MAX 96

typedef enum { RULES_QP_HOLD = 0, RULES_QP_DOUBLE = 1, RULES_QP_EXTRA = 2 } rules_qp_mode;
typedef enum {
    RULES_QP_SCOPE_ALL = 0,
    RULES_QP_SCOPE_SELECTED = 1,
    RULES_QP_SCOPE_EXCEPT = 2
} rules_qp_scope;

typedef struct {
    bool enabled;
    rules_qp_mode mode;
    unsigned hold_ms;
    unsigned double_ms;
    uint16_t extra_mod; /* KEY_LEFTSHIFT, KEY_LEFTALT or KEY_LEFTCTRL */
    rules_qp_scope scope;
    char exception[RULES_QP_EXC_MAX][RULES_APPID_MAX];
    unsigned n_exceptions;
} rules_qp_shortcut;

typedef struct {
    bool enabled;
    rules_qp_shortcut quit;  /* Ctrl+Q */
    rules_qp_shortcut close; /* Ctrl+W */
} rules_qp_config;

/* --- the whole configuration --------------------------------------------- */

typedef struct {
    rules_kdb_config kdb;
    rules_mcd_config mcd;
    rules_scroll_config scroll;
    rules_smooth_config smooth;
    rules_sk_config sk;
    rules_mbs_config mbs;
    rules_qp_config qp;
} rules_config;

/* The context the app pushes with SetContext: what the relay cannot see for
 * itself, because a grabbed input device says nothing about which window has
 * focus. Only quit_protection uses it today. */
typedef struct {
    char focused_app_id[RULES_APPID_MAX];
} rules_context;

/* --- notices -------------------------------------------------------------- */

/* Things a rule decided that the helper must tell the app about rather than
 * act on itself: a workspace gesture (switching workspaces is the
 * compositor's job, see WP-C1), a quit that was blocked or confirmed, a super
 * key tap action the app maps to a layout switch. Drained by the daemon into
 * the Event signal with kind "rule". */
#define RULES_NOTICE_CAP 64
#define RULES_NOTICE_MAX 192

typedef struct {
    uint64_t ts;
    char detail[RULES_NOTICE_MAX];
} rules_notice;

/* --- state ---------------------------------------------------------------- */

typedef struct {
    bool is_down;
    bool have_press, have_release, have_last;
    uint64_t last_press, last_release, last_event;
} rules_kdb_key;

typedef struct {
    bool accepted_down;
    bool suppressed_down;
    bool have_last_up, have_last_event;
    uint64_t last_up, last_event;
} rules_mcd_button;

/* SmoothScrollSupport.Engine, one axis. */
typedef struct {
    double remaining;
    double carry;
    int32_t emitted_units; /* toward the next whole REL_WHEEL notch */
} rules_smooth_axis;

/* SuperKeySupport.State */
typedef struct {
    bool is_held;
    bool is_alone;
    bool have_down_ts;
    uint64_t down_ts;
    bool did_repeat;
    bool mods_down; /* the configured combination is currently emitted */
    bool caps_led;  /* what LED_CAPSL was last driven to */
} rules_sk_state;

/* MouseButtonShortcutSupport.SideWheelGestureGate */
typedef struct {
    bool have_last;
    uint64_t last_ts;
    uint8_t fired_directions;
} rules_tilt_gate;

/* MouseSpacesGestureSupport.Tracker */
typedef struct {
    bool active;
    bool did_fire;
    int axis; /* 0 none, 1 horizontal, 2 vertical */
    double acc_x, acc_y;
    bool have_fired_at;
    double last_fired_at_s;
    uint64_t down_ts;
} rules_gesture_state;

typedef struct {
    /* QP_HOLD: the press is withheld until the deadline or the release. */
    bool pending;
    uint64_t pending_deadline;
    /* QP_DOUBLE: when the first press was seen. */
    bool armed;
    uint64_t armed_ts;
} rules_qp_state;

typedef struct {
    rules_config cfg;
    rules_context ctx;

    rules_kdb_key kdb[RULES_KEY_MAX + 1];
    bool kdb_have_last_accepted;
    uint16_t kdb_last_accepted;

    rules_mcd_button mcd[3];

    rules_smooth_axis smooth_v, smooth_h;
    bool smooth_have_frame;
    uint64_t smooth_last_frame;

    rules_sk_state sk;
    rules_tilt_gate tilt;
    rules_gesture_state gesture;
    bool mbs_held[RULES_MBS_MAX]; /* a bound button whose chord is down */

    rules_qp_state qp_quit, qp_close;

    /* Physical modifier state, tracked from the source stream because
     * quit_protection and super_key both have to know what is held. */
    bool mod_down[RULES_KEY_MAX + 1];
    unsigned mod_held_count;

    rules_notice notice[RULES_NOTICE_CAP];
    size_t notice_head, notice_tail;
    uint64_t stat_notices_dropped;

    uint64_t stat_in;
    uint64_t stat_out;
    uint64_t stat_kdb_dropped;
    uint64_t stat_mcd_dropped;
    uint64_t stat_taps;
    uint64_t stat_holds;
    uint64_t stat_smooth_frames;
    uint64_t stat_qp_blocked;
} rules_state;

/* Fill cfg with every rule at its documented default. Every rule ships off: a
 * relay that starts changing input the moment it is enabled is not something
 * a capabilities page can honestly describe. */
void rules_config_defaults(rules_config *cfg);

void rules_init(rules_state *st, const rules_config *cfg);

/* Replace the configuration without losing in-flight key state. Anything the
 * engine is currently holding down on the user's behalf (super key modifiers,
 * a mouse-button chord) is released into out first, so a reconfigure cannot
 * strand a modifier. */
int rules_reconfigure(rules_state *st, const rules_config *cfg, rules_event *out, int out_cap);

void rules_set_context(rules_state *st, const rules_context *ctx);

/* Feed one input event observed at now_ns. Writes 0..n output events into out
 * and returns the count, or -1 if out_cap is too small (never happens with
 * out_cap >= RULES_MAX_OUT). EV_SYN input is dropped; the caller re-syncs. */
#define RULES_MAX_OUT 32
int rules_process(rules_state *st, const rules_event *in, uint64_t now_ns, rules_event *out,
                  int out_cap);

/* Deadline for the next time-driven transition, in absolute ns, or 0 if the
 * engine is not waiting on the clock. The relay arms a timerfd on it, so a
 * quit-protection hold resolves and a smooth-scroll frame is emitted while
 * the user is doing nothing at all. */
uint64_t rules_deadline_ns(const rules_state *st);

/* Run every transition whose deadline has passed. */
int rules_timer(rules_state *st, uint64_t now_ns, rules_event *out, int out_cap);

/* Take the oldest pending notice. Returns false when there is none. */
bool rules_notice_pop(rules_state *st, rules_notice *out);

/* The largest SetRules or SetContext document the helper will look at. The
 * reader is not a streaming parser: json_find() restarts from the beginning
 * of the string for every key, so the work is O(keys x length) and an
 * unbounded string is an unbounded amount of a root process's time for one
 * unprivileged D-Bus call. D-Bus already caps a message at 128 MiB, which is
 * not a useful limit here. */
#define RULES_JSON_MAX (64 * 1024)

/* The SetRules document. Unknown keys are ignored; a missing key keeps the
 * default; a present key with a value outside its range is an error and the
 * whole document is refused. See docs/linux-port/PRIVILEGES.md § 4.1. */
int rules_config_from_json(const char *json, rules_config *cfg, char *err, size_t err_cap);

/* The SetContext document: {"focused_app_id":"org.gnome.TextEditor"}. */
int rules_context_from_json(const char *json, rules_context *ctx, char *err, size_t err_cap);

/* Serialise the active configuration and counters as JSON. */
int rules_state_to_json(const rules_state *st, char *buf, size_t cap);

/* --- pure decision functions, exported so the ported Swift vectors run
 * against exactly the code the relay runs ---------------------------------- */

/* KeyboardDebounceConfig.windowMs(for:), decodeKeyWindows, encodeKeyWindows. */
unsigned rules_kdb_window_ms(const rules_kdb_config *cfg, uint16_t code);
unsigned rules_kdb_sanitize_window(long ms);
int rules_kdb_decode_key_windows(const char *raw, rules_kdb_config *cfg);
int rules_kdb_encode_key_windows(const rules_kdb_config *cfg, char *buf, size_t cap);
/* KeyboardDebounceState.shouldSuppress */
bool rules_kdb_should_suppress(rules_state *st, uint16_t code, bool is_autorepeat, bool is_down,
                               uint64_t ts, const rules_kdb_config *cfg);
void rules_kdb_reset(rules_state *st);

/* MouseClickDebounceState.shouldSuppress; button is 0, 1 or 2. */
unsigned rules_mcd_sanitize_window(long ms);
bool rules_mcd_should_suppress(rules_state *st, int button, rules_click_kind kind, uint64_t ts,
                               const rules_mcd_config *cfg);
void rules_mcd_reset(rules_state *st);
/* MouseClickDebounceInput.resolve: BTN_LEFT/RIGHT/MIDDLE -> 0/1/2, else -1. */
int rules_mcd_button_for(uint16_t code);

/* ScrollWheelSupport.inversionPlan */
rules_inversion_plan rules_scroll_inversion_plan(bool has_vertical, bool has_horizontal,
                                                 bool shift_redirects_vertical,
                                                 bool invert_vertical, bool invert_horizontal);

/* SmoothScrollSupport, verbatim. Time is in seconds, as in the Swift. */
double rules_smooth_frame_delta(double remaining, double elapsed_s, int response);
double rules_smooth_ticks(double line, double fixed_point);
double rules_smooth_continuous_distance(double fixed_point_delta, double point_delta, double step);
void rules_smooth_whole_pixels(double distance, double carry, double *pixels, double *out_carry);
double rules_smooth_final_pixels(double distance, double carry);
double rules_smooth_carry(double current, double continuing);
void rules_smooth_axes(double vertical, double horizontal, bool shift_pressed, double *out_v,
                       double *out_h);
int rules_smooth_sanitize_step(int value);
int rules_smooth_sanitize_response(int value);
void rules_smooth_engine_add(rules_smooth_axis *axis, double distance);
double rules_smooth_engine_advance(rules_smooth_axis *axis, double elapsed_s, int response);
#define RULES_SMOOTH_FRAME_INTERVAL_S (1.0 / 60.0)
#define RULES_SMOOTH_MAX_FRAME_INTERVAL_S (1.0 / 20.0)
#define RULES_SMOOTH_FINISH_THRESHOLD 1.0

/* SuperKeySupport.State.decide and soloEffect. */
rules_sk_decision rules_sk_decide(rules_sk_state *st, rules_sk_event_kind kind, bool is_repeat,
                                  bool has_primary_modifiers, uint64_t ts, uint64_t threshold_ns,
                                  bool *out_repeated);
rules_sk_action rules_sk_solo_effect(rules_sk_action action, bool long_hold, bool repeated);
void rules_sk_reset(rules_sk_state *st);
/* SuperKeyMappingGuard: whether this source may be claimed at all. */
bool rules_sk_source_conflict(const rules_config *cfg, uint16_t source);

/* MouseButtonShortcutSupport.SideWheelGestureGate.shouldFire */
bool rules_tilt_should_fire(rules_tilt_gate *gate, int32_t input, uint64_t ts);
void rules_tilt_reset(rules_tilt_gate *gate);
/* MouseButtonShortcutSupport.canMap */
bool rules_mbs_can_map(int32_t input);
/* MouseSpacesGestureSupport.Tracker.advance, .resolved and .canBind */
rules_gesture_action rules_gesture_advance(rules_gesture_state *g, double x, double y,
                                           double now_s);
rules_gesture_action rules_gesture_resolved(rules_gesture_action a, bool follows_drag);
bool rules_gesture_can_bind(int32_t input);

/* QuitProtectionSupport */
unsigned rules_qp_sanitize_hold(double ms);
unsigned rules_qp_sanitize_double(double ms);
bool rules_qp_within_double(uint64_t first_ts, uint64_t second_ts, double interval_ms);
bool rules_qp_scope_allows(rules_qp_scope scope, const char *app_id, const rules_qp_shortcut *sc);
bool rules_qp_is_base_shortcut(uint16_t code, bool ctrl, bool alt, bool shift, bool meta,
                               uint16_t shortcut_key);
bool rules_qp_is_extra_shortcut(uint16_t code, bool ctrl, bool alt, bool shift, bool meta,
                                uint16_t shortcut_key, uint16_t extra_mod);

const char *rules_sk_state_name(const rules_sk_state *st);

#endif /* VORSSAINT_RULES_H */
