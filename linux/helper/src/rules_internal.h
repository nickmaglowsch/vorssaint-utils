/* Shared plumbing between the rule files. Not part of the engine's API: the
 * daemon, the relay CLI and the tests all include rules.h only. */
#ifndef VORSSAINT_RULES_INTERNAL_H
#define VORSSAINT_RULES_INTERNAL_H

#include "rules.h"

#define MS_NS(ms) ((uint64_t)(ms) * 1000000ULL)

/* Append one event. Returns the new count, or -1 when out is full. Every
 * caller checks, because a truncated chord would leave a modifier down. */
int rules_emit(rules_event *out, int cap, int n, uint16_t type, uint16_t code, int32_t value);

/* Queue a notice for the Event signal. Drops (and counts) when the ring is
 * full rather than blocking the relay. */
void rules_notice_push(rules_state *st, uint64_t ts, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Press or release a modifier list, in order for a press and in reverse for a
 * release, which is what a keyboard actually does and what an application's
 * xkb state expects. */
int rules_emit_mods(rules_event *out, int cap, int n, const uint16_t *mods, unsigned n_mods,
                    bool down);

/* Each rule's entry point. Every one takes the already-emitted count and
 * returns the new one, or -1 on overflow. `handled` is set when the rule
 * consumed the event and no later rule (nor the pass-through) may see it. */
int rules_kdb_apply(rules_state *st, const rules_event *in, uint64_t now, rules_event *out, int cap,
                    int n, bool *handled);
int rules_mcd_apply(rules_state *st, const rules_event *in, uint64_t now, rules_event *out, int cap,
                    int n, bool *handled);
int rules_scroll_apply(rules_state *st, rules_event *in, uint64_t now, rules_event *out, int cap,
                       int n, bool *handled);
int rules_smooth_apply(rules_state *st, const rules_event *in, uint64_t now, rules_event *out,
                       int cap, int n, bool *handled);
int rules_sk_apply(rules_state *st, const rules_event *in, uint64_t now, rules_event *out, int cap,
                   int n, bool *handled);
int rules_mbs_apply(rules_state *st, const rules_event *in, uint64_t now, rules_event *out, int cap,
                    int n, bool *handled);
int rules_qp_apply(rules_state *st, const rules_event *in, uint64_t now, rules_event *out, int cap,
                   int n, bool *handled);

/* Timer halves, for the rules that have one. */
uint64_t rules_smooth_deadline(const rules_state *st);
int rules_smooth_tick(rules_state *st, uint64_t now, rules_event *out, int cap, int n);
uint64_t rules_qp_deadline(const rules_state *st);
int rules_qp_tick(rules_state *st, uint64_t now, rules_event *out, int cap, int n);

/* Release everything the engine is holding down on the user's behalf. */
int rules_release_held(rules_state *st, rules_event *out, int cap, int n);

/* True for the eight modifier keycodes the engine tracks. */
bool rules_is_modifier(uint16_t code);

#endif /* VORSSAINT_RULES_INTERNAL_H */
