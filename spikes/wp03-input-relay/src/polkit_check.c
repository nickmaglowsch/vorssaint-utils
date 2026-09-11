#include "polkit_check.h"

#include <stdio.h>
#include <string.h>

#ifdef WITH_POLKIT

#include <polkit/polkit.h>

const char *polkit_backend_name(void) { return "polkit (polkit_authority_check_authorization_sync)"; }
bool polkit_is_real(void) { return true; }
void polkit_set_stub_allow(bool allow) { (void)allow; }

authz_result polkit_check(const char *action_id, const char *sender, bool allow_interaction,
                          char *err, size_t err_cap)
{
    PolkitAuthority *authority;
    PolkitSubject *subject;
    PolkitAuthorizationResult *result;
    PolkitCheckAuthorizationFlags flags =
        allow_interaction ? POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION
                          : POLKIT_CHECK_AUTHORIZATION_FLAGS_NONE;
    GError *gerr = NULL;
    authz_result rc;

    authority = polkit_authority_get_sync(NULL, &gerr);
    if (!authority) {
        snprintf(err, err_cap, "polkit authority unavailable: %s",
                 gerr ? gerr->message : "unknown");
        if (gerr)
            g_error_free(gerr);
        return AUTHZ_ERROR;
    }

    /* The subject is named by its unique bus name. polkit resolves that to a
     * pid and uid through the bus daemon, so a caller cannot claim to be
     * another process: it never supplies its own pid. */
    subject = polkit_system_bus_name_new(sender);
    if (!subject) {
        snprintf(err, err_cap, "cannot build subject for sender '%s'", sender);
        g_object_unref(authority);
        return AUTHZ_ERROR;
    }

    result = polkit_authority_check_authorization_sync(authority, subject, action_id, NULL, flags,
                                                       NULL, &gerr);
    if (!result) {
        snprintf(err, err_cap, "check_authorization failed: %s", gerr ? gerr->message : "unknown");
        if (gerr)
            g_error_free(gerr);
        g_object_unref(subject);
        g_object_unref(authority);
        return AUTHZ_ERROR;
    }

    if (polkit_authorization_result_get_is_authorized(result))
        rc = AUTHZ_ALLOWED;
    else if (polkit_authorization_result_get_is_challenge(result))
        rc = AUTHZ_CHALLENGE;
    else
        rc = AUTHZ_DENIED;

    g_object_unref(result);
    g_object_unref(subject);
    g_object_unref(authority);
    return rc;
}

#else /* !WITH_POLKIT */

static bool stub_allow = true;

const char *polkit_backend_name(void) { return "STUB (built without polkit: -DWITH_POLKIT=OFF)"; }
bool polkit_is_real(void) { return false; }
void polkit_set_stub_allow(bool allow) { stub_allow = allow; }

authz_result polkit_check(const char *action_id, const char *sender, bool allow_interaction,
                          char *err, size_t err_cap)
{
    (void)allow_interaction;
    (void)err;
    (void)err_cap;
    fprintf(stderr,
            "helper: POLKIT STUB: would check action '%s' for sender '%s' -> %s\n",
            action_id, sender, stub_allow ? "allowed" : "denied");
    return stub_allow ? AUTHZ_ALLOWED : AUTHZ_DENIED;
}

#endif
