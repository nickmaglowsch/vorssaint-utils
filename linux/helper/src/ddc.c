#include "ddc.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* --- framing -------------------------------------------------------------- */

uint8_t ddc_checksum(uint8_t seed, const uint8_t *buf, size_t len)
{
    uint8_t x = seed;
    for (size_t i = 0; i < len; i++)
        x = (uint8_t)(x ^ buf[i]);
    return x;
}

int ddc_build_set_vcp(uint8_t vcp, uint16_t value, uint8_t *buf, size_t cap)
{
    if (cap < 7)
        return -EINVAL;
    buf[0] = DDC_HOST_ADDR;
    buf[1] = 0x80 | 4; /* four data bytes follow */
    buf[2] = 0x03;     /* Set VCP Feature */
    buf[3] = vcp;
    buf[4] = (uint8_t)(value >> 8);
    buf[5] = (uint8_t)(value & 0xFF);
    /* The address byte the frame travels under is part of the checksum even
     * though the i2c layer, not this buffer, puts it on the wire. */
    buf[6] = ddc_checksum(DDC_ADDR_W, buf, 6);
    return 7;
}

int ddc_build_get_vcp(uint8_t vcp, uint8_t *buf, size_t cap)
{
    if (cap < 5)
        return -EINVAL;
    buf[0] = DDC_HOST_ADDR;
    buf[1] = 0x80 | 2;
    buf[2] = 0x01; /* Get VCP Feature */
    buf[3] = vcp;
    buf[4] = ddc_checksum(DDC_ADDR_W, buf, 4);
    return 5;
}

int ddc_parse_get_reply(const uint8_t *buf, size_t len, uint8_t expect_vcp, uint16_t *current,
                        uint16_t *max, char *err, size_t err_cap)
{
    uint8_t n_data, want;

    if (len < 11) {
        snprintf(err, err_cap, "reply too short: %zu bytes, expected 11", len);
        return -EBADMSG;
    }
    if (buf[0] != DDC_ADDR_W) {
        snprintf(err, err_cap, "reply source address 0x%02x, expected 0x%02x", buf[0], DDC_ADDR_W);
        return -EBADMSG;
    }
    if ((buf[1] & 0x80) == 0) {
        snprintf(err, err_cap, "reply length byte 0x%02x has no 0x80 flag", buf[1]);
        return -EBADMSG;
    }
    n_data = buf[1] & 0x7F;
    if (n_data != 8) {
        snprintf(err, err_cap, "reply carries %u data bytes, expected 8", n_data);
        return -EBADMSG;
    }
    /* Seed 0x50, not 0x6E: see the comment at the top of ddc.h. */
    want = ddc_checksum(DDC_REPLY_SEED, buf, 10);
    if (want != buf[10]) {
        snprintf(err, err_cap, "reply checksum 0x%02x, computed 0x%02x", buf[10], want);
        return -EBADMSG;
    }
    if (buf[2] != 0x02) {
        snprintf(err, err_cap, "reply opcode 0x%02x, expected 0x02 (Get VCP Reply)", buf[2]);
        return -EPROTO;
    }
    if (buf[3] != 0x00) {
        snprintf(err, err_cap, "display reported result code 0x%02x for VCP 0x%02x%s", buf[3],
                 expect_vcp, buf[3] == 0x01 ? " (unsupported feature)" : "");
        return -EPROTO;
    }
    if (buf[4] != expect_vcp) {
        snprintf(err, err_cap, "reply is for VCP 0x%02x, asked for 0x%02x", buf[4], expect_vcp);
        return -EPROTO;
    }
    if (max)
        *max = (uint16_t)((buf[6] << 8) | buf[7]);
    if (current)
        *current = (uint16_t)((buf[8] << 8) | buf[9]);
    return 0;
}

/* --- validation ----------------------------------------------------------- */

bool ddc_bus_name_ok(const char *bus)
{
    size_t digits = 0;

    if (!bus || strncmp(bus, "i2c-", 4) != 0)
        return false;
    for (const char *p = bus + 4; *p; p++) {
        if (!isdigit((unsigned char)*p))
            return false;
        if (++digits > 4)
            return false;
    }
    return digits > 0;
}

/* --- the real transport --------------------------------------------------- */

typedef struct {
    int fd;
    char path[64];
} i2c_priv;

