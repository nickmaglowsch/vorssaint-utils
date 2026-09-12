/* DDC/CI over /dev/i2c-*: the brightness path for external monitors.
 *
 * A monitor speaks MCCS over the display data channel at 7-bit I2C address
 * 0x37. The framing is fixed by the DDC/CI specification:
 *
 *   host -> display   0x51  (0x80|len)  <len bytes>  checksum
 *   display -> host   0x6E  (0x80|len)  <len bytes>  checksum
 *
 * with the checksum an XOR over every byte of the frame *including the I2C
 * address byte that carried it*: 0x6E (0x37<<1) for what the host sends, and
 * 0x50 for what the host reads back. That 0x50 is the quirk worth naming: it
 * is the host's own address 0x51 with the read bit cleared, and a reply whose
 * checksum is computed from 0x6E instead — which is what a naive reading of
 * the spec produces — fails against every real monitor.
 *
 * Why this is privileged at all: /dev/i2c-* is root-only, and it is a general
 * bus, not a monitor API. Anything on those pins is reachable from it, which
 * on many boards includes the SPD EEPROMs of the memory modules and, on some,
 * the embedded controller. PRIVILEGES.md § 7 carries the argument; the helper
 * therefore refuses any address but 0x37 and any bus that is not an i2c-N
 * under /dev, and never exposes a raw transfer method.
 *
 * NOT TESTED AGAINST HARDWARE. There is no /dev/i2c-* and no
 * /sys/bus/i2c in the container this was written in, so everything below has
 * only ever run against ddc_transport_fake(), which models one monitor
 * including checksum validation. The framing is checked byte for byte against
 * the specification's values in tests/test_ddc.c; whether a given display
 * tolerates the timing is a hardware question WP-B9 must answer.
 */
#ifndef VORSSAINT_DDC_H
#define VORSSAINT_DDC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define DDC_ADDR 0x37        /* 7-bit display address */
#define DDC_ADDR_W 0x6E      /* DDC_ADDR << 1, the byte on the wire */
#define DDC_HOST_ADDR 0x51   /* source address the host puts in its frames */
#define DDC_REPLY_SEED 0x50  /* checksum seed for a frame read back */
#define DDC_MAX_FRAME 36

/* Minimum spacing the specification requires between transactions. */
#define DDC_WRITE_DELAY_US 50000
#define DDC_READ_DELAY_US 40000

/* --- framing (pure, no I/O) ----------------------------------------------- */

/* Build "Set VCP Feature". Returns the frame length, or -EINVAL. */
int ddc_build_set_vcp(uint8_t vcp, uint16_t value, uint8_t *buf, size_t cap);

/* Build "Get VCP Feature". Returns the frame length, or -EINVAL. */
int ddc_build_get_vcp(uint8_t vcp, uint8_t *buf, size_t cap);

/* Parse a "Get VCP Feature Reply". Returns 0, or -EBADMSG / -EPROTO with a
 * reason in err. current/max may be NULL. */
int ddc_parse_get_reply(const uint8_t *buf, size_t len, uint8_t expect_vcp, uint16_t *current,
                        uint16_t *max, char *err, size_t err_cap);

uint8_t ddc_checksum(uint8_t seed, const uint8_t *buf, size_t len);

/* --- transport ------------------------------------------------------------ */

typedef struct ddc_transport ddc_transport;

struct ddc_transport {
    const char *name;
    ssize_t (*write)(ddc_transport *self, const uint8_t *buf, size_t len);
    ssize_t (*read)(ddc_transport *self, uint8_t *buf, size_t cap);
    void (*sleep_us)(ddc_transport *self, unsigned us);
    void (*close)(ddc_transport *self);
    void *priv;
};

/* Open /dev/i2c-N and select the DDC address. bus is validated: it must be
 * exactly "i2c-N". Returns NULL with a reason in err. */
ddc_transport *ddc_transport_i2c(const char *bus, char *err, size_t err_cap);

/* A fake monitor. Validates every frame it is handed (address, length byte,
 * checksum) and answers Get VCP from a small feature table, so a framing bug
 * fails the test rather than being echoed back. Sleeps are recorded, not
 * taken. */
ddc_transport *ddc_transport_fake(void);
void ddc_fake_set_vcp(ddc_transport *t, uint8_t vcp, uint16_t current, uint16_t max);
int ddc_fake_get_vcp(ddc_transport *t, uint8_t vcp, uint16_t *current);
const char *ddc_fake_last_error(ddc_transport *t);
unsigned ddc_fake_slept_us(ddc_transport *t);

bool ddc_bus_name_ok(const char *bus);

/* --- operations ----------------------------------------------------------- */

int ddc_write_vcp(ddc_transport *t, uint8_t vcp, uint16_t value, char *err, size_t err_cap);
int ddc_read_vcp(ddc_transport *t, uint8_t vcp, uint16_t *current, uint16_t *max, char *err,
                 size_t err_cap);

/* Enumerate /dev/i2c-* for GetCapabilities. Never fails; an absent /dev/i2c-*
 * is an empty array. */
int ddc_enumerate_json(const char *dev_root, const char *sys_root, char *buf, size_t cap);

#endif /* VORSSAINT_DDC_H */
