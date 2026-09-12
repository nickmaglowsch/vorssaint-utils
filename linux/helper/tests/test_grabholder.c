/* Naming the process that holds a device open.
 *
 * When EVIOCGRAB is refused the user has to decide what to stop, and
 * "Device or resource busy" does not tell them. This runs against the real
 * /proc: the test opens a file, forks a child that also holds it open, and
 * asks who is holding it. There is no /dev/input here, so the file stands in
 * for the device node -- the walker does not care what kind of file it is,
 * which is the point of testing it this way rather than against a fake.
 */
#include "grabholder.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

static void ok(const char *name) { printf("  %-52s ok\n", name); }
static void fail(const char *name, const char *why)
{
    printf("  FAIL %s: %s\n", name, why);
    failures++;
}

int main(void)
{
    char path[] = "/tmp/vorssaint-grabholder-XXXXXX";
    char buf[512];
    int fd, n;
    pid_t child;

    fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        return 1;
    }

    printf("nobody else has it open\n");
    n = grab_holder_describe(path, buf, sizeof(buf));
    if (n == 0 && buf[0] == '\0')
        ok("our own descriptor is not reported as a holder");
    else {
        char why[600];
        snprintf(why, sizeof(why), "found %d: %s", n, buf);
        fail("our own descriptor is not reported as a holder", why);
    }

    printf("\nanother process has it open\n");
    child = fork();
    if (child == 0) {
        /* A child that holds the file open and then sleeps, standing in for
         * keyd holding /dev/input/event3. */
        int cfd = open(path, O_RDONLY);
        if (cfd < 0)
            _exit(1);
        pause();
        _exit(0);
    }
    if (child < 0) {
        perror("fork");
        return 1;
    }
    /* Wait for the child to have the file open. Polling for the answer beats
     * sleeping a fixed time, which is flaky on a loaded machine. */
    for (int i = 0; i < 200; i++) {
        n = grab_holder_describe(path, buf, sizeof(buf));
        if (n > 0)
            break;
        usleep(10000);
    }

    printf("  grab_holder_describe -> %d holder(s): %s\n", n, buf);
    if (n >= 1)
        ok("the holding process is found");
    else
        fail("the holding process is found", "none found");

    {
        char want[64];
        snprintf(want, sizeof(want), "(pid %ld)", (long)child);
        if (strstr(buf, want))
            ok("the message carries the holder's pid");
        else
            fail("the message carries the holder's pid", buf);
    }
    /* The process *name* is what makes the message actionable: "keyd" tells
     * the user what to stop, a pid does not. This binary's comm is
     * test_grabholder, truncated to 15 characters by the kernel. */
    if (strstr(buf, "test_grabholde"))
        ok("the message carries the holder's process name");
    else
        fail("the message carries the holder's process name", buf);

    kill(child, SIGTERM);
    waitpid(child, NULL, 0);

    printf("\nafter the holder exits\n");
    for (int i = 0; i < 200; i++) {
        n = grab_holder_describe(path, buf, sizeof(buf));
        if (n == 0)
            break;
        usleep(10000);
    }
    if (n == 0)
        ok("the holder is gone from the answer");
    else
        fail("the holder is gone from the answer", buf);

    printf("\na node nobody has open\n");
    n = grab_holder_describe("/dev/does-not-exist", buf, sizeof(buf));
    if (n == 0 && buf[0] == '\0')
        ok("an unknown node yields no holders and an empty string");
    else
        fail("an unknown node yields no holders and an empty string", buf);

    close(fd);
    unlink(path);

    printf("\n%s\n", failures ? "FAILURES" : "all grab-holder tests passed");
    return failures ? 1 : 0;
}
