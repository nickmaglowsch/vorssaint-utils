/* DDC/CI framing and transactions.
 *
 * NOT A HARDWARE TEST. There is no /dev/i2c-* on this machine, so the only
 * transport exercised here is ddc_transport_fake(), a monitor model that
 * validates every frame it is handed. What that buys is real but bounded:
 *
 *  - the frame bytes are compared against the values the DDC/CI
 *    specification fixes, one byte at a time, including both checksum seeds
 *    (0x6E outbound, 0x50 for the reply) -- the seed is the classic bug and
 *    an implementation that used 0x6E for both would fail here;
 *  - the parser rejects short frames, wrong opcodes, wrong VCP codes, bad
 *    checksums and the display's own "unsupported feature" result code;
 *  - the specified inter-transaction delays are actually taken.
 *
 * What it cannot show is whether a given display answers, tolerates the
 * timing, or reports a sane maximum. That is WP-B9's, on hardware.
 */
#include "ddc.h"

#include "pathguard.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void ok(const char *name) { printf("  %-52s ok\n", name); }
static void fail(const char *name, const char *why)
{
    printf("  FAIL %s: %s\n", name, why);
    failures++;
}

static void hex(const uint8_t *b, size_t n, char *out, size_t cap)
{
    size_t used = 0;
    out[0] = '\0';
    for (size_t i = 0; i < n && used + 4 < cap; i++)
        used += (size_t)snprintf(out + used, cap - used, "%02x ", b[i]);
}

#define VCP_BRIGHTNESS 0x10
#define VCP_CONTRAST 0x12

