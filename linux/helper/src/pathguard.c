#include "pathguard.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* True if `path` is `prefix` itself or lies under it. Compared component-wise,
 * so /tmp/foo does not contain /tmp/foobar. */
static bool under(const char *path, const char *prefix)
{
    size_t n = strlen(prefix);

    if (n == 0)
        return false;
    while (n > 1 && prefix[n - 1] == '/')
        n--; /* tolerate a trailing slash on the prefix */
    if (strncmp(path, prefix, n) != 0)
        return false;
    return path[n] == '\0' || path[n] == '/';
}

int path_guard_dir(const char *root, const char *entry, char *out, size_t out_cap, char *err,
                   size_t err_cap)
{
    char asked[PATH_MAX];
    char *real_root = NULL;
    char *real_entry = NULL;
    struct stat st;
    int rc = 0;

    if (snprintf(asked, sizeof(asked), "%s/%s", root, entry) >= (int)sizeof(asked)) {
        snprintf(err, err_cap, "path under %s is too long", root);
        return -ENAMETOOLONG;
    }

    real_root = realpath(root, NULL);
    if (!real_root) {
        snprintf(err, err_cap, "%s: %s", root, strerror(errno));
        return -errno;
    }
    real_entry = realpath(asked, NULL);
    if (!real_entry) {
        /* Also the answer for a dangling symlink, which is the right refusal:
         * we will not create what it points at. */
        rc = -errno;
        snprintf(err, err_cap, "%s: %s", asked, strerror(-rc));
        free(real_root);
        return rc;
    }

    /* The class entry is allowed to be a symlink -- in sysfs it always is --
     * but only within the boundary. Anywhere under /sys is legitimate when the
     * root is itself a sysfs class directory, because that is precisely where
     * /sys/class/hwmon/hwmonN points; for any other root (a test tree, a
     * bind-mount) the only legitimate target is the root itself. */
    if (!under(real_entry, real_root) && !(under(real_root, "/sys") && under(real_entry, "/sys"))) {
        snprintf(err, err_cap,
                 "%s resolves to %s, which is outside %s; refusing to follow it", asked, real_entry,
                 under(real_root, "/sys") ? "/sys" : real_root);
        rc = -ELOOP;
        goto out;
    }

    if (stat(real_entry, &st) != 0) {
        rc = -errno;
        snprintf(err, err_cap, "%s: %s", real_entry, strerror(-rc));
        goto out;
    }
    if (!S_ISDIR(st.st_mode)) {
        snprintf(err, err_cap, "%s is not a directory", real_entry);
        rc = -ENOTDIR;
        goto out;
    }

    if (snprintf(out, out_cap, "%s", real_entry) >= (int)out_cap) {
        snprintf(err, err_cap, "resolved path is too long");
        rc = -ENAMETOOLONG;
    }

out:
    free(real_root);
    free(real_entry);
    return rc;
}

int path_guard_open_leaf(const char *dir, const char *leaf, int flags, char *err, size_t err_cap)
{
    char path[PATH_MAX];
    struct stat st;
    int fd;

    if (snprintf(path, sizeof(path), "%s/%s", dir, leaf) >= (int)sizeof(path)) {
        snprintf(err, err_cap, "path under %s is too long", dir);
        return -ENAMETOOLONG;
    }

    /* O_NOFOLLOW makes the kernel, not us, decide: a symlink here fails with
     * ELOOP before anything is read or written. There is no window between a
     * check and the open in which the link could be swapped in. */
    fd = open(path, flags | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        int rc = -errno;
        if (rc == -ELOOP)
            snprintf(err, err_cap, "%s is a symbolic link; sysfs attributes never are, refusing",
                     path);
        else
            snprintf(err, err_cap, "%s: %s", path, strerror(-rc));
        return rc;
    }
    if (fstat(fd, &st) != 0) {
        int rc = -errno;
        snprintf(err, err_cap, "%s: %s", path, strerror(-rc));
        close(fd);
        return rc;
    }
    if (!S_ISREG(st.st_mode)) {
        snprintf(err, err_cap, "%s is not a regular file; sysfs attributes are, refusing", path);
        close(fd);
        return -EINVAL;
    }
    return fd;
}

int path_guard_open_chardev(const char *path, int flags, char *err, size_t err_cap)
{
    struct stat st;
    int fd;

    fd = open(path, flags | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        int rc = -errno;
        if (rc == -ELOOP)
            snprintf(err, err_cap, "%s is a symbolic link; a device node is not, refusing", path);
        else
            snprintf(err, err_cap, "open %s: %s", path, strerror(-rc));
        return rc;
    }
    if (fstat(fd, &st) != 0) {
        int rc = -errno;
        snprintf(err, err_cap, "%s: %s", path, strerror(-rc));
        close(fd);
        return rc;
    }
    if (!S_ISCHR(st.st_mode)) {
        snprintf(err, err_cap, "%s is not a character device, refusing", path);
        close(fd);
        return -EINVAL;
    }
    return fd;
}
