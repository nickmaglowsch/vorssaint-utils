# `SelectSources` with `types=WINDOW` is silently served as a whole-monitor cast

**Draft for upstream (`xdg-desktop-portal-wlr`). Not submitted.** Posting to
another project's tracker is the repository owner's call; this file is the text
ready to go.

## Affected version

- `xdg-desktop-portal-wlr` **0.7.1**, as packaged in Ubuntu 24.04
  (`0.7.1-1build2`).
- Upstream source: `src/screencast/screencast.c:354` and
  `include/screencast_common.h:23-26`.

## Summary

The guard that is meant to reject non-monitor source types tests the wrong bit.
`enum source_types` holds bit *values*, not bit *indices*, so `1<<WINDOW` is
`1<<2 == 4` — the VIRTUAL bit — and a `types=WINDOW` request (value `2`) passes
straight through. The portal then hands back a full-monitor stream. A client
that asked to share one window silently shares the entire output.

```c
/* include/screencast_common.h:23 */
enum source_types {
  MONITOR = 1,
  WINDOW = 2,
};

/* src/screencast/screencast.c:352 */
uint32_t mask;
sd_bus_message_read(msg, "v", "u", &mask);
if (mask & (1<<WINDOW)) {                       /* == mask & 4 */
        logprint(INFO, "dbus: non-monitor cast requested, not replying");
        return -1;
}
```

## Reproduction

Needs only sway headless (same stack as the DMA-BUF report: `WLR_BACKENDS=headless
WLR_RENDERER=pixman`, xdpw with `chooser_type=none` and `output_name=HEADLESS-1`,
plus `xdg-desktop-portal`). With any ScreenCast client, call `CreateSession`
then `SelectSources` with `types` set to 2, and then `Start`.

## Expected

Either `SelectSources` is refused, or `AvailableSourceTypes` is honoured. The
backend advertises monitor-only:

```
org.freedesktop.portal.ScreenCast AvailableSourceTypes = 1   # MONITOR only
```

so a WINDOW request should not succeed.

## Actual

It succeeds, and returns a MONITOR stream:

```
SelectSources_response=0  types_requested=2
Start_results={'streams': <[(uint32 45, {'position': <(0, 0)>,
                                         'size': <(1280, 720)>,
                                         'source_type': <uint32 1>})]>, ...}

# xdpw log — the guard never fired
dbus: option types:2
```

`types=3` (MONITOR|WINDOW) behaves identically.

Asking for the bit the code actually tests confirms the diagnosis — `types=4`
(VIRTUAL) does trip the guard:

```
SelectSources_response=2  types_requested=4

# xdpw log
dbus: non-monitor cast requested, not replying
```

So `types=4` is rejected and `types=2` is not, which is exactly inverted from
the intent.

## Proposed fix

Compare against the enum values directly, and reject anything outside what the
backend advertises, rather than testing a single hard-coded bit:

```diff
--- a/src/screencast/screencast.c
+++ b/src/screencast/screencast.c
@@ -351,7 +351,7 @@
 			uint32_t mask;
 			sd_bus_message_read(msg, "v", "u", &mask);
-			if (mask & (1<<WINDOW)) {
+			if (mask & ~((uint32_t)MONITOR)) {
 				logprint(INFO, "dbus: non-monitor cast requested, not replying");
 				return -1;
 			}
```

That rejects WINDOW, VIRTUAL and any future type, and keeps working if
`AvailableSourceTypes` later grows.

Two smaller points worth fixing alongside:

- Returning `-1` without a reply makes the frontend synthesise a generic error
  (response 2) instead of the "cancelled" response 1 the spec expects for a
  refused selection. Sending a proper response would give clients a clearer
  signal.
- The enumerators would be less error-prone as `MONITOR = 1<<0, WINDOW = 1<<1,
  VIRTUAL = 1<<2`, or as indices used consistently with `1<<`.

## Impact

This is a privacy issue rather than a cosmetic one. A user who picks "share this
window" in an application that trusts `SelectSources` succeeding will share
their whole screen, including anything else visible on it, with no indication
that the request was downgraded. Clients can defend themselves by reading
`AvailableSourceTypes` first and checking each returned stream's `source_type`,
but they should not have to.
