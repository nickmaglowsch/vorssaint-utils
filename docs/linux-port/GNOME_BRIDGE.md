# The GNOME bridge (WP-C2)

`linux/platform/window/gnome-extension/` is `vorssaint-bridge`, the GNOME
Shell extension that exports `org.vorssaint.WindowBridge` on the session bus.
On GNOME it is the whole window channel and the whole clipboard channel:
Mutter implements no foreign-toplevel protocol, no layer-shell and no
`ext-data-control`, so without the extension the switcher, window layout,
auto-quit and clipboard history have nothing to talk to and the app shows them
as unavailable (`PLAN.md` § 4.6).

The C side is `linux/platform/window/bridge_client.c`, written first by WP-C1;
this extension implements exactly the interface that client calls, and nothing
more. `SECURITY.md` inside the extension directory says what putting it on the
session bus exposes — read it before recommending the install.

## The interface

```
org.vorssaint.WindowBridge  at  /org/vorssaint/WindowBridge

  List()                      -> a(tsssiiiiiuis)
  Activate(t)  Close(t)  SetMinimized(t, b)
  MoveResize(t, i, i, i, i)   Geometry(t) -> (iiii)
  CurrentWorkspace() -> i     SetWorkspace(i)

  ClipboardMimeTypes(u) -> as
  ClipboardRead(u, s) -> ay   ClipboardWrite(u, s, ay)   ClipboardClear(u)

  WindowAdded(t)  WindowRemoved(t)  WindowActivated(t)  WindowChanged(t)
  WorkspaceChanged(i)  ClipboardChanged(u, as)

  property Version : u
```

The window struct is `id, app_id, app_name, title, pid, x, y, width, height,
flags, workspace, output`, with `flags` the `vs_window_flag` bitmask from
`linux/platform/include/vorssaint_platform.h`. The window half is exactly what
WP-C1 specified; **nothing in it needed changing to fit Mutter**, so there is
no deviation for the lead to approve (unlike KWin, which cannot own a bus name
at all — see `WINDOW_BACKENDS.md`).

Three things were *added*, none of which the C client sees as a change:

- the four clipboard members and `ClipboardChanged`, which WP-A8 needs and
  which have no other home on GNOME. `selection` is 0 for the clipboard and 1
  for the primary selection.
- the `Version` property, so the Capabilities page (WP-25) can tell an
  out-of-date installed copy from a current one without parsing files.
- `ClipboardRead` is the one asynchronous member: reading a selection is a
  transfer from the owning client, and blocking the Shell's main loop on
  another process is not acceptable.

## What Mutter can do that the other backends cannot

| | GNOME (this bridge) | KWin script | wlr protocols |
|---|---|---|---|
| Owns a D-Bus name itself | **yes** — GJS exports the object, so the extension *is* the service | no: `callDBus` calls out only, so the C side owns a sink and pushes one-shot scripts in | n/a |
| Real stacking order | **yes** — `global.get_window_actors()` is Mutter's stack, bottom to top | yes (`windowList()`) | no: announcement order only, marked unreliable |
| pid, geometry, workspace, monitor per window | yes, all four | yes | only via a compositor IPC (sway); the protocols carry none of it |
| Move/resize another client's window | yes (`move_resize_frame`) | yes | **impossible** in any Wayland protocol |
| Minimize | yes | yes | request exists, wlroots has no minimized state |
| Background clipboard read | yes, via `Meta.Selection` | `ext-data-control` (no extension needed) | `wlr-data-control` |
| Structured payloads | binary `a(tsssiiiiiuis)` | JSON strings: `callDBus` marshals only simple QVariants | n/a |

The stacking order is the one that matters most downstream: `SwitcherItem`'s
MRU on macOS comes from `CGWindowListCopyWindowInfo`'s front-to-back order, and
GNOME is one of only three backends that can supply an equivalent. WP-C3 can
use it directly here instead of falling back to its own history.

What Mutter **cannot** do, and how the bridge answers:

- **Tell you whether a move worked.** `move_resize_frame` returns nothing and
  Mutter applies constraints (minimum size, tiling, maximization) afterwards.
  `MoveResize` therefore reports no error, and only the client's `Geometry`
  read-back decides — which is exactly what `gnome_move_resize()` in
  `bridge_client.c` does, and what the fixture's "Text Editor refuses to
  resize" case tests.
- **Name a monitor.** `MetaMonitorManager` maps a connector to an index but
  offers no list and no reverse lookup in the introspection data (checked
  against Meta-14). The bridge asks Mutter's own
  `org.gnome.Mutter.DisplayConfig.GetCurrentState` for the connector names and
  maps each back through `get_monitor_for_connector`, so no ordering is
  assumed; until that answer arrives, and wherever the call fails, a monitor is
  `monitor-N`. Stable for the session either way, which is all display
  filtering needs.
