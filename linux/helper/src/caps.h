/* GetCapabilities: what this machine can actually do, answered by trying.
 *
 * The capabilities page needs to distinguish "the helper is not installed",
 * "the helper is installed but this kernel has no uinput", "there are no
 * writable pwm channels on this board" and "there is no i2c bus a monitor
 * could be on" — because they lead to four different sentences and only one
 * of them is fixable by the user. Every field here is produced by attempting
 * the operation (open the node, stat the attribute, check W_OK as the root
 * the helper runs as), never by inferring from a name, because a device node
 * can exist with no driver behind it: that is exactly what happened to
 * /dev/uinput in the WP-03 spike container.
 */
#ifndef VORSSAINT_CAPS_H
#define VORSSAINT_CAPS_H

#include <stddef.h>

typedef struct {
    const char *uinput_path;  /* "/dev/uinput" */
    const char *input_dir;    /* "/dev/input" */
    const char *hwmon_root;   /* "/sys/class/hwmon" */
    const char *i2c_dev_root; /* "/dev" */
    const char *i2c_sys_root; /* "/sys/class/i2c-dev" */
} caps_paths;

void caps_paths_defaults(caps_paths *p);

/* Writes the JSON GetCapabilities returns. Always succeeds. */
int caps_to_json(const caps_paths *p, char *buf, size_t cap);

#endif /* VORSSAINT_CAPS_H */
