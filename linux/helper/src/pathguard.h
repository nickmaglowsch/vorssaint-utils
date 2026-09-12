/* Keeping a resolved path inside the tree it was supposed to name.
 *
 * The hwmon and i2c methods validate their `hwmonN` / `i2c-N` argument before
 * building a path from it, which stops traversal in the *name*. It does not
 * stop traversal through a **symlink already on disk**: a `hwmon0` that is a
 * link to somewhere else resolves to somewhere else, and the name was never
 * wrong.
 *
 * On a real system this is depth, not the primary defence. `/sys/class/hwmon`,
 * `/sys/devices` and `/dev` are root-owned and not writable by the
 * unprivileged caller, so it cannot plant the link in the first place; the
 * primary defence is that the caller never gets to name a path at all, only a
 * `hwmonN`. This layer is here for the cases the primary one does not cover: a
 * caller that is *already* root by another route, a misconfigured container
 * that bind-mounts something writable over part of `/sys`, and our own bugs.
 *
 * The rule is not "no symlinks", because that would be wrong on every real
 * machine: `/sys/class/<class>/<name>` is *always* a symlink into
 * `/sys/devices` -- that is how the sysfs class model works
 * (`eth0 -> ../../devices/pci0000:00/.../net/eth0`). So the rule is split:
 *
 *   - the class entry (`hwmonN`) MAY be a symlink, but its target must stay
 *     inside the boundary: the root's own realpath, or `/sys` when the root is
 *     itself under `/sys`;
 *   - the leaf (`pwmN`, `pwmN_enable`) MUST NOT be a symlink and must be a
 *     regular file. Real sysfs attributes never are anything else;
 *   - `/dev/i2c-N` MUST NOT be a symlink and must be a character device. Real
 *     i2c device nodes never are anything else.
 */
#ifndef VORSSAINT_PATHGUARD_H
#define VORSSAINT_PATHGUARD_H

#include <stddef.h>

/* Resolve <root>/<entry> and confirm it is a directory inside the boundary
 * described above. Writes the resolved directory into out.
 *
 * Returns 0, or a negative errno:
 *   -ELOOP   it resolved outside the boundary (the interesting refusal)
 *   -ENOENT  it does not exist
 *   -ENOTDIR it is not a directory
 * err receives a message naming both the path asked for and where it landed.
 */
int path_guard_dir(const char *root, const char *entry, char *out, size_t out_cap, char *err,
                   size_t err_cap);

/* Open <dir>/<leaf> with O_NOFOLLOW, and confirm it is a regular file.
 * flags is passed to open(2) (O_RDONLY or O_WRONLY|O_TRUNC).
 * Returns the fd, or a negative errno; -ELOOP means the leaf was a symlink. */
int path_guard_open_leaf(const char *dir, const char *leaf, int flags, char *err, size_t err_cap);

/* Open a device node with O_NOFOLLOW, and confirm it is a character device.
 * Returns the fd, or a negative errno. */
int path_guard_open_chardev(const char *path, int flags, char *err, size_t err_cap);

#endif /* VORSSAINT_PATHGUARD_H */