- **Clear somebody else's clipboard.** `meta_selection_unset_owner` takes the
  source object, and there is no introspected way to fetch the current owner's.
  `ClipboardClear` therefore only drops ownership the bridge itself took. This
  is a limit, not a bug: the alternative would be a way for any session-bus
  caller to wipe any app's selection.
- **Activate without a timestamp.** A D-Bus call carries no input event, so
  `global.get_current_time()` is 0 and focus-stealing prevention would refuse
  the activation. The bridge falls back to
  `global.display.get_current_time_roundtrip()`, which is the documented way to
  activate a window from outside an event handler.

## Structure, and why

```
metadata.json      uuid vorssaint-bridge@vorssaint.com, shell-version 46..50
extension.js       Extension subclass: enable() wires three objects, disable()
                   gives every one of them back
lib/protocol.js    interface XML, flag bitmask, struct encoding — no GNOME
lib/throttle.js    leading edge + coalesced trailing edge, injected timer
lib/ids.js         the `t` window id and its reverse map
lib/service.js     owns the bus name, answers the interface, emits the signals
lib/mutter.js      the ONLY file that imports Meta/Shell/resource:///…
```

The split is what makes the extension testable at all on a machine with no
GNOME session: everything except `lib/mutter.js` and `extension.js` runs under
plain `gjs`, so the same `lib/service.js` that runs inside gnome-shell can own
the name in a test process and answer the real C client. There is no
`stylesheet.css` (the extension draws nothing) and no `schemas/` (it has no
settings — adding one would mean a GSettings surface to defend).

Teardown is the part reviewers should read: GNOME runs `disable()` whenever the
screen locks, and an extension that leaks there leaks once per lock.
`WindowBridgeService.stop()` cancels every armed throttle timer *first* (a
timeout firing into an unexported object crashes the Shell), drops the
listeners the collaborators hold on it, unowns the name, then unexports the
object; `MutterWindowSource.disable()` disconnects every per-window handler and
clears the id table, which is also what stops the extension holding `MetaWindow`
references after unload.

Throttling: `WindowChanged` is leading-edge plus one coalesced trailing edge per
window, 120 ms. Mutter emits `position-changed`/`size-changed` per frame of an
interactive drag; unthrottled that is a bus message per frame per window, which
is the classic way a bridge slows down the desktop it is watching. Added,
removed, activated and workspace changes are not throttled — they are rare and
the switcher wants them immediately.

## Compatibility: the risk PLAN.md § 9 names

`shell-version` is `["46", "47", "48", "49", "50"]`: 46 is the baseline
`PLAN.md` § 10 proposes, 49 and 50 are the two most recent majors, and 47/48 are
the releases in between. Only GNOME 45+ is possible at all — the ESM
`Extension` class landed there — and 46 is the oldest we claim.

The exposure is real: GNOME renames and removes introspected API between
majors, and an extension that calls a method that no longer exists fails at
runtime in front of the user. Two things reduce it here.

1. **All the Mutter surface is in one file.** `lib/mutter.js` is the only place
   to re-read on a version bump.
2. **`test/check_mutter_api.js` asserts every member exists**, against whatever
   Mutter and Shell introspection data is installed. It needs no session, so CI
   can run it on a container per GNOME version — a removed or renamed method
   fails a test rather than a user's desktop. That is the mechanism the CI
   requirement in the WP text ("against the two latest Shell versions") should
   be built on; the CI job itself is not written here, because this machine has
   exactly one GNOME version available.

Known version sensitivities, from reading the API rather than running it:

| Member | Risk |
|---|---|
| `global.backend.get_monitor_manager()` | `global.backend` is GNOME 45+; older code used `Meta.MonitorManager.get()` |
| `Meta.Selection.get_mimetypes()` | present in 46; the older `MetaSelection` API shape differed |
| `actor.meta_window` | still there in 46; the code already falls back to `get_meta_window()` |
| `meta_window_get_id()` | Mutter's own id; `lib/ids.js` allocates from a synthetic range if a build ever drops it |
| `Main.activateWindow` | stable for many releases; verified present in the GNOME 46 shell JS |

## Test evidence

Everything below was run on Ubuntu 24.04 in this container (gjs 1.80.2,
gnome-shell 46.2, mutter 46.2 introspection data). **GNOME Shell itself cannot
run here** — no DRM device, no seat — so no claim in this document is measured
on a live Mutter. What *is* real is that the extension's own D-Bus service code
ran in a real process, on a real bus, and answered the real C client.

```
$ ctest --test-dir build -R gnome --output-on-failure
    Start  6: window_gnome_fake           Passed
    Start  7: window_gnome_extension      Passed
    Start  8: gnome_extension_units       Passed
    Start  9: gnome_extension_api         Passed
    Start 10: gnome_extension_static      Passed
    Start 11: gnome_extension_clipboard   Passed
100% tests passed, 0 tests failed out of 6
```

