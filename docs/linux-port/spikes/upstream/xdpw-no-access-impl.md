# The Screenshot portal is never exported on a wlroots session (no `impl.portal.Access`)

**Draft for upstream (`xdg-desktop-portal-wlr`). Not submitted.** Posting to
another project's tracker is the repository owner's call; this file is the text
ready to go.

This is a **documentation / expectation** issue rather than a code defect:
xdg-desktop-portal-wlr implements `org.freedesktop.impl.portal.Screenshot`
correctly, but on a session where it is the only backend the Screenshot portal
is not reachable by applications at all, and nothing says so.

## Affected version

- `xdg-desktop-portal-wlr` **0.7.1**, as packaged in Ubuntu 24.04
  (`0.7.1-1build2`), with `xdg-desktop-portal` **1.18.4**.
- Relevant file: the installed `wlr.portal`, which declares
  `Interfaces=org.freedesktop.impl.portal.Screenshot;org.freedesktop.impl.portal.ScreenCast;`.

## Summary

`xdg-desktop-portal`'s Screenshot frontend performs its own consent step
("Allow Applications to Take Screenshots?") through
`org.freedesktop.impl.portal.Access`. No wlroots backend implements `Access`, so
the frontend cannot build the Screenshot skeleton and drops the portal
entirely. `ScreenCast` is unaffected because its frontend delegates the whole
consent UI to the backend's own `SelectSources` and never needs `Access`.

The same gate removes `Camera`, `Device`, `Location` and `WebExtensions`.

## Reproduction

Needs only sway headless (same stack as the other two reports:
`WLR_BACKENDS=headless WLR_RENDERER=pixman`, xdpw with `chooser_type=none`,
plus `xdg-desktop-portal`).

```sh
gdbus introspect --session -d org.freedesktop.portal.Desktop \
  -o /org/freedesktop/portal/desktop | grep '^  interface org.freedesktop.portal'
```

## Expected

`org.freedesktop.portal.Screenshot` is present, since `wlr.portal` declares the
corresponding impl interface and xdpw exports it.

## Actual

It is absent:

```
  interface org.freedesktop.portal.Trash {
  interface org.freedesktop.portal.MemoryMonitor {
  interface org.freedesktop.portal.GameMode {
  interface org.freedesktop.portal.ProxyResolver {
  interface org.freedesktop.portal.NetworkMonitor {
  interface org.freedesktop.portal.PowerProfileMonitor {
  interface org.freedesktop.portal.ScreenCast {
  interface org.freedesktop.portal.Realtime {
```

```
$ gdbus call --session -d org.freedesktop.portal.Desktop \
    -o /org/freedesktop/portal/desktop \
    -m org.freedesktop.DBus.Properties.Get \
    org.freedesktop.portal.Screenshot version
Error: GDBus.Error:org.freedesktop.DBus.Error.InvalidArgs:
No such interface "org.freedesktop.portal.Screenshot"
```

The backend *is* found and *is* offering the interface; only the frontend
skeleton is missing:

```
$ G_MESSAGES_DEBUG=all /usr/libexec/xdg-desktop-portal -v
XDP: loading /usr/share/xdg-desktop-portal/portals/wlr.portal
XDP: portal implementation supports org.freedesktop.impl.portal.Screenshot
XDP: portal implementation supports org.freedesktop.impl.portal.ScreenCast
...
XDP: providing portal org.freedesktop.portal.Realtime
xdg-desktop-portal-WARNING **: No skeleton to export
XDP: Found 'wlr' in configuration for default
XDP: Using wlr.portal for org.freedesktop.impl.portal.ScreenCast (config)
XDP: providing portal org.freedesktop.portal.ScreenCast
```

and `gdbus introspect` on the backend confirms it:

```
interface org.freedesktop.impl.portal.ScreenCast {
  readonly u AvailableSourceTypes = 1;
  readonly u AvailableCursorModes = 3;
  readonly u version = 4;
interface org.freedesktop.impl.portal.Screenshot {
  readonly u version = 1;
```

## Isolating the cause

We replaced only the Screenshot backend with a stand-in that serves real pixels
via `grim` (i.e. through `zwlr_screencopy_v1`, the same protocol xdpw uses) and
whose advertised `version` and interface set are controllable. Three arms, one
variable each, everything else identical:

| Arm | stand-in exports | `org.freedesktop.portal.Screenshot` on the frontend |
|---|---|---|
| A | `impl.portal.Screenshot` version **1** | absent |
| B | `impl.portal.Screenshot` version **2** | absent |
| C | `impl.portal.Screenshot` v2 **+ `impl.portal.Access`** | **present, version 2** |

Arm C:

```
XDP: Using wp02mock.portal for org.freedesktop.impl.portal.Screenshot in sway (fallback)
XDP: providing portal org.freedesktop.portal.Screenshot

$ gdbus call ... org.freedesktop.portal.Screenshot version
(<uint32 2>,)
```

So the advertised impl version is irrelevant; the missing `Access` backend is
the cause. With it present, Screenshot works end to end — response 0,
37–58 ms per call, and the backend receives `interactive=False` verbatim:

```
Screenshot handle=/org/freedesktop/portal/desktop/request/1_57/shot_19359_1
  app_id='' interactive=False
  options={'interactive': False, 'permission_store_checked': True}
```

Arm C also brought back `Camera`, `Device`, `Location` and `WebExtensions`,
which are absent in arms A and B.

## Proposed fix

Any one of these would help; the first is the smallest.

1. **Document it.** State in the README and in `xdg-desktop-portal-wlr(5)` that
   the Screenshot portal requires a separate `org.freedesktop.impl.portal.Access`
   backend on the session, and name the usual candidates. Today a user who
   installs `xdg-desktop-portal-wlr` reasonably expects the Screenshot portal to
   work, and gets a portal that silently does not exist.
2. **Implement a minimal `Access`** in xdpw, with the same
   `chooser_cmd`/`chooser_type` treatment the screencast chooser already has, so
   a session can configure how consent is asked for (or auto-approve in
   unattended setups).
3. **Warn loudly.** Have `xdg-desktop-portal` log which impl interface was
   missing rather than the current bare `No skeleton to export`, so the cause is
   discoverable without instrumenting the frontend.

## Impact

Applications that use the Screenshot portal on a bare wlroots session get
`org.freedesktop.DBus.Error.InvalidArgs: No such interface`, with no hint that
the fix is to install an unrelated portal backend. Clients should probe for the
interface rather than assume the portal spec's interfaces are all present, but
the current behaviour is very hard to diagnose from the outside.
