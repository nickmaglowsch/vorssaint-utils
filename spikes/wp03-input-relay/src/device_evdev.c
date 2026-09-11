/* Real device backend: libudev discovery, EVIOCGRAB on every source, and one
 * uinput device advertising the union of what the sources can produce.
 *
 * This is the code the helper would ship. It is compiled unconditionally; on a
 * kernel without CONFIG_INPUT_UINPUT, open() fails with the exact errno and
 * the daemon says so rather than pretending. */
#include "device.h"

#include <errno.h>
#include <fcntl.h>
#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>
#include <libudev.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAX_SOURCES 32

/* The three udev properties the relay claims. Anything else (tablets, game
 * pads, power buttons, the virtual console keyboard) is left alone: grabbing
 * more than is needed is exactly the over-reach PRIVILEGES.md argues against. */
static const char *const CLAIMED_PROPERTIES[] = {
    "ID_INPUT_KEYBOARD",
    "ID_INPUT_MOUSE",
    "ID_INPUT_TOUCHPAD",
};
#define N_CLAIMED (sizeof(CLAIMED_PROPERTIES) / sizeof(CLAIMED_PROPERTIES[0]))

typedef struct {
    int fd;
    struct libevdev *dev;
    char node[64];
    char name[128];
    char kind[64];
    bool grabbed;
} source;

typedef struct {
    source src[MAX_SOURCES];
    int n_src;
    struct pollfd pfd[MAX_SOURCES];
    struct libevdev_uinput *uidev;
    struct libevdev *out_template;
    bool grab;
} evdev_priv;

static void kind_append(char *kind, size_t cap, const char *what)
{
    if (kind[0])
        strncat(kind, "+", cap - strlen(kind) - 1);
    strncat(kind, what, cap - strlen(kind) - 1);
}

static int add_source(evdev_priv *p, const char *node, const char *kind, bool grab, char *err, size_t err_cap)
{
    source *s;
    int rc;

    for (int i = 0; i < p->n_src; i++) {
        if (strcmp(p->src[i].node, node) == 0) {
            kind_append(p->src[i].kind, sizeof(p->src[i].kind), kind);
            return 0;
        }
    }
    if (p->n_src >= MAX_SOURCES) {
        snprintf(err, err_cap, "more than %d input sources", MAX_SOURCES);
        return -ENOSPC;
    }

    s = &p->src[p->n_src];
    memset(s, 0, sizeof(*s));
    s->fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (s->fd < 0) {
        snprintf(err, err_cap, "open %s: %s", node, strerror(errno));
        return -errno;
    }
    rc = libevdev_new_from_fd(s->fd, &s->dev);
    if (rc < 0) {
        snprintf(err, err_cap, "libevdev_new_from_fd %s: %s", node, strerror(-rc));
        close(s->fd);
        return rc;
    }
    snprintf(s->node, sizeof(s->node), "%s", node);
    snprintf(s->name, sizeof(s->name), "%s", libevdev_get_name(s->dev) ?: "?");
    snprintf(s->kind, sizeof(s->kind), "%s", kind);

    if (grab) {
        rc = libevdev_grab(s->dev, LIBEVDEV_GRAB);
        if (rc < 0) {
            /* A refused grab means another relay (keyd, interception-tools)
             * already owns the device. Running two grabbers silently splits
             * the event stream, so this is fatal, not a warning. */
            snprintf(err, err_cap, "EVIOCGRAB %s (%s): %s", node, s->name, strerror(-rc));
            libevdev_free(s->dev);
            close(s->fd);
            return rc;
        }
        s->grabbed = true;
    }
    p->n_src++;
    return 0;
}

/* Ungrab and close every claimed source. Safe to call twice, and safe to call
 * halfway through a failed enumeration: a device this relay has grabbed is
 * unusable by the session until it is released, so every exit path leads here. */
static void release_sources(evdev_priv *p)
{
    for (int i = 0; i < p->n_src; i++) {
        if (p->src[i].grabbed)
            libevdev_grab(p->src[i].dev, LIBEVDEV_UNGRAB);
        if (p->src[i].dev)
            libevdev_free(p->src[i].dev);
        if (p->src[i].fd >= 0)
            close(p->src[i].fd);
        memset(&p->src[i], 0, sizeof(p->src[i]));
    }
    p->n_src = 0;
}

