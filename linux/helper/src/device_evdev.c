/* Real device backend: libudev discovery, EVIOCGRAB on every source, and one
 * uinput device advertising the union of what the sources can produce.
 *
 * This is the code the helper would ship. It is compiled unconditionally; on a
 * kernel without CONFIG_INPUT_UINPUT, open() fails with the exact errno and
 * the daemon says so rather than pretending. */
#include "device.h"

#include "grabholder.h"

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
    struct pollfd pfd[MAX_SOURCES + 1]; /* +1 for the udev monitor */
    struct libevdev_uinput *uidev;
    struct libevdev *out_template;
    bool grab;

    /* Hot-plug. The monitor is enabled before enumeration, so a device that
     * appears during discovery is reported rather than lost in the window
     * between the two. */
    struct udev *udev;
    struct udev_monitor *mon;
    int mon_fd;
    char hot_desc[192];
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
             * the event stream, so this is fatal, not a warning.
             *
             * "Device or resource busy" on its own leaves the user nothing to
             * act on, so name the process holding it: the capabilities hub
             * prints this string verbatim, and "held open by keyd (pid 812)"
             * is the difference between a bug report and a fix. */
            char holder[256] = "";
            int n_holders = grab_holder_describe(node, holder, sizeof(holder));
            if (n_holders > 0)
                snprintf(err, err_cap, "EVIOCGRAB %s (%s): %s; held open by %s", node, s->name,
                         strerror(-rc), holder);
            else
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

/* True if this udev device is one the relay claims, and not the relay's own
 * output device (70-vorssaint-uinput.rules tags that; grabbing it is a
 * feedback loop that locks the session out of its keyboard). Returns the
 * kind, or NULL. */
static const char *claimed_kind(struct udev_device *d)
{
    if (udev_device_get_property_value(d, "ID_INPUT_VORSSAINT_RELAY"))
        return NULL;
    for (size_t k = 0; k < N_CLAIMED; k++) {
        const char *v = udev_device_get_property_value(d, CLAIMED_PROPERTIES[k]);
        if (v && strcmp(v, "1") == 0)
            return CLAIMED_PROPERTIES[k] + strlen("ID_INPUT_");
    }
    return NULL;
}

static int monitor_start(evdev_priv *p, char *err, size_t err_cap)
{
    p->mon = udev_monitor_new_from_netlink(p->udev, "udev");
    if (!p->mon) {
        snprintf(err, err_cap, "udev_monitor_new_from_netlink failed");
        return -ENOMEM;
    }
    udev_monitor_filter_add_match_subsystem_devtype(p->mon, "input", NULL);
    if (udev_monitor_enable_receiving(p->mon) < 0) {
        snprintf(err, err_cap, "udev_monitor_enable_receiving failed");
        udev_monitor_unref(p->mon);
        p->mon = NULL;
        return -EIO;
    }
    p->mon_fd = udev_monitor_get_fd(p->mon);
    return 0;
}

static void monitor_stop(evdev_priv *p)
{
    if (p->mon) {
        udev_monitor_unref(p->mon);
        p->mon = NULL;
        p->mon_fd = -1;
    }
    if (p->udev) {
        udev_unref(p->udev);
        p->udev = NULL;
    }
}

static void drop_source(evdev_priv *p, const char *node)
{
    for (int i = 0; i < p->n_src; i++) {
        if (strcmp(p->src[i].node, node) != 0)
            continue;
        snprintf(p->hot_desc, sizeof(p->hot_desc), "removed %s (%s)", p->src[i].node,
                 p->src[i].name);
        if (p->src[i].grabbed)
            libevdev_grab(p->src[i].dev, LIBEVDEV_UNGRAB);
        if (p->src[i].dev)
            libevdev_free(p->src[i].dev);
        if (p->src[i].fd >= 0)
            close(p->src[i].fd);
        p->src[i] = p->src[p->n_src - 1];
        memset(&p->src[p->n_src - 1], 0, sizeof(p->src[0]));
        p->n_src--;
        return;
    }
}

