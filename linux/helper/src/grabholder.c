#include "grabholder.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_HOLDERS 8

static char g_proc_root[256] = "/proc";

void grab_holder_set_proc_root(const char *root)
{
    snprintf(g_proc_root, sizeof(g_proc_root), "%s", root ? root : "/proc");
}

static int is_numeric(const char *s)
{
    if (!*s)
        return 0;
    for (; *s; s++)
        if (!isdigit((unsigned char)*s))
            return 0;
    return 1;
}

static void read_comm(const char *pid, char *out, size_t cap)
{
    char path[sizeof(g_proc_root) + 64];
    FILE *f;

    snprintf(out, cap, "?");
    snprintf(path, sizeof(path), "%s/%.20s/comm", g_proc_root, pid);
    f = fopen(path, "re");
    if (!f)
        return;
    if (fgets(out, (int)cap, f)) {
        size_t n = strlen(out);
        while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
            out[--n] = '\0';
    }
    fclose(f);
}

/* True if process <pid> has devnode open. Reading /proc/<pid>/fd of another
 * uid's process needs privilege; the helper is root, so the common case
 * succeeds, and a failure is simply a process we cannot see into. */
static int holds_node(const char *pid, const char *devnode)
{
    char dirpath[sizeof(g_proc_root) + 64];
    struct dirent *de;
    DIR *d;
    int found = 0;

    snprintf(dirpath, sizeof(dirpath), "%s/%.20s/fd", g_proc_root, pid);
    d = opendir(dirpath);
    if (!d)
        return 0;

    while (!found && (de = readdir(d)) != NULL) {
        char link[sizeof(dirpath) + 64];
        char target[512];
        ssize_t n;

        if (de->d_name[0] == '.')
            continue;
        snprintf(link, sizeof(link), "%s/%.20s", dirpath, de->d_name);
        n = readlink(link, target, sizeof(target) - 1);
        if (n < 0)
            continue;
        target[n] = '\0';
        if (strcmp(target, devnode) == 0)
            found = 1;
    }
    closedir(d);
    return found;
}

int grab_holder_describe(const char *devnode, char *buf, size_t cap)
{
    struct dirent *de;
    DIR *proc;
    int n_found = 0;
    size_t used = 0;
    char self[32];

    if (!buf || cap == 0)
        return -EINVAL;
    buf[0] = '\0';
    snprintf(self, sizeof(self), "%ld", (long)getpid());

    proc = opendir(g_proc_root);
    if (!proc)
        return -errno;

    while ((de = readdir(proc)) != NULL) {
        char comm[128];
        int w;

        if (!is_numeric(de->d_name))
            continue;
        /* Our own fds are not a useful answer: the caller already knows it
         * opened the node, that is why it tried to grab it. */
        if (strcmp(de->d_name, self) == 0)
            continue;
        if (!holds_node(de->d_name, devnode))
            continue;

        read_comm(de->d_name, comm, sizeof(comm));
        if (n_found < MAX_HOLDERS && used < cap) {
            w = snprintf(buf + used, cap - used, "%s%s (pid %s)", n_found ? ", " : "", comm,
                         de->d_name);
            if (w > 0 && (size_t)w < cap - used)
                used += (size_t)w;
        }
        n_found++;
    }
    closedir(proc);
    return n_found;
}
