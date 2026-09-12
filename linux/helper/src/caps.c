#include "caps.h"

#include "ddc.h"
#include "fan.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void caps_paths_defaults(caps_paths *p)
{
    p->uinput_path = "/dev/uinput";
    p->input_dir = "/dev/input";
    p->hwmon_root = "/sys/class/hwmon";
    p->i2c_dev_root = "/dev";
    p->i2c_sys_root = "/sys/class/i2c-dev";
}

/* The only honest test of uinput is opening it. A node can exist for a driver
 * that is not in the kernel (mknod is a filesystem operation), and it answers
 * ENODEV; a kernel built without CONFIG_INPUT_UINPUT has no node at all and
 * answers ENOENT. Both are reported verbatim so the hub can tell the user
 * which one they have. */
static int uinput_probe(const char *path, char *reason, size_t cap)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        close(fd);
        reason[0] = '\0';
        return 1;
    }
    snprintf(reason, cap, "open %s: %s (errno %d)", path, strerror(errno), errno);
    return 0;
}

static int count_event_nodes(const char *dir)
{
    struct dirent *de;
    DIR *d = opendir(dir);
    int n = 0;

    if (!d)
        return -1; /* no /dev/input at all, which is not the same as zero */
    while ((de = readdir(d)) != NULL)
        if (strncmp(de->d_name, "event", 5) == 0)
            n++;
    closedir(d);
    return n;
}

int caps_to_json(const caps_paths *p, char *buf, size_t cap)
{
    char reason[256];
    char hwmon[4096];
    char i2c[2048];
    int have_uinput = uinput_probe(p->uinput_path, reason, sizeof(reason));
    int n_events = count_event_nodes(p->input_dir);

    fan_enumerate_json(p->hwmon_root, hwmon, sizeof(hwmon));
    ddc_enumerate_json(p->i2c_dev_root, p->i2c_sys_root, i2c, sizeof(i2c));

    return snprintf(buf, cap,
                    "{\"uinput\":{\"available\":%s,\"path\":\"%s\",\"reason\":\"%s\"},"
                    "\"input\":{\"dir\":\"%s\",\"event_nodes\":%d},"
                    "\"hwmon\":{\"root\":\"%s\",\"devices\":%s},"
                    "\"i2c\":{\"buses\":%s},"
                    "\"fan_watchdog_ms\":%llu,"
                    "\"ddc\":{\"address\":\"0x%02x\",\"hardware_tested\":false}}",
                    have_uinput ? "true" : "false", p->uinput_path, reason, p->input_dir, n_events,
                    p->hwmon_root, hwmon, i2c, (unsigned long long)(FAN_WATCHDOG_NS / 1000000ULL),
                    DDC_ADDR);
}
