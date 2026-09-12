/* Who is holding an input device open.
 *
 * `EVIOCGRAB` returning EBUSY means another process already has an exclusive
 * grab on the device — in practice keyd, interception-tools, or a second copy
 * of this helper. "Device or resource busy" is useless to a user who has to
 * decide what to stop, so the helper walks /proc/<pid>/fd for the device node
 * and names the process in the D-Bus error, which is what the capabilities
 * hub shows.
 *
 * This is a heuristic by construction: /proc shows who has the node *open*,
 * and the kernel does not expose who holds the grab. Every grabber must have
 * it open, so the true holder is always in the answer; a process that merely
 * reads the device without grabbing (evtest, libinput debug-events) can be in
 * it too. The message therefore says "held open by", not "grabbed by".
 */
#ifndef VORSSAINT_GRABHOLDER_H
#define VORSSAINT_GRABHOLDER_H

#include <stddef.h>

/* Writes a human-readable list of the processes holding devnode open, for
 * example "keyd (pid 812)" or "keyd (pid 812), evtest (pid 4110)", skipping
 * this process. Returns the number of holders found, or -errno.
 * buf is always NUL-terminated; on 0 holders it is set to "". */
int grab_holder_describe(const char *devnode, char *buf, size_t cap);

/* /proc, overridable so the test can drive a fake one. Pass NULL to reset. */
void grab_holder_set_proc_root(const char *root);

#endif /* VORSSAINT_GRABHOLDER_H */