static ssize_t i2c_write(ddc_transport *self, const uint8_t *buf, size_t len)
{
    i2c_priv *p = self->priv;
    ssize_t n = write(p->fd, buf, len);
    return n < 0 ? -errno : n;
}

static ssize_t i2c_read(ddc_transport *self, uint8_t *buf, size_t cap)
{
    i2c_priv *p = self->priv;
    ssize_t n = read(p->fd, buf, cap);
    return n < 0 ? -errno : n;
}

static void i2c_sleep(ddc_transport *self, unsigned us)
{
    struct timespec ts = {.tv_sec = us / 1000000, .tv_nsec = (long)(us % 1000000) * 1000L};
    (void)self;
    nanosleep(&ts, NULL);
}

static void i2c_close(ddc_transport *self)
{
    i2c_priv *p = self->priv;
    if (p->fd >= 0)
        close(p->fd);
    free(p);
    free(self);
}

ddc_transport *ddc_transport_i2c(const char *bus, char *err, size_t err_cap)
{
    ddc_transport *t;
    i2c_priv *p;

    if (!ddc_bus_name_ok(bus)) {
        snprintf(err, err_cap, "not an i2c bus name: '%s' (expected i2c-N)", bus ? bus : "");
        return NULL;
    }
    t = calloc(1, sizeof(*t));
    p = calloc(1, sizeof(*p));
    if (!t || !p) {
        free(t);
        free(p);
        snprintf(err, err_cap, "out of memory");
        return NULL;
    }
    snprintf(p->path, sizeof(p->path), "/dev/%s", bus);
    p->fd = open(p->path, O_RDWR | O_CLOEXEC);
    if (p->fd < 0) {
        snprintf(err, err_cap, "open %s: %s", p->path, strerror(errno));
        free(p);
        free(t);
        return NULL;
    }
    /* The address is fixed at DDC_ADDR. The helper exposes no way to talk to
     * any other device on the bus, which is what keeps "read the monitor's
     * brightness" from also being "read the memory modules' SPD EEPROMs". */
    if (ioctl(p->fd, I2C_SLAVE, DDC_ADDR) < 0) {
        snprintf(err, err_cap, "I2C_SLAVE 0x%02x on %s: %s", DDC_ADDR, p->path, strerror(errno));
        close(p->fd);
        free(p);
        free(t);
        return NULL;
    }
    t->name = "i2c";
    t->write = i2c_write;
    t->read = i2c_read;
    t->sleep_us = i2c_sleep;
    t->close = i2c_close;
    t->priv = p;
    return t;
}

/* --- the fake monitor ----------------------------------------------------- */

#define FAKE_VCP_MAX 8

typedef struct {
    struct {
        uint8_t vcp;
        uint16_t cur, max;
        bool present;
    } f[FAKE_VCP_MAX];
    uint8_t reply[DDC_MAX_FRAME];
    size_t reply_len;
    char last_error[192];
    unsigned slept_us;
} fake_priv;

