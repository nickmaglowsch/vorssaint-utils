/* In-process device backend: a queue in, a queue out.
 *
 * This exists because the kernel this spike was developed on has
 * CONFIG_INPUT_UINPUT unset, so no uinput device can exist. It is the same
 * shape as the evdev backend from the relay's point of view, which is what
 * makes the rules and latency results transferable. */
#include "device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FAKE_CAP 65536
#define FAKE_MAX_DEV 16

/* A described source, so hot-plug can add one after open() and describe()
 * reports it exactly as the evdev backend reports a real device. */
typedef struct {
    char node[64];
    char name[96];
    char kind[32];
    bool grabbed;
} fake_source;

typedef struct {
    rules_event src[FAKE_CAP];
    uint64_t src_ts[FAKE_CAP];
    size_t src_head, src_tail;

    rules_event sink[FAKE_CAP];
    size_t sink_n;

    fake_source dev[FAKE_MAX_DEV];
    size_t n_dev;

    /* Queued udev-style events, delivered one per read(). */
    struct {
        bool add;
        fake_source d;
    } hot[FAKE_MAX_DEV];
    size_t hot_head, hot_tail;
    /* Same arithmetic as HOT_DESC_MAX in device_evdev.c, over this backend's
     * own (smaller) field sizes: "added " + node + " (" + name + "), grabbed". */
    char hot_desc[sizeof(((fake_source *)0)->node) + sizeof(((fake_source *)0)->name) + 32];

    uint64_t clock_ns;
    bool grabbed;
} fake_priv;

static void fake_add_dev(fake_priv *p, const char *node, const char *name, const char *kind,
                         bool grabbed)
{
    if (p->n_dev >= FAKE_MAX_DEV)
        return;
    snprintf(p->dev[p->n_dev].node, sizeof(p->dev[p->n_dev].node), "%s", node);
    snprintf(p->dev[p->n_dev].name, sizeof(p->dev[p->n_dev].name), "%s", name);
    snprintf(p->dev[p->n_dev].kind, sizeof(p->dev[p->n_dev].kind), "%s", kind);
    p->dev[p->n_dev].grabbed = grabbed;
    p->n_dev++;
}

uint64_t now_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int fake_open(input_backend *self, bool grab, char *err, size_t err_cap)
{
    fake_priv *p = self->priv;
    (void)err;
    (void)err_cap;
    p->grabbed = grab;
    p->n_dev = 0;
    fake_add_dev(p, "fake:keyboard0", "vorssaint fake keyboard", "keyboard", grab);
    fake_add_dev(p, "fake:mouse0", "vorssaint fake mouse", "mouse", grab);
    return 0;
}

/* Apply one queued udev-style event. Returns true if the source set changed. */
static bool fake_drain_hotplug(fake_priv *p)
{
    if (p->hot_head == p->hot_tail)
        return false;

    /* By value, not by pointer into p: the formatting below writes into
     * p->hot_desc while reading from another field of the same object, which
     * is the overlap snprintf's `restrict` parameters forbid (-Wrestrict at
     * -O3). Taking a copy makes the two provably distinct. */
    if (p->hot[p->hot_head].add) {
        fake_source d = p->hot[p->hot_head].d;
        /* A device that appears while the relay holds a grab must be grabbed
         * too, or it is the one keyboard in the session the rules do not
         * apply to -- and on a machine where the relay swallows Caps Lock,
         * a keyboard that suddenly behaves differently is the bug report. */
        fake_add_dev(p, d.node, d.name, d.kind, p->grabbed);
        snprintf(p->hot_desc, sizeof(p->hot_desc), "added %s (%s)%s", d.node, d.name,
                 p->grabbed ? ", grabbed" : "");
    } else {
        fake_source want = p->hot[p->hot_head].d;
        for (size_t i = 0; i < p->n_dev; i++) {
            if (strcmp(p->dev[i].node, want.node) == 0) {
                fake_source gone = p->dev[i];
                snprintf(p->hot_desc, sizeof(p->hot_desc), "removed %s (%s)", gone.node,
                         gone.name);
                p->dev[i] = p->dev[p->n_dev - 1];
                p->n_dev--;
                break;
            }
        }
    }
    p->hot_head = (p->hot_head + 1) % FAKE_MAX_DEV;
    return true;
}

static int fake_read(input_backend *self, rules_event *ev, uint64_t *ts_ns, uint64_t deadline_ns)
{
    fake_priv *p = self->priv;

    if (fake_drain_hotplug(p))
        return DEV_READ_HOTPLUG;

    if (p->src_head == p->src_tail) {
        if (deadline_ns != 0) {
            p->clock_ns = deadline_ns;
            return DEV_READ_TIMEOUT;
        }
        return DEV_READ_EOF;
    }

    /* A deadline that falls before the next queued event fires first, exactly
     * as poll() would: this is what exercises the hold timer. */
    if (deadline_ns != 0 && p->src_ts[p->src_head] > deadline_ns) {
        p->clock_ns = deadline_ns;
        return DEV_READ_TIMEOUT;
    }

    *ev = p->src[p->src_head];
    *ts_ns = p->src_ts[p->src_head];
    p->clock_ns = p->src_ts[p->src_head];
    p->src_head++;
    return DEV_READ_EVENT;
}