- **`window_gnome_extension`** is WP-C1's own `test_gnome_fake.sh`, unchanged in
  its assertions, pointed at `gjs -m gnome-extension/test/run_bridge.js`
  instead of `fake_window_bridge.py`. The extension's `lib/service.js` and
  `lib/protocol.js` own the name and answer `vs-window` for real: the backend
  declines with no bridge on the bus, selects `gnome` when it is there,
  decodes `a(tsssiiiiiuis)`, activates, minimizes, moves with a read-back,
  catches the refused move, switches workspace, gets
  `org.freedesktop.DBus.Error.InvalidArgs` / "no such window" back for an
  unknown id, and receives all four signals.

  ```
  backend	gnome
  101	org.gnome.Nautilus	Home	6101	0,0 1280x800	on-current-workspace,has-geometry,has-pid,on-screen	0	XWAYLAND0	0
  102	firefox	Vorssaint on GNOME	6102	100,60 1600x900	focused,on-current-workspace,has-geometry,has-pid,on-screen	0	XWAYLAND0	1
  103	org.gnome.TextEditor	notes.md	6103	40,40 800x600	has-geometry,has-pid	1	XWAYLAND0	2
  event	added	104	-	-1
  event	activated	104	-	-1
  event	removed	104	-	-1
  event	workspace	0	-	1
  PASS: gnome (against the vorssaint-bridge extension under gjs; unverified on a live GNOME Shell)
  ```

  Only the compositor is substituted: `test/fixture_source.js` replaces Mutter
  with the same fixture the Python fake carries. Everything between the bus and
  that fixture is the code that ships.

- **`gnome_extension_units`** — 86 checks over the interface XML (parsed with
  `Gio.DBusNodeInfo` and compared member by member against the signatures
  `bridge_client.h` reads, including "no member beyond the interface WP-C1
  defined"), the flag rules, the struct encoding, the throttler under a fake
  clock, and the id allocator. Mutation-checked: relaxing "minimized clears
  ON_SCREEN" in `protocol.js` turns it red.

- **`gnome_extension_api`** — 60 Meta/Shell members, signals and properties
  that `lib/mutter.js` calls, asserted present in the installed introspection
  data (`Meta-14`, `Shell-14`). Mutation-checked: adding a
  `Meta.Display.get_unicorn()` to the list turns it red. This is the test a
  per-GNOME-version CI matrix should run.

- **`gnome_extension_static`** — every `.js` parses as an ES module (including
  `extension.js` and `lib/mutter.js`, which cannot be *executed* outside
  gnome-shell), the four Mutter-free modules load under `gjs`,
  `gnome-extensions pack --force --extra-source=lib` produces the install zip,
  and the installed code contains no `eval`, no `new Function` and no
  subprocess — the promise `SECURITY.md` makes, enforced mechanically.

- **`gnome_extension_clipboard`** — the clipboard members over a real session
  bus with `gdbus` as the client: introspection, the `Version` property, `ay`
  in and out, `as`, the asynchronous read path, an absent mime type answering
  with an error rather than empty data, `ClipboardChanged`, and the primary
  selection being a separate selection rather than an alias.

### What is *not* verified

- Nothing in `lib/mutter.js` has ever run. Its API surface is asserted to
  exist; its behaviour — that `move_resize_frame(true, …)` lands where asked,
  that `Main.activateWindow` with a roundtrip timestamp beats focus-stealing
  prevention, that `get_window_actors()` order is the stack in the presence of
  override-redirect windows, that `Meta.Selection` transfers really do work for
  a background reader on both Wayland and X11 — is read-only reasoning about
  Mutter's API and must be re-checked on a live session.
- The `output` connector lookup through `DisplayConfig` has never answered.
- `enable()`/`disable()` have never been called by a Shell. The teardown is
  argued, not observed; the lock-screen cycle is the thing to watch first.
- The extension has never been loaded by gnome-shell at all, so
  `shell-version` compatibility is a claim about the API, not an install.

**First things to check on a real GNOME session**, in order: that the
extension loads and takes the name (`gdbus introspect --session --dest
org.vorssaint.WindowBridge`); that lock/unlock twice leaves no duplicate
signal handlers and no warning in `journalctl -f -o cat /usr/bin/gnome-shell`;
that a window drag does not flood the bus (`dbus-monitor` while dragging);
that `MoveResize` lands on a maximized window after the unmaximize; and that
`ClipboardRead` works while another application holds the focus.

## Installing

```
$ linux/platform/window/gnome-extension/install.sh
$ linux/platform/window/gnome-extension/uninstall.sh
```

`install.sh` replaces `~/.local/share/gnome-shell/extensions/vorssaint-bridge@vorssaint.com/`
(replaces, not merges, so an upgrade cannot leave a stale file the Shell would
still load) and runs `gnome-extensions enable`. GNOME will not load a new
extension into a running Shell: on **X11** the user restarts it with Alt+F2, r;
on **Wayland** there is no Shell restart at all and the user must log out and
back in. Both scripts print the right one for `$XDG_SESSION_TYPE`. The
Capabilities page (WP-25) calls these two scripts and must show the same
sentence rather than implying the feature is live immediately.