static ssize_t fake_write(ddc_transport *self, const uint8_t *buf, size_t len)
{
    fake_priv *p = self->priv;
    uint8_t n_data;

    p->reply_len = 0;
    p->last_error[0] = '\0';

    if (len < 4) {
        snprintf(p->last_error, sizeof(p->last_error), "frame too short: %zu", len);
        return -EIO;
    }
    if (buf[0] != DDC_HOST_ADDR) {
        snprintf(p->last_error, sizeof(p->last_error), "source address 0x%02x, expected 0x%02x",
                 buf[0], DDC_HOST_ADDR);
        return -EIO;
    }
    if ((buf[1] & 0x80) == 0) {
        snprintf(p->last_error, sizeof(p->last_error), "length byte 0x%02x has no 0x80 flag",
                 buf[1]);
        return -EIO;
    }
    n_data = buf[1] & 0x7F;
    if ((size_t)n_data + 3 != len) {
        snprintf(p->last_error, sizeof(p->last_error), "length byte says %u data bytes, frame is %zu",
                 n_data, len);
        return -EIO;
    }
    if (ddc_checksum(DDC_ADDR_W, buf, len - 1) != buf[len - 1]) {
        snprintf(p->last_error, sizeof(p->last_error), "bad checksum: got 0x%02x, computed 0x%02x",
                 buf[len - 1], ddc_checksum(DDC_ADDR_W, buf, len - 1));
        return -EIO;
    }

    if (buf[2] == 0x03 && n_data == 4) { /* Set VCP */
        uint8_t vcp = buf[3];
        uint16_t v = (uint16_t)((buf[4] << 8) | buf[5]);
        for (int i = 0; i < FAKE_VCP_MAX; i++) {
            if (p->f[i].present && p->f[i].vcp == vcp) {
                p->f[i].cur = v > p->f[i].max ? p->f[i].max : v;
                return (ssize_t)len;
            }
        }
        snprintf(p->last_error, sizeof(p->last_error), "Set on unsupported VCP 0x%02x", vcp);
        return (ssize_t)len; /* a real display ignores it silently */
    }

    if (buf[2] == 0x01 && n_data == 2) { /* Get VCP */
        uint8_t vcp = buf[3];
        uint8_t *r = p->reply;
        uint16_t cur = 0, max = 0;
        uint8_t result = 0x01; /* unsupported, until found */

        for (int i = 0; i < FAKE_VCP_MAX; i++) {
            if (p->f[i].present && p->f[i].vcp == vcp) {
                cur = p->f[i].cur;
                max = p->f[i].max;
                result = 0x00;
            }
        }
        r[0] = DDC_ADDR_W;
        r[1] = 0x80 | 8;
        r[2] = 0x02;
        r[3] = result;
        r[4] = vcp;
        r[5] = 0x00; /* set parameter, continuous */
        r[6] = (uint8_t)(max >> 8);
        r[7] = (uint8_t)(max & 0xFF);
        r[8] = (uint8_t)(cur >> 8);
        r[9] = (uint8_t)(cur & 0xFF);
        r[10] = ddc_checksum(DDC_REPLY_SEED, r, 10);
        p->reply_len = 11;
        return (ssize_t)len;
    }

    snprintf(p->last_error, sizeof(p->last_error), "unknown opcode 0x%02x", buf[2]);
    return -EIO;
}

static ssize_t fake_read(ddc_transport *self, uint8_t *buf, size_t cap)
{
    fake_priv *p = self->priv;
    size_t n = p->reply_len;

    if (n == 0)
        return -EIO; /* nothing pending: a real bus NAKs */
    if (n > cap)
        n = cap;
    memcpy(buf, p->reply, n);
    p->reply_len = 0;
    return (ssize_t)n;
}

static void fake_sleep(ddc_transport *self, unsigned us)
{
    ((fake_priv *)self->priv)->slept_us += us;
}

static void fake_close(ddc_transport *self)
{
    free(self->priv);
    free(self);
}

ddc_transport *ddc_transport_fake(void)
{
    ddc_transport *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->priv = calloc(1, sizeof(fake_priv));
    if (!t->priv) {
        free(t);
        return NULL;
    }
    t->name = "fake";
    t->write = fake_write;
    t->read = fake_read;
    t->sleep_us = fake_sleep;
    t->close = fake_close;
    return t;
}

void ddc_fake_set_vcp(ddc_transport *t, uint8_t vcp, uint16_t current, uint16_t max)
{
    fake_priv *p;
    if (!t || t->write != fake_write)
        return;
    p = t->priv;
    for (int i = 0; i < FAKE_VCP_MAX; i++) {
        if (p->f[i].present && p->f[i].vcp != vcp)
            continue;
        p->f[i].present = true;
        p->f[i].vcp = vcp;
        p->f[i].cur = current;
        p->f[i].max = max;
        return;
    }
}

int ddc_fake_get_vcp(ddc_transport *t, uint8_t vcp, uint16_t *current)
{
    fake_priv *p;
    if (!t || t->write != fake_write)
        return -EINVAL;
    p = t->priv;
    for (int i = 0; i < FAKE_VCP_MAX; i++) {
        if (p->f[i].present && p->f[i].vcp == vcp) {
            if (current)
                *current = p->f[i].cur;
            return 0;
        }
    }
    return -ENOENT;
}

const char *ddc_fake_last_error(ddc_transport *t)
{
    if (!t || t->write != fake_write)
        return "";
    return ((fake_priv *)t->priv)->last_error;
}

unsigned ddc_fake_slept_us(ddc_transport *t)
{
    if (!t || t->write != fake_write)
        return 0;
    return ((fake_priv *)t->priv)->slept_us;
}

