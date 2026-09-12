/* The relay's device layer.
 *
 * Two implementations sit behind this interface:
 *   device_evdev.c  libudev discovery + EVIOCGRAB + one uinput output device
 *   device_fake.c   in-process event queues, no kernel involvement
 *
 * The daemon, the rules engine and the D-Bus service are identical across
 * both; only which vtable is installed differs. That is what lets the rules
 * and the latency path be proven on a kernel with no uinput. */
#ifndef VORSSAINT_DEVICE_H
#define VORSSAINT_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rules.h"

typedef struct input_backend input_backend;

typedef enum {
    DEV_READ_HOTPLUG = 2, /* the source set changed; ev is not filled in */
    DEV_READ_EVENT = 1,   /* ev and ts_ns are filled in */
    DEV_READ_TIMEOUT = 0, /* deadline reached, no event */
    DEV_READ_EOF = -1,    /* source exhausted (fake/replay) */
    DEV_READ_ERROR = -2
} dev_read_result;

struct input_backend {
    const char *name;

    /* Discover and claim input sources, and create the output device.
     * grab selects whether EVIOCGRAB is taken on each source. Returns 0 on
     * success, -errno otherwise; err receives a human-readable reason. */
    int (*open)(input_backend *self, bool grab, char *err, size_t err_cap);

    /* Block until an event arrives or the absolute monotonic deadline passes.
     * deadline_ns of 0 means "block indefinitely". */
    int (*read)(input_backend *self, rules_event *ev, uint64_t *ts_ns, uint64_t deadline_ns);

    /* EV_KEY/EV_REL/EV_ABS go to the output device. EV_LED is different: the
     * lamp the user looks at is on the keyboard the relay grabbed, not on the
     * uinput device, so an EV_LED write is routed back to the sources. */
    int (*write)(input_backend *self, const rules_event *ev);
    int (*sync)(input_backend *self);

    /* JSON array describing the claimed sources, for GetDevices(). */
    int (*describe)(input_backend *self, char *buf, size_t cap);

    void (*close)(input_backend *self);

    /* What the last DEV_READ_HOTPLUG was, for the log and the Event stream:
     * "added /dev/input/event7 (Keychron K2)" or "removed …". Empty before
     * the first one. Never NULL. */
    const char *(*last_hotplug)(input_backend *self);

    void *priv;
};

/* Real backend. Always compiled; open() fails with a precise reason where
 * /dev/uinput or /dev/input are unavailable. */
input_backend *device_evdev_new(void);

/* Fake backend: events pushed in by the test driver come out of read(), and
 * everything written lands in a sink the driver can inspect. */
input_backend *device_fake_new(void);

/* Fake-backend driver API (no-ops on any other backend). */
void device_fake_push(input_backend *b, uint16_t type, uint16_t code, int32_t value, uint64_t ts_ns);
/* As above, but from a source of the given class and id, which is what a
 * per-device-class rule (scroll_invert) needs to be tested against. */
void device_fake_push_dev(input_backend *b, uint8_t dev_class, uint16_t dev_id, uint16_t type,
                          uint16_t code, int32_t value, uint64_t ts_ns);
size_t device_fake_sink_count(input_backend *b);
const rules_event *device_fake_sink(input_backend *b, size_t idx);
void device_fake_sink_clear(input_backend *b);
/* Number of source events not yet read. */
size_t device_fake_pending(input_backend *b);
/* How many fake backends have been closed since the process started. The
 * counter outlives the object, which is the point: close() frees the backend,
 * so a caller cannot ask a closed one whether it was closed. */
unsigned device_fake_close_count(void);
/* Inject a hot-plug add, as udev would report one. The next read() returns
 * DEV_READ_HOTPLUG, and the device is claimed on the same terms as the
 * sources already held (grabbed if the backend was opened with grab). */
void device_fake_inject_add(input_backend *b, const char *node, const char *name, const char *kind);
void device_fake_inject_remove(input_backend *b, const char *node);

/* Replace the fake's clock. Used by the replay driver so recorded timestamps,
 * not wall time, drive the rules. */
void device_fake_set_clock(input_backend *b, uint64_t now_ns);

uint64_t now_monotonic_ns(void);

#endif /* VORSSAINT_DEVICE_H */
