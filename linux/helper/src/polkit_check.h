/* Authorisation gate for the helper's privileged D-Bus methods.
 *
 * The real implementation calls polkit_authority_check_authorization_sync()
 * against the calling process's D-Bus name, which is the only identity the
 * helper can trust: it is asserted by the bus daemon, not by the caller.
 *
 * WITH_POLKIT is off only for the private-bus harness in scripts/, where no
 * system polkit authority is running. The stub is loud about it, and the
 * helper refuses to start with --allow-unauthorized unless it was built
 * without polkit, so a production build cannot fall back to it. */
#ifndef VORSSAINT_POLKIT_CHECK_H
#define VORSSAINT_POLKIT_CHECK_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    AUTHZ_ALLOWED = 0,
    AUTHZ_DENIED = 1,
    AUTHZ_CHALLENGE = 2, /* polkit wants an interactive prompt we cannot show */
    AUTHZ_ERROR = 3
} authz_result;

/* action_id is one of the actions in data/org.vorssaint.helper.policy.
 * sender is the caller's unique bus name (":1.7"), taken from the message. */
authz_result polkit_check(const char *action_id, const char *sender, bool allow_interaction,
                          char *err, size_t err_cap);

const char *polkit_backend_name(void);
bool polkit_is_real(void);

/* Only honoured in a build without polkit; ignored otherwise. */
void polkit_set_stub_allow(bool allow);

#endif /* VORSSAINT_POLKIT_CHECK_H */