/* --- operations ----------------------------------------------------------- */

int ddc_write_vcp(ddc_transport *t, uint8_t vcp, uint16_t value, char *err, size_t err_cap)
{
    uint8_t frame[DDC_MAX_FRAME];
    int len = ddc_build_set_vcp(vcp, value, frame, sizeof(frame));
    ssize_t n;

    if (len < 0) {
        snprintf(err, err_cap, "cannot build Set VCP frame");
        return len;
    }
    n = t->write(t, frame, (size_t)len);
    if (n < 0) {
        snprintf(err, err_cap, "i2c write: %s", strerror((int)-n));
        return (int)n;
    }
    if (n != len) {
        snprintf(err, err_cap, "short i2c write: %zd of %d bytes", n, len);
        return -EIO;
    }
    /* The specification's inter-transaction delay. Skipping it is the usual
     * cause of a monitor that works once and then stops answering. */
    t->sleep_us(t, DDC_WRITE_DELAY_US);
    return 0;
}

int ddc_read_vcp(ddc_transport *t, uint8_t vcp, uint16_t *current, uint16_t *max, char *err,
                 size_t err_cap)
{
    uint8_t frame[DDC_MAX_FRAME];
    uint8_t reply[DDC_MAX_FRAME];
    int len = ddc_build_get_vcp(vcp, frame, sizeof(frame));
    ssize_t n;

    if (len < 0) {
        snprintf(err, err_cap, "cannot build Get VCP frame");
        return len;
    }
    n = t->write(t, frame, (size_t)len);
    if (n < 0) {
        snprintf(err, err_cap, "i2c write: %s", strerror((int)-n));
        return (int)n;
    }
    t->sleep_us(t, DDC_READ_DELAY_US);

    n = t->read(t, reply, sizeof(reply));
    if (n < 0) {
        snprintf(err, err_cap, "i2c read: %s", strerror((int)-n));
        return (int)n;
    }
    return ddc_parse_get_reply(reply, (size_t)n, vcp, current, max, err, err_cap);
}

/* --- enumeration ---------------------------------------------------------- */

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int ddc_enumerate_json(const char *dev_root, const char *sys_root, char *buf, size_t cap)
{
    char *names[64];
    size_t n_names = 0, used = 0;
    struct dirent *de;
    DIR *d;
    int n;

    n = snprintf(buf, cap, "[");
    used = n > 0 ? (size_t)n : 0;

    d = opendir(dev_root);
    if (d) {
        while ((de = readdir(d)) != NULL && n_names < 64) {
            if (!ddc_bus_name_ok(de->d_name))
                continue;
            names[n_names++] = strdup(de->d_name);
        }
        closedir(d);
    }
    qsort(names, n_names, sizeof(names[0]), cmp_str);

    for (size_t i = 0; i < n_names; i++) {
        char path[512], label[128] = "";
        FILE *f;

        /* The adapter name says whether this bus can plausibly carry DDC at
         * all: a display controller's gmbus/AUX channel can, an SMBus
         * controller on the motherboard cannot, and the capabilities page
         * shows the name so the user is not offered every bus on the board. */
        snprintf(path, sizeof(path), "%s/%s/name", sys_root, names[i]);
        f = fopen(path, "re");
        if (f) {
            if (fgets(label, sizeof(label), f)) {
                size_t l = strlen(label);
                while (l > 0 && (label[l - 1] == '\n' || label[l - 1] == '\r'))
                    label[--l] = '\0';
                for (char *q = label; *q; q++)
                    if (*q == '"' || *q == '\\' || (unsigned char)*q < 0x20)
                        *q = '_';
            }
            fclose(f);
        }
        if (used < cap) {
            char node[80];
            snprintf(node, sizeof(node), "%s/%s", dev_root, names[i]);
            used += (size_t)snprintf(buf + used, cap - used,
                                     "%s{\"bus\":\"%s\",\"node\":\"%s\",\"name\":\"%s\","
                                     "\"readable\":%s}",
                                     i ? "," : "", names[i], node, label,
                                     access(node, R_OK | W_OK) == 0 ? "true" : "false");
        }
        free(names[i]);
    }
    if (used < cap)
        used += (size_t)snprintf(buf + used, cap - used, "]");
    return (int)used;
}
