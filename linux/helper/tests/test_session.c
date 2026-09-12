/* Session binding: Enable(true) belongs to the session that asked for it.
 *
 * logind cannot run here (no systemd as pid 1, so sd_pid_get_session() has no
 * session for any pid), which is exactly why the lookup is an interface. What
 * is under test is session_owner_check(), the function the daemon's
 * authorize_owner() calls on every Enable and SetRules, driven with two fake
 * sessions in place of two seats.
 */
#include "session.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void ok(const char *name) { printf("  %-52s ok\n", name); }

static void fail(const char *name, const char *why)
{
    printf("  FAIL %s: %s\n", name, why);
    failures++;
}

/* Two seats. pid 100 is on session "c1", pid 200 on "c2", pid 300 is on no
 * session at all -- a system service, or this container. */
#define PID_A 100
#define PID_B 200
#define PID_NONE 300
#define UID_A 1000
#define UID_B 1001

int main(void)
{
    session_lookup *lk = session_lookup_fake();
    session_owner owner;
    char err[512];
    char id[SESSION_ID_MAX];

    session_fake_add(lk, PID_A, "c1");
    session_fake_add(lk, PID_B, "c2");

    printf("session lookup\n");
    session_identify(lk, PID_A, id, sizeof(id));
    if (strcmp(id, "c1") == 0)
        ok("a pid in a session resolves to its session id");
    else
        fail("a pid in a session resolves to its session id", id);

    session_identify(lk, PID_NONE, id, sizeof(id));
    if (id[0] == '\0')
        ok("a pid outside any session resolves to nothing");
    else
        fail("a pid outside any session resolves to nothing", id);

    printf("\nownership: Enable(true) binds the relay to the caller's session\n");

    session_owner_clear(&owner);
    if (session_owner_check(&owner, lk, PID_B, UID_B, err, sizeof(err)) == 0)
        ok("nothing bound: any caller may enable");
    else
        fail("nothing bound: any caller may enable", err);

    /* Session c1 enables. */
    session_identify(lk, PID_A, id, sizeof(id));
    session_owner_bind(&owner, id, UID_A, PID_A);

    if (session_owner_check(&owner, lk, PID_A, UID_A, err, sizeof(err)) == 0)
        ok("the owning session may disable and set rules");
    else
        fail("the owning session may disable and set rules", err);

    if (session_owner_check(&owner, lk, PID_B, UID_B, err, sizeof(err)) == -EPERM)
        ok("a different session is refused");
    else
        fail("a different session is refused", "the call was allowed");

    /* The refusal must say enough for the hub to explain it; a bare EPERM
     * leaves the user staring at a switch that will not move. */
    if (strstr(err, "c1") && strstr(err, "c2"))
        ok("the refusal names both sessions");
    else
        fail("the refusal names both sessions", err);

    if (session_owner_check(&owner, lk, PID_B, 0, err, sizeof(err)) == 0)
        ok("uid 0 is exempt: root can stop the unit anyway");
    else
        fail("uid 0 is exempt: root can stop the unit anyway", err);

    if (session_owner_check(&owner, lk, PID_NONE, UID_B, err, sizeof(err)) == -EPERM)
        ok("a caller outside any session is refused");
    else
        fail("a caller outside any session is refused", "the call was allowed");

    /* Enable(false) clears the binding, so the next session may take it. */
    session_owner_clear(&owner);
    if (session_owner_check(&owner, lk, PID_B, UID_B, err, sizeof(err)) == 0)
        ok("after release the binding is gone");
    else
        fail("after release the binding is gone", err);

    printf("\nownership when the owner had no logind session (no-systemd case)\n");
    session_owner_bind(&owner, "", UID_A, PID_NONE);
    if (session_owner_check(&owner, lk, PID_NONE, UID_A, err, sizeof(err)) == 0)
        ok("the same sessionless uid may change it");
    else
        fail("the same sessionless uid may change it", err);

    if (session_owner_check(&owner, lk, PID_B, UID_B, err, sizeof(err)) == -EPERM)
        ok("a different uid is still refused");
    else
        fail("a different uid is still refused", "the call was allowed");

    /* A sessionless owner must not become a wildcard that any session on the
     * machine inherits merely by sharing the uid the daemon happened to see. */
    if (session_owner_check(&owner, lk, PID_A, UID_A, err, sizeof(err)) == -EPERM)
        ok("a session-bound caller does not inherit a sessionless binding");
    else
        fail("a session-bound caller does not inherit a sessionless binding", "allowed");

    session_lookup_free(lk);

    printf("\nthe real lookup is compiled in and is the daemon's default\n");
    {
        session_lookup *real = session_lookup_logind();
        char buf[SESSION_ID_MAX];
        int r;

        if (real && real->session_of_pid && strstr(real->name, "sd_pid_get_session"))
            ok("session_lookup_logind() is sd_pid_get_session");
        else
            fail("session_lookup_logind() is sd_pid_get_session", real ? real->name : "NULL");

        /* It must be *called*, not merely linked. In this container there is
         * no logind, so it answers a negative errno rather than a session --
         * which is the honest result and is what the daemon then records as
         * "no session". */
        r = real->session_of_pid(real, getpid(), buf, sizeof(buf));
        printf("  sd_pid_get_session(%ld) -> %d (%s)\n", (long)getpid(), r,
               r < 0 ? strerror(-r) : buf);
    }

    printf("\n%s\n", failures ? "FAILURES" : "all session tests passed");
    return failures ? 1 : 0;
}