static int discover(evdev_priv *p, bool grab, char *err, size_t err_cap)
{
    struct udev *udev = udev_new();
    int rc = 0;

    if (!udev) {
        snprintf(err, err_cap, "udev_new failed");
        return -ENOMEM;
    }

    for (size_t k = 0; k < N_CLAIMED && rc == 0; k++) {
        struct udev_enumerate *e = udev_enumerate_new(udev);
        struct udev_list_entry *entry;

        if (!e) {
            rc = -ENOMEM;
            break;
        }
        udev_enumerate_add_match_subsystem(e, "input");
        udev_enumerate_add_match_property(e, CLAIMED_PROPERTIES[k], "1");
        udev_enumerate_scan_devices(e);

        udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e)) {
            const char *syspath = udev_list_entry_get_name(entry);
            struct udev_device *d = udev_device_new_from_syspath(udev, syspath);
            const char *node;

            if (!d)
                continue;
            node = udev_device_get_devnode(d);
            /* A single physical device shows up as several udev nodes; only
             * the eventN character device carries the event stream. */
            if (node && strncmp(node, "/dev/input/event", 16) == 0) {
                const char *kind = CLAIMED_PROPERTIES[k] + strlen("ID_INPUT_");
                rc = add_source(p, node, kind, grab, err, err_cap);
            }
            udev_device_unref(d);
            if (rc < 0)
                break;
        }
        udev_enumerate_unref(e);
    }

    udev_unref(udev);
    if (rc == 0 && p->n_src == 0) {
        snprintf(err, err_cap,
                 "no ID_INPUT_KEYBOARD/MOUSE/TOUCHPAD device found under /dev/input");
        return -ENODEV;
    }
    return rc;
}

/* The output device advertises the union of the sources' capabilities. A
 * uinput device that under-advertises silently drops events the relay
 * forwards, and libinput classifies the virtual device from these bits. */
static int build_output(evdev_priv *p, char *err, size_t err_cap)
{
    struct libevdev *out = libevdev_new();
    int rc;

    if (!out) {
        snprintf(err, err_cap, "libevdev_new failed");
        return -ENOMEM;
    }
    libevdev_set_name(out, "Vorssaint Relay");
    libevdev_set_id_vendor(out, 0x1d6b);  /* Linux Foundation */
    libevdev_set_id_product(out, 0x5670); /* arbitrary, stable */
    libevdev_set_id_version(out, 1);
    libevdev_set_id_bustype(out, BUS_VIRTUAL);

    for (int i = 0; i < p->n_src; i++) {
        struct libevdev *in = p->src[i].dev;
        for (unsigned type = 0; type <= EV_MAX; type++) {
            if (type == EV_SYN || !libevdev_has_event_type(in, type))
                continue;
            libevdev_enable_event_type(out, type);
            for (unsigned code = 0; code <= (unsigned)libevdev_event_type_get_max(type); code++) {
                if (!libevdev_has_event_code(in, type, code))
                    continue;
                if (type == EV_ABS) {
                    const struct input_absinfo *ai = libevdev_get_abs_info(in, code);
                    libevdev_enable_event_code(out, type, code, ai);
                } else {
                    libevdev_enable_event_code(out, type, code, NULL);
                }
            }
        }
    }
    /* The rules can synthesise keys no source has (Escape, Control on a
     * mouse-only session), so the output must be able to send them. */
    libevdev_enable_event_type(out, EV_KEY);
    libevdev_enable_event_code(out, EV_KEY, KEY_ESC, NULL);
    libevdev_enable_event_code(out, EV_KEY, KEY_LEFTCTRL, NULL);

    rc = libevdev_uinput_create_from_device(out, LIBEVDEV_UINPUT_OPEN_MANAGED, &p->uidev);
    if (rc < 0) {
        snprintf(err, err_cap, "uinput create (/dev/uinput): %s", strerror(-rc));
        libevdev_free(out);
        return rc;
    }
    p->out_template = out;
    return 0;
}

static int ev_open(input_backend *self, bool grab, char *err, size_t err_cap)
{
    evdev_priv *p = self->priv;
    int rc;

    /* The helper closes the backend on Enable(false) and opens a fresh one on
     * the next Enable(true), so open() must start from an empty source list.
     * Without this reset a reopen would append to the previous enumeration and
     * grab every device twice. */
    if (p->n_src != 0 || p->uidev != NULL) {
        snprintf(err, err_cap, "backend already open with %d source(s)", p->n_src);
        return -EBUSY;
    }

    /* grab == false is listen-only (--tap): the relay reads devices to show the
     * user what they pressed and emits nothing, so it needs neither the grab
     * nor an output device. Requiring uinput here would make shortcut
     * recording fail on machines where only the relay proper cannot run. */
    if (grab) {
        /* LOCKOUT GUARD -- do not reorder. The output path is proven to work
         * before a single device is grabbed. If uinput were opened after the
         * grabs and then failed, every keyboard and pointer in the session
         * would be held by a relay that cannot emit anything, and the user
         * would have no way to type their way out of it. */
        int fd = open("/dev/uinput", O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            snprintf(err, err_cap, "open /dev/uinput: %s (errno %d)", strerror(errno), errno);
            return -errno;
        }
        close(fd);
    }

    p->grab = grab;
    rc = discover(p, grab, err, err_cap);
    if (rc < 0) {
        /* Enumeration can fail after some devices were already grabbed. Leaving
         * those held would take the user's keyboard away on a failed start. */
        release_sources(p);
        return rc;
    }
    if (!grab)
        return 0;

    rc = build_output(p, err, err_cap);
    if (rc < 0)
        release_sources(p);
    return rc;
}

