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

typedef struct {
    rules_event src[FAKE_CAP];
    uint64_t src_ts[FAKE_CAP];
    size_t src_head, src_tail;

    rules_event sink[FAKE_CAP];
    size_t sink_n;

    uint64_t clock_ns;
    bool grabbed;
} fake_priv;

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
    return 0;
}

static int fake_read(input_backend *self, rules_event *ev, uint64_t *ts_ns, uint64_t deadline_ns)
{
    fake_priv *p = self->priv;

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
    return snprintf(buf, cap,
                    "[{\"node\":\"fake:keyboard0\",\"name\":\"vorssaint fake keyboard\","
                    "\"kind\":\"keyboard\",\"grabbed\":%s},"
                    "{\"node\":\"fake:mouse0\",\"name\":\"vorssaint fake mouse\","
                    "\"kind\":\"mouse\",\"grabbed\":%s}]",
                    p->grabbed ? "true" : "false", p->grabbed ? "true" : "false");
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
    return b;
}

static bool is_fake(input_backend *b)
{
    return b && b->open == fake_open;
}

void device_fake_push(input_backend *b, uint16_t type, uint16_t code, int32_t value, uint64_t ts_ns)
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
    p->src_ts[p->src_tail] = ts_ns;
    p->src_tail++;
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

void device_fake_set_clock(input_backend *b, uint64_t now_ns)
{
    if (!is_fake(b))
        return;
    ((fake_priv *)b->priv)->clock_ns = now_ns;
}
