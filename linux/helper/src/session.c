#include "session.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-login.h>

/* --- the real lookup ------------------------------------------------------ */

static int logind_session_of_pid(session_lookup *self, pid_t pid, char *buf, size_t cap)
{
    char *s = NULL;
    int r;

    (void)self;
    r = sd_pid_get_session(pid, &s);
    if (r < 0)
        return r;
    snprintf(buf, cap, "%s", s);
    free(s);
    return 0;
}

static session_lookup g_logind = {
    .name = "logind (sd_pid_get_session)",
    .session_of_pid = logind_session_of_pid,
    .priv = NULL,
};

session_lookup *session_lookup_logind(void) { return &g_logind; }

/* --- the fake ------------------------------------------------------------- */

#define FAKE_MAX 16

typedef struct {
    pid_t pid;   /* matched when by_uid is false */
    uid_t uid;   /* matched when by_uid is true */
    bool by_uid;
    char session[SESSION_ID_MAX];
} fake_entry;

typedef struct {
    fake_entry e[FAKE_MAX];
    size_t n;
} fake_priv;

/* -1 if it cannot be determined, which the caller treats as "no match". */
static long uid_of_pid(pid_t pid)
{
    char path[64], line[256];
    FILE *f;
    long uid = -1;

    snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
    f = fopen(path, "re");
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Uid:", 4) == 0) {
            /* "Uid:\treal\teffective\tsaved\tfs"; the effective one is what
             * the bus daemon reports as the caller's identity. */
            long real = -1, eff = -1;
            if (sscanf(line + 4, "%ld %ld", &real, &eff) == 2)
                uid = eff;
            break;
        }
    }
    fclose(f);
    return uid;
}

static int fake_session_of_pid(session_lookup *self, pid_t pid, char *buf, size_t cap)
{
    fake_priv *p = self->priv;
    long uid = -1;

    for (size_t i = 0; i < p->n; i++) {
        if (p->e[i].by_uid)
            continue;
        if (p->e[i].pid == pid) {
            snprintf(buf, cap, "%s", p->e[i].session);
            return 0;
        }
    }
    for (size_t i = 0; i < p->n; i++) {
        if (!p->e[i].by_uid)
            continue;
        if (uid < 0)
            uid = uid_of_pid(pid);
        if (uid >= 0 && (uid_t)uid == p->e[i].uid) {
            snprintf(buf, cap, "%s", p->e[i].session);
            return 0;
        }
    }
    return -ENXIO;
}

session_lookup *session_lookup_fake(void)
{
    session_lookup *lk = calloc(1, sizeof(*lk));
    if (!lk)
        return NULL;
    lk->priv = calloc(1, sizeof(fake_priv));
    if (!lk->priv) {
        free(lk);
        return NULL;
    }
    lk->name = "fake (test sessions)";
    lk->session_of_pid = fake_session_of_pid;
    return lk;
}

void session_fake_add(session_lookup *lk, pid_t pid, const char *session)
{
    fake_priv *p;
    if (!lk || lk->session_of_pid != fake_session_of_pid)
        return;
    p = lk->priv;
    if (p->n >= FAKE_MAX)
        return;
    p->e[p->n].pid = pid;
    snprintf(p->e[p->n].session, sizeof(p->e[p->n].session), "%s", session);
    p->n++;
}

void session_fake_add_uid(session_lookup *lk, uid_t uid, const char *session)
{
    fake_priv *p;
    if (!lk || lk->session_of_pid != fake_session_of_pid)
        return;
    p = lk->priv;
    if (p->n >= FAKE_MAX)
        return;
    p->e[p->n].by_uid = true;
    p->e[p->n].uid = uid;
    snprintf(p->e[p->n].session, sizeof(p->e[p->n].session), "%s", session);
    p->n++;
}

void session_lookup_free(session_lookup *lk)
{
    if (!lk || lk->session_of_pid != fake_session_of_pid)
        return; /* the logind lookup is static */
    free(lk->priv);
    free(lk);
}

/* --- ownership ------------------------------------------------------------ */

void session_owner_clear(session_owner *o) { memset(o, 0, sizeof(*o)); }

void session_owner_bind(session_owner *o, const char *session, uid_t uid, pid_t pid)
{
    memset(o, 0, sizeof(*o));
    o->bound = true;
    o->uid = uid;
    o->pid = pid;
    snprintf(o->session, sizeof(o->session), "%s", session ? session : "");
}

void session_identify(session_lookup *lk, pid_t pid, char *buf, size_t cap)
{
    buf[0] = '\0';
    if (!lk || !lk->session_of_pid)
        return;
    if (lk->session_of_pid(lk, pid, buf, cap) < 0)
        buf[0] = '\0';
}

int session_owner_check(const session_owner *o, session_lookup *lk, pid_t caller_pid,
                        uid_t caller_uid, char *err, size_t err_cap)
{
    char caller[SESSION_ID_MAX];

    if (!o->bound)
        return 0;
    if (caller_uid == 0)
        return 0; /* root can stop the unit anyway; see session.h */

    session_identify(lk, caller_pid, caller, sizeof(caller));

    if (o->session[0] != '\0') {
        if (strcmp(o->session, caller) == 0)
            return 0;
        snprintf(err, err_cap,
                 "the relay was enabled by session %s (uid %u) and only that session or an "
                 "administrator may change it; this call came from session %s (uid %u)",
                 o->session, (unsigned)o->uid, caller[0] ? caller : "(none)",
                 (unsigned)caller_uid);
        return -EPERM;
    }

    /* The owner had no logind session at all. Fall back to the uid, so a
     * sessionless owner is still not interchangeable with every other caller
     * on the machine. */
    if (caller[0] == '\0' && caller_uid == o->uid)
        return 0;
    snprintf(err, err_cap,
             "the relay was enabled by a caller outside any login session (uid %u); this call "
             "came from session %s (uid %u)",
             (unsigned)o->uid, caller[0] ? caller : "(none)", (unsigned)caller_uid);
    return -EPERM;
}