static int ev_read(input_backend *self, rules_event *ev, uint64_t *ts_ns, uint64_t deadline_ns)
{
    evdev_priv *p = self->priv;
    struct input_event ie;

    for (;;) {
        int timeout_ms = -1;
        int n;

        /* Drain whatever libevdev already has buffered before polling: its
         * fd can be quiet while events remain in the library's queue. */
        for (int i = 0; i < p->n_src; i++) {
            int rc = libevdev_next_event(p->src[i].dev, LIBEVDEV_READ_FLAG_NORMAL, &ie);
            if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                /* The kernel buffer overflowed. Resync, discarding the
                 * dropped window; the alternative is stuck modifiers. */
                while (libevdev_next_event(p->src[i].dev, LIBEVDEV_READ_FLAG_SYNC, &ie) ==
                       LIBEVDEV_READ_STATUS_SYNC)
                    ;
                continue;
            }
            if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
                ev->type = ie.type;
                ev->code = ie.code;
                ev->value = ie.value;
                *ts_ns = now_monotonic_ns();
                return DEV_READ_EVENT;
            }
            if (rc != -EAGAIN)
                return DEV_READ_ERROR;
        }

        if (deadline_ns != 0) {
            uint64_t now = now_monotonic_ns();
            if (now >= deadline_ns)
                return DEV_READ_TIMEOUT;
            timeout_ms = (int)((deadline_ns - now + 999999ULL) / 1000000ULL);
        }

        for (int i = 0; i < p->n_src; i++) {
            p->pfd[i].fd = p->src[i].fd;
            p->pfd[i].events = POLLIN;
            p->pfd[i].revents = 0;
        }
        n = poll(p->pfd, (nfds_t)p->n_src, timeout_ms);
        if (n == 0)
            return DEV_READ_TIMEOUT;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return DEV_READ_ERROR;
        }
        for (int i = 0; i < p->n_src; i++) {
            if (p->pfd[i].revents & (POLLERR | POLLHUP))
                return DEV_READ_EOF; /* device unplugged */
        }
    }
}

static int ev_write(input_backend *self, const rules_event *ev)
{
    evdev_priv *p = self->priv;
    if (!p->uidev)
        return -ENODEV; /* listen-only: there is no output device by design */
    return libevdev_uinput_write_event(p->uidev, ev->type, ev->code, ev->value);
}

static int ev_sync(input_backend *self)
{
    evdev_priv *p = self->priv;
    if (!p->uidev)
        return -ENODEV;
    return libevdev_uinput_write_event(p->uidev, EV_SYN, SYN_REPORT, 0);
}

static int ev_describe(input_backend *self, char *buf, size_t cap)
{
    evdev_priv *p = self->priv;
    size_t used = 0;
    int n;

    n = snprintf(buf, cap, "[");
    if (n < 0)
        return n;
    used = (size_t)n;
    for (int i = 0; i < p->n_src && used < cap; i++) {
        n = snprintf(buf + used, cap - used,
                     "%s{\"node\":\"%s\",\"name\":\"%s\",\"kind\":\"%s\",\"grabbed\":%s}",
                     i ? "," : "", p->src[i].node, p->src[i].name, p->src[i].kind,
                     p->src[i].grabbed ? "true" : "false");
        if (n < 0)
            return n;
        used += (size_t)n;
    }
    if (used < cap)
        used += (size_t)snprintf(buf + used, cap - used, "]");
    return (int)used;
}

static void ev_close(input_backend *self)
{
    evdev_priv *p = self->priv;

    if (p->uidev) {
        libevdev_uinput_destroy(p->uidev);
        p->uidev = NULL;
    }
    if (p->out_template) {
        libevdev_free(p->out_template);
        p->out_template = NULL;
    }
    release_sources(p);
    free(p);
    free(self);
}

input_backend *device_evdev_new(void)
{
    input_backend *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->priv = calloc(1, sizeof(evdev_priv));
    if (!b->priv) {
        free(b);
        return NULL;
    }
    b->name = "evdev";
    b->open = ev_open;
    b->read = ev_read;
    b->write = ev_write;
    b->sync = ev_sync;
    b->describe = ev_describe;
    b->close = ev_close;
    return b;
}
