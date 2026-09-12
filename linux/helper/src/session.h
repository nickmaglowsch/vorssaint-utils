/* Session binding for the helper's privileged methods.
 *
 * PLAN.md § 4.4 gate decision: `Enable(true)` binds the relay to the logind
 * session of the caller, and only that session — or an administrator — may
 * turn it off again or reprogram the rules. Without this a second process on
 * a *different* seat can disable a relay it did not enable, or point its
 * rules somewhere else, purely by being authorised for the action.
 *
 * The lookup is behind an interface because logind cannot be exercised
 * everywhere the helper is developed: the container this was written in has
 * no systemd as pid 1, so `sd_pid_get_session` cannot return a session for
 * any pid. The logind implementation is the daemon's default and is compiled
 * on every build; the fake exists for the tests, which drive two distinct
 * sessions through the same ownership function the daemon calls.
 */
#ifndef VORSSAINT_SESSION_H
#define VORSSAINT_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define SESSION_ID_MAX 64

typedef struct session_lookup session_lookup;

struct session_lookup {
    const char *name;
    /* Writes the caller's logind session id into buf. Returns 0 on success,
     * -errno otherwise (notably -ENXIO where the pid belongs to no session,
     * which is what a system service or a container without logind gives). */
    int (*session_of_pid)(session_lookup *self, pid_t pid, char *buf, size_t cap);
    void *priv;
};

/* The real one: sd_pid_get_session(3). Always compiled. */
session_lookup *session_lookup_logind(void);

/* The test one. Sessions are declared up front; an undeclared pid answers
 * -ENXIO exactly as a pid outside any session does under logind. */
session_lookup *session_lookup_fake(void);
void session_fake_add(session_lookup *lk, pid_t pid, const char *session);

/* The same fake, keyed on the caller's uid instead of its pid: the pid's uid
 * is read from /proc/<pid>/status and matched. The bus harness needs this
 * because it cannot know a client's pid before starting it, and two
 * unprivileged users are the closest thing to two seats a container can
 * offer. Under logind a session is per-login, not per-uid, so this is a test
 * double and nothing more -- the daemon's default is always the real
 * sd_pid_get_session. */
void session_fake_add_uid(session_lookup *lk, uid_t uid, const char *session);
void session_lookup_free(session_lookup *lk);

/* Who, if anyone, currently owns the relay. */
typedef struct {
    bool bound;
    char session[SESSION_ID_MAX]; /* empty when the owner had no session */
    uid_t uid;
    pid_t pid;
} session_owner;

void session_owner_clear(session_owner *o);
void session_owner_bind(session_owner *o, const char *session, uid_t uid, pid_t pid);

/* Ownership verdict for a caller. Returns
 *   0       the caller may act (nothing is bound, it owns the binding, or
 *           it is uid 0)
 *   -EPERM  another session owns it; err receives a message naming both
 *
 * uid 0 is exempt deliberately: root can stop the unit, so refusing it here
 * would buy nothing and would lock an administrator out of a relay left
 * enabled by a session that has since gone away.
 */
int session_owner_check(const session_owner *o, session_lookup *lk, pid_t caller_pid,
                        uid_t caller_uid, char *err, size_t err_cap);

/* Resolve a caller's session for binding. Never fails: a caller outside any
 * session (no logind, a system service) binds with an empty session id, and
 * session_owner_check() then treats "no session" as its own distinct owner so
 * two sessionless callers are not silently the same principal unless they
 * share a uid. */
void session_identify(session_lookup *lk, pid_t pid, char *buf, size_t cap);

#endif /* VORSSAINT_SESSION_H */