int main(void)
{
    char err[512];
    char buf[128];

    printf("frame construction against the specification's bytes\n");
    {
        uint8_t f[16];
        /* Set VCP 0x10 (brightness) to 0x0032 = 50.
         *   0x51  source address
         *   0x84  0x80 | 4 data bytes
         *   0x03  Set VCP Feature
         *   0x10  VCP code
         *   0x00 0x32  value, big endian
         *   checksum = 0x6E ^ 0x51 ^ 0x84 ^ 0x03 ^ 0x10 ^ 0x00 ^ 0x32 */
        uint8_t want[] = {0x51, 0x84, 0x03, 0x10, 0x00, 0x32, 0x6E ^ 0x51 ^ 0x84 ^ 0x03 ^ 0x10 ^ 0x32};
        int n = ddc_build_set_vcp(VCP_BRIGHTNESS, 50, f, sizeof(f));

        hex(f, n > 0 ? (size_t)n : 0, buf, sizeof(buf));
        printf("  Set VCP 0x10 = 50 -> %s\n", buf);
        if (n == 7 && memcmp(f, want, 7) == 0)
            ok("Set VCP frame is byte for byte the specified frame");
        else
            fail("Set VCP frame is byte for byte the specified frame", buf);

        /* The checksum seed is the address byte the frame travels under.
         * Seeding with anything else is the classic DDC bug. */
        if (f[6] == ddc_checksum(DDC_ADDR_W, f, 6))
            ok("the outbound checksum is seeded with 0x6E (the write address)");
        else
            fail("the outbound checksum is seeded with 0x6E", buf);
    }
    {
        uint8_t f[16];
        uint8_t want[] = {0x51, 0x82, 0x01, 0x10, 0x6E ^ 0x51 ^ 0x82 ^ 0x01 ^ 0x10};
        int n = ddc_build_get_vcp(VCP_BRIGHTNESS, f, sizeof(f));

        hex(f, n > 0 ? (size_t)n : 0, buf, sizeof(buf));
        printf("  Get VCP 0x10      -> %s\n", buf);
        if (n == 5 && memcmp(f, want, 5) == 0)
            ok("Get VCP frame is byte for byte the specified frame");
        else
            fail("Get VCP frame is byte for byte the specified frame", buf);
    }
    {
        uint8_t f[4];
        if (ddc_build_set_vcp(0x10, 0, f, sizeof(f)) == -EINVAL)
            ok("a buffer too small for the frame is refused");
        else
            fail("a buffer too small for the frame is refused", "accepted");
    }

    printf("\nreply parsing\n");
    {
        /* 0x6E 0x88 0x02 result vcp type maxhi maxlo curhi curlo cksum, with
         * the checksum seeded 0x50 -- the host's own address with the read
         * bit cleared, not 0x6E. */
        uint8_t r[11] = {0x6E, 0x88, 0x02, 0x00, 0x10, 0x00, 0x00, 0x64, 0x00, 0x32, 0};
        uint16_t cur = 0, max = 0;

        r[10] = ddc_checksum(DDC_REPLY_SEED, r, 10);
        if (ddc_parse_get_reply(r, 11, VCP_BRIGHTNESS, &cur, &max, err, sizeof(err)) == 0 &&
            cur == 50 && max == 100)
            ok("a well-formed reply yields current and maximum");
        else
            fail("a well-formed reply yields current and maximum", err);

        /* The seed is not decorative: a reply checksummed from 0x6E must be
         * rejected, or a corrupted frame would be believed. */
        r[10] = ddc_checksum(DDC_ADDR_W, r, 10);
        if (ddc_parse_get_reply(r, 11, VCP_BRIGHTNESS, &cur, &max, err, sizeof(err)) == -EBADMSG &&
            strstr(err, "checksum"))
            ok("a reply checksummed with the wrong seed is rejected");
        else
            fail("a reply checksummed with the wrong seed is rejected", err);

        r[10] = ddc_checksum(DDC_REPLY_SEED, r, 10);
        if (ddc_parse_get_reply(r, 7, VCP_BRIGHTNESS, NULL, NULL, err, sizeof(err)) == -EBADMSG)
            ok("a short reply is rejected");
        else
            fail("a short reply is rejected", err);

        if (ddc_parse_get_reply(r, 11, VCP_CONTRAST, NULL, NULL, err, sizeof(err)) == -EPROTO &&
            strstr(err, "0x10"))
            ok("a reply for a different VCP code is rejected");
        else
            fail("a reply for a different VCP code is rejected", err);

        r[3] = 0x01; /* the display says: unsupported feature */
        r[10] = ddc_checksum(DDC_REPLY_SEED, r, 10);
        if (ddc_parse_get_reply(r, 11, VCP_BRIGHTNESS, NULL, NULL, err, sizeof(err)) == -EPROTO &&
            strstr(err, "unsupported"))
            ok("the display's own unsupported-feature result is surfaced");
        else
            fail("the display's own unsupported-feature result is surfaced", err);
    }

    printf("\ntransactions against the fake monitor\n");
    {
        ddc_transport *t = ddc_transport_fake();
        uint16_t cur = 0, max = 0, stored = 0;

        ddc_fake_set_vcp(t, VCP_BRIGHTNESS, 50, 100);

        if (ddc_read_vcp(t, VCP_BRIGHTNESS, &cur, &max, err, sizeof(err)) == 0 && cur == 50 &&
            max == 100)
            ok("DdcRead returns what the monitor holds");
        else
            fail("DdcRead returns what the monitor holds", err);

        if (ddc_write_vcp(t, VCP_BRIGHTNESS, 80, err, sizeof(err)) == 0 &&
            ddc_fake_get_vcp(t, VCP_BRIGHTNESS, &stored) == 0 && stored == 80)
            ok("DdcWrite changes it");
        else
            fail("DdcWrite changes it", err);

        if (ddc_read_vcp(t, VCP_BRIGHTNESS, &cur, NULL, err, sizeof(err)) == 0 && cur == 80)
            ok("and the change reads back over the wire, not from a cache");
        else
            fail("and the change reads back over the wire", err);

        /* The monitor model clamps, as a real one does. */
        ddc_write_vcp(t, VCP_BRIGHTNESS, 5000, err, sizeof(err));
        ddc_read_vcp(t, VCP_BRIGHTNESS, &cur, &max, err, sizeof(err));
        if (cur == 100)
            ok("a value above the maximum is clamped by the display");
        else
            fail("a value above the maximum is clamped by the display", err);

        if (ddc_read_vcp(t, VCP_CONTRAST, &cur, &max, err, sizeof(err)) == -EPROTO)
            ok("reading a feature the display lacks fails with its result code");
        else
            fail("reading a feature the display lacks fails with its result code", err);

        if (ddc_fake_last_error(t)[0] == '\0')
            ok("the monitor model accepted every frame it was sent");
        else
            fail("the monitor model accepted every frame it was sent", ddc_fake_last_error(t));

        /* The specified delays are the difference between a monitor that
         * answers and one that stops answering after the first call. */
        if (ddc_fake_slept_us(t) >= DDC_WRITE_DELAY_US + DDC_READ_DELAY_US)
            ok("the specified inter-transaction delays are taken");
        else
            fail("the specified inter-transaction delays are taken", "no delay");

        t->close(t);
    }

    printf("\nbus name validation (the path is built from untrusted input)\n");
    {
        struct {
            const char *bus;
            bool want;
        } cases[] = {
            {"i2c-0", true},  {"i2c-12", true}, {"i2c-", false},    {"i2c", false},
            {"../mem", false}, {"i2c-0/..", false}, {"i2c-99999", false}, {"", false},
        };
        bool all = true;
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
            if (ddc_bus_name_ok(cases[i].bus) != cases[i].want) {
                fail("only i2c-N is accepted", cases[i].bus);
                all = false;
            }
        if (all)
            ok("only i2c-N is accepted; traversal is rejected");

        if (ddc_transport_i2c("../../dev/mem", err, sizeof(err)) == NULL && strstr(err, "i2c-N"))
            ok("ddc_transport_i2c refuses a path before opening anything");
        else
            fail("ddc_transport_i2c refuses a path before opening anything", err);
    }

    printf("\nthe node itself must be a character device, not a link to one\n");
    {
        /* The name `i2c-99` is valid; what would be wrong is /dev/i2c-99 being
         * a symlink to something else. The real check runs against /dev, which
         * this test cannot write to, so the two halves are checked separately:
         * that a non-character-device is refused (here, through a path we can
         * create), and that the open uses O_NOFOLLOW (pathguard.c, and the
         * hwmon half of the same guard is exercised in test_fan.c). */
        char tmp[] = "/tmp/vorssaint-ddc-node-XXXXXX";
        char link[512];
        int fd = mkstemp(tmp);

        if (fd >= 0)
            close(fd);
        snprintf(link, sizeof(link), "%s.link", tmp);
        if (symlink(tmp, link) != 0)
            perror(link);

        if (path_guard_open_chardev(tmp, O_RDONLY, err, sizeof(err)) == -EINVAL &&
            strstr(err, "character device"))
            ok("a regular file where a device node belongs is refused");
        else
            fail("a regular file where a device node belongs is refused", err);

        if (path_guard_open_chardev(link, O_RDONLY, err, sizeof(err)) == -ELOOP &&
            strstr(err, "symbolic link"))
            ok("a symlink where a device node belongs is refused with ELOOP");
        else
            fail("a symlink where a device node belongs is refused with ELOOP", err);

        /* And the guard must still accept a real character device, or it would
         * refuse every genuine i2c node too. */
        {
            int ok_fd = path_guard_open_chardev("/dev/null", O_RDONLY, err, sizeof(err));
            if (ok_fd >= 0) {
                ok("a real character device is accepted");
                close(ok_fd);
            } else {
                fail("a real character device is accepted", err);
            }
        }

        unlink(link);
        unlink(tmp);
    }

    printf("\nenumeration for GetCapabilities\n");
    {
        char json[1024];
        ddc_enumerate_json("/dev", "/sys/class/i2c-dev", json, sizeof(json));
        printf("  real /dev/i2c-* on this machine: %s\n", json);
        if (strcmp(json, "[]") == 0)
            ok("no i2c bus here, reported as an empty list rather than an error");
        else
            printf("  (this machine has i2c buses; DDC may be testable on hardware here)\n");
    }

    printf("\n%s\n", failures ? "FAILURES" : "all ddc tests passed (fake transport only)");
    return failures ? 1 : 0;
}