/* Handle one pending udev event. Returns 1 if the source set changed. */
static int monitor_handle(evdev_priv *p)
{
    struct udev_device *d = udev_monitor_receive_device(p->mon);
    const char *action, *node, *kind;
    int changed = 0;

    if (!d)
        return 0;
    action = udev_device_get_action(d);
    node = udev_device_get_devnode(d);
    if (!action || !node || strncmp(node, "/dev/input/event", 16) != 0) {
        udev_device_unref(d);
        return 0;
    }

    if (strcmp(action, "remove") == 0) {
        p->hot_desc[0] = '\0';
        drop_source(p, node);
        changed = p->hot_desc[0] != '\0';
    } else if (strcmp(action, "add") == 0 && (kind = claimed_kind(d)) != NULL) {
        char err[320] = "";
        int before = p->n_src;
        if (add_source(p, node, kind, p->grab, err, sizeof(err)) == 0 && p->n_src > before) {
            snprintf(p->hot_desc, sizeof(p->hot_desc), "added %s (%s)%s", node,
                     p->src[p->n_src - 1].name, p->grab ? ", grabbed" : "");
            changed = 1;
        } else if (err[0]) {
            /* A device we cannot grab is worse than one we never saw: the
             * rules then apply to every keyboard but this one. Say so, and
             * keep relaying what we already hold. */
            fprintf(stderr, "relay: hot-plugged %s not claimed: %s\n", node, err);
        }
    }
    udev_device_unref(d);
    return changed;
}

static int discover(evdev_priv *p, bool grab, char *err, size_t err_cap)
{
    struct udev *udev = p->udev;
    int rc = 0;

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
            if (node && strncmp(node, "/dev/input/event", 16) == 0 && claimed_kind(d)) {
                const char *kind = CLAIMED_PROPERTIES[k] + strlen("ID_INPUT_");
                rc = add_source(p, node, kind, grab, err, err_cap);
            }
            udev_device_unref(d);
            if (rc < 0)
                break;
        }
        udev_enumerate_unref(e);
    }

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

    p->udev = udev_new();
    if (!p->udev) {
        snprintf(err, err_cap, "udev_new failed");
        return -ENOMEM;
    }
    /* Before discovery, so a device that appears while we are enumerating is
     * queued on the monitor rather than missed by both. */
    rc = monitor_start(p, err, err_cap);
    if (rc < 0) {
        udev_unref(p->udev);
        p->udev = NULL;
        return rc;
    }

    p->grab = grab;
    rc = discover(p, grab, err, err_cap);
    if (rc < 0) {
        /* Enumeration can fail after some devices were already grabbed. Leaving
         * those held would take the user's keyboard away on a failed start. */
        release_sources(p);
        monitor_stop(p);
        return rc;
    }
    if (!grab)
        return 0;

    rc = build_output(p, err, err_cap);
    if (rc < 0) {
        release_sources(p);
        monitor_stop(p);
    }
    return rc;
}

static int ev_read(input_backend *self, rules_event *ev, uint64_t *ts_ns, uint64_t deadline_ns)
{
    evdev_priv *p = self->priv;
    struct input_event ie;

    for (;;) {
        int timeout_ms = -1;
        int nfds;
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
        nfds = p->n_src;
        if (p->mon) {
            p->pfd[nfds].fd = p->mon_fd;
            p->pfd[nfds].events = POLLIN;
            p->pfd[nfds].revents = 0;
            nfds++;
        }
        n = poll(p->pfd, (nfds_t)nfds, timeout_ms);
        if (n == 0)
            return DEV_READ_TIMEOUT;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return DEV_READ_ERROR;
        }
        if (p->mon && (p->pfd[nfds - 1].revents & POLLIN)) {
            /* Report the change to the caller so it can log and re-describe.
             * Doing it here rather than silently means the helper's device
             * list and the kernel's never drift apart without a line saying
             * why. */
            if (monitor_handle(p))
                return DEV_READ_HOTPLUG;
            continue;
        }
        for (int i = 0; i < p->n_src; i++) {
            if (p->pfd[i].revents & (POLLERR | POLLHUP)) {
                /* One device went away. Drop just that one and carry on: an
                 * unplugged mouse must not cost the user the keyboards the
                 * relay is holding, which is what returning EOF here would
                 * do (the helper releases everything on a source loss). */
                char gone[64];
                snprintf(gone, sizeof(gone), "%s", p->src[i].node);
                p->hot_desc[0] = '\0';
                drop_source(p, gone);
                if (p->n_src == 0)
                    return DEV_READ_EOF; /* nothing left to relay */
                return DEV_READ_HOTPLUG;
            }
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
    monitor_stop(p);
    free(p);
    free(self);
}

static const char *ev_last_hotplug(input_backend *self)
{
    return ((evdev_priv *)self->priv)->hot_desc;
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
    ((evdev_priv *)b->priv)->mon_fd = -1;
    b->name = "evdev";
    b->open = ev_open;
    b->read = ev_read;
    b->write = ev_write;
    b->sync = ev_sync;
    b->describe = ev_describe;
    b->close = ev_close;
    b->last_hotplug = ev_last_hotplug;
    return b;
}