static int fake_write(input_backend *self, const rules_event *ev)
{
    fake_priv *p = self->priv;
    if (p->sink_n >= FAKE_CAP)
        return -1;
    p->sink[p->sink_n++] = *ev;
    return 0;
}

static int fake_sync(input_backend *self)
{
    (void)self;
    return 0;
}

static int fake_describe(input_backend *self, char *buf, size_t cap)
{
    fake_priv *p = self->priv;
    size_t used = 0;
    int n = snprintf(buf, cap, "[");

    if (n < 0)
        return n;
    used = (size_t)n;
    for (size_t i = 0; i < p->n_dev && used < cap; i++) {
        n = snprintf(buf + used, cap - used,
                     "%s{\"node\":\"%s\",\"name\":\"%s\",\"kind\":\"%s\",\"grabbed\":%s}",
                     i ? "," : "", p->dev[i].node, p->dev[i].name, p->dev[i].kind,
                     p->dev[i].grabbed ? "true" : "false");
        if (n < 0)
            return n;
        used += (size_t)n;
    }
    if (used < cap)
        used += (size_t)snprintf(buf + used, cap - used, "]");
    return (int)used;
}

static const char *fake_last_hotplug(input_backend *self)
{
    return ((fake_priv *)self->priv)->hot_desc;
}

/* Counts closes for the lifecycle tests. It has to be a file-scope counter
 * rather than a field, because close() frees the object it would live in. */
static unsigned fake_closes;

unsigned device_fake_close_count(void) { return fake_closes; }

static void fake_close(input_backend *self)
{
    fake_closes++;
    free(self->priv);
    free(self);
}

input_backend *device_fake_new(void)
{
    input_backend *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->priv = calloc(1, sizeof(fake_priv));
    if (!b->priv) {
        free(b);
        return NULL;
    }
    b->name = "fake";
    b->open = fake_open;
    b->read = fake_read;
    b->write = fake_write;
    b->sync = fake_sync;
    b->describe = fake_describe;
    b->close = fake_close;
    b->last_hotplug = fake_last_hotplug;
    return b;
}

static bool is_fake(input_backend *b)
{
    return b && b->open == fake_open;
}

void device_fake_push_dev(input_backend *b, uint8_t dev_class, uint16_t dev_id, uint16_t type,
                          uint16_t code, int32_t value, uint64_t ts_ns)
{
    fake_priv *p;
    if (!is_fake(b))
        return;
    p = b->priv;
    if (p->src_tail >= FAKE_CAP)
        return;
    p->src[p->src_tail].type = type;
    p->src[p->src_tail].code = code;
    p->src[p->src_tail].value = value;
    p->src[p->src_tail].dev_id = dev_id;
    p->src[p->src_tail].dev_class = dev_class;
    p->src_ts[p->src_tail] = ts_ns;
    p->src_tail++;
}

/* The plain push keeps the shape the WP-S1 tests use: a keyboard event from
 * source 0. Anything that needs another device class says so. */
void device_fake_push(input_backend *b, uint16_t type, uint16_t code, int32_t value, uint64_t ts_ns)
{
    device_fake_push_dev(b, RULES_DEV_KEYBOARD, 0, type, code, value, ts_ns);
}

size_t device_fake_sink_count(input_backend *b)
{
    if (!is_fake(b))
        return 0;
    return ((fake_priv *)b->priv)->sink_n;
}

const rules_event *device_fake_sink(input_backend *b, size_t idx)
{
    fake_priv *p;
    if (!is_fake(b))
        return NULL;
    p = b->priv;
    if (idx >= p->sink_n)
        return NULL;
    return &p->sink[idx];
}

void device_fake_sink_clear(input_backend *b)
{
    if (!is_fake(b))
        return;
    ((fake_priv *)b->priv)->sink_n = 0;
}

size_t device_fake_pending(input_backend *b)
{
    fake_priv *p;
    if (!is_fake(b))
        return 0;
    p = b->priv;
    return p->src_tail - p->src_head;
}

void device_fake_inject_add(input_backend *b, const char *node, const char *name, const char *kind)
{
    fake_priv *p;
    size_t next;
    if (!is_fake(b))
        return;
    p = b->priv;
    next = (p->hot_tail + 1) % FAKE_MAX_DEV;
    if (next == p->hot_head)
        return;
    p->hot[p->hot_tail].add = true;
    snprintf(p->hot[p->hot_tail].d.node, sizeof(p->hot[p->hot_tail].d.node), "%s", node);
    snprintf(p->hot[p->hot_tail].d.name, sizeof(p->hot[p->hot_tail].d.name), "%s", name);
    snprintf(p->hot[p->hot_tail].d.kind, sizeof(p->hot[p->hot_tail].d.kind), "%s", kind);
    p->hot_tail = next;
}

void device_fake_inject_remove(input_backend *b, const char *node)
{
    fake_priv *p;
    size_t next;
    if (!is_fake(b))
        return;
    p = b->priv;
    next = (p->hot_tail + 1) % FAKE_MAX_DEV;
    if (next == p->hot_head)
        return;
    p->hot[p->hot_tail].add = false;
    snprintf(p->hot[p->hot_tail].d.node, sizeof(p->hot[p->hot_tail].d.node), "%s", node);
    p->hot_tail = next;
}

void device_fake_set_clock(input_backend *b, uint64_t now_ns)
{
    if (!is_fake(b))
        return;
    ((fake_priv *)b->priv)->clock_ns = now_ns;
}
