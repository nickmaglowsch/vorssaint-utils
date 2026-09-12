/* Fan control over hwmon `pwmN` / `pwmN_enable`, with a watchdog.
 *
 * Writing a fan's duty cycle is the second-most dangerous thing the helper
 * does, and it fails differently from the input relay: a bad grab takes the
 * user's keyboard away until the daemon exits, but a fan left at a low duty
 * cycle by a client that crashed cooks the hardware silently, hours later,
 * with nothing on screen. So:
 *
 *  - every write is verified by reading the value back (sysfs accepts writes
 *    the driver then clamps, ignores, or refuses at a lower layer);
 *  - manual control is armed with a deadline. The client must call
 *    FanHeartbeat within FAN_WATCHDOG_NS or every channel this helper put
 *    into manual mode is handed back to the firmware's automatic curve. The
 *    safe state is the one the machine boots in, and it is what a crash, a
 *    disconnect, a suspend or a killed app all converge on.
 *
 * The sysfs root is a parameter so the tests drive a fake hwmon tree. There
 * is no hwmon at all in the container this was written in (/sys/class/hwmon
 * does not exist), so the fake is the only thing that has run here; the path
 * building, the read-back and the watchdog are the same code either way.
 */
#ifndef VORSSAINT_FAN_H
#define VORSSAINT_FAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FAN_MAX_TRACKED 32
#define FAN_WATCHDOG_NS (10ULL * 1000000000ULL) /* 10 s */

/* pwmN_enable values, as defined by Documentation/hwmon/sysfs-interface. */
#define PWM_ENABLE_NONE 0   /* full speed, no control */
#define PWM_ENABLE_MANUAL 1 /* pwmN is obeyed */
#define PWM_ENABLE_AUTO 2   /* the chip's automatic curve */

typedef struct {
    char hwmon[32]; /* "hwmon3" */
    uint32_t channel;
    uint8_t applied;
    int restore_enable; /* what pwmN_enable said before we touched it */
    bool manual;
} fan_channel;

typedef struct {
    char root[256]; /* "/sys/class/hwmon" */
    fan_channel ch[FAN_MAX_TRACKED];
    size_t n_ch;
    uint64_t deadline_ns; /* 0 when nothing is under manual control */
    uint64_t stat_writes;
    uint64_t stat_restores;
    uint64_t stat_watchdog_trips;
} fan_state;

void fan_init(fan_state *st, const char *root);

/* A hwmon name is untrusted input from an unprivileged caller and is turned
 * into a path in a root process, so it is validated rather than escaped:
 * exactly "hwmon" followed by one to eight digits. Returns true if accepted. */
bool fan_hwmon_name_ok(const char *name);

/* Set the duty cycle. Puts pwmN_enable into manual mode first, writes pwmN,
 * reads both back, and arms the watchdog. Returns 0, or -errno with a reason
 * in err. */
int fan_set_pwm(fan_state *st, const char *hwmon, uint32_t channel, uint8_t value, uint64_t now_ns,
                char *err, size_t err_cap);

/* Hand one channel back to the firmware's automatic curve, verified. */
int fan_set_auto(fan_state *st, const char *hwmon, uint32_t channel, char *err, size_t err_cap);

/* Push the watchdog deadline out. Returns -ENOENT if nothing is under manual
 * control, so a client heart-beating at a helper that restarted learns it
 * must re-apply its curve rather than believing it is still in charge. */
int fan_heartbeat(fan_state *st, uint64_t now_ns);

/* Called from the main loop. If the deadline has passed, every channel this
 * helper put into manual mode is restored. Returns the number restored. */
int fan_watchdog_tick(fan_state *st, uint64_t now_ns);

/* Restore everything unconditionally: Enable(false), SIGTERM, exit. */
int fan_restore_all(fan_state *st);

int fan_state_to_json(const fan_state *st, uint64_t now_ns, char *buf, size_t cap);

/* Enumerate hwmon devices and their writable pwm channels, as the JSON array
 * GetCapabilities reports. Never fails: an absent or unreadable root is an
 * empty array, which is the honest answer. */
int fan_enumerate_json(const char *root, char *buf, size_t cap);

#endif /* VORSSAINT_FAN_H */
