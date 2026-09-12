# Window backends (WP-C1)

What the five Linux window backends under `linux/platform/window` can do, how
each one does it, and which rows in the table were measured here rather than
designed. The C contract they all implement is
`linux/platform/include/vorssaint_platform.h`; `linux/platform/README.md`
explains how WP-12's Swift protocol mirrors it.

Backends are selected at runtime, in this order, by
`vs_window_system_create()`:

| Order | Backend | Chosen when |
|---|---|---|
| 1 | `hyprland` | `HYPRLAND_INSTANCE_SIGNATURE` is set and `$XDG_RUNTIME_DIR/hypr/<sig>/.socket.sock` answers `j/version` |
| 2 | `kwin` | `org.kde.KWin` has an owner on the session bus and `vorssaint-window.js` is installed |
| 3 | `gnome` | `org.vorssaint.WindowBridge` has an owner on the session bus (the WP-C2 Shell extension) |
| 4 | `wlr` | `WAYLAND_DISPLAY` is set and the compositor advertises `zwlr_foreign_toplevel_manager_v1` |
| 5 | `x11` | `DISPLAY` is set and the root window carries `_NET_SUPPORTING_WM_CHECK` |

Hyprland, KWin and GNOME come first deliberately: those sessions also set
`WAYLAND_DISPLAY` (and usually `DISPLAY`, through Xwayland), so a generic
backend would otherwise win a session that has a better channel. The order can
be overridden for testing with `--backend NAME` or `VS_WINDOW_BACKEND=NAME`.

## Capability matrix

`measured` means the row was produced by running the suite in this repository
on this machine; `designed` means it is written against the published protocol
and exercised only against a fake, because the compositor cannot run here.

| Capability | x11 | wlr (sway) | hyprland | kwin | gnome |
|---|---|---|---|---|---|
| `can_list` | ✓ measured | ✓ measured | ✓ designed | ✓ designed | ✓ designed |
| `can_activate` | ✓ measured | ✓ measured | ✓ designed | ✓ designed | ✓ designed |
| `can_close` | ✓ measured | ✓ measured | ✓ designed | ✓ designed | ✓ designed |
| `can_minimize` | ✓ measured | ◐ measured (advertised; sway ignores it, so the call returns `VS_ERR_NOT_APPLIED`) | ✗ by design | ✓ designed | ✓ designed |
| `can_move_resize` | ✓ measured | ✓ measured (sway IPC only) | ✓ designed | ✓ designed | ✓ designed |
| `can_workspace_switch` | ✓ measured | ✓ measured (sway IPC only) | ✓ designed | ✓ designed | ✓ designed |
| `has_live_events` | ✓ measured | ✓ measured | ✓ designed | ✓ designed | ✓ designed |
| `has_previews` | ✗ | ✗ | ✗ | ✗ | ✗ |

`has_previews` is reserved for WP-C3 (portal ScreenCast window streams,
`ext-image-copy-capture-v1`, XComposite); no backend sets it yet.

### What each backend reports per window

| Field | x11 | wlr (sway) | hyprland | kwin | gnome |
|---|---|---|---|---|---|
| `app_id` | `WM_CLASS` instance | Wayland `app_id` | `class` | `resourceClass` | extension's choice |
| `app_name` | `WM_CLASS` class | = `app_id` (protocol has no second name) | = `class` | `resourceName` | extension's choice |
| `title` | `_NET_WM_NAME`, else `WM_NAME` | `title` | `title` | `caption` | extension's choice |
| `pid` | `_NET_WM_PID` (absent on some clients) | sway IPC only | `pid` | `pid` | extension's choice |
| geometry | frame rect: client rect grown by `_NET_FRAME_EXTENTS` | sway IPC `rect` only | `at` + `size` | `frameGeometry` | `get_frame_rect` |
| workspace | `_NET_WM_DESKTOP` | sway IPC only | `workspace.id` | desktop index | workspace index |
| output | — (not read) | sway IPC `output`, else `output_enter` | `j/monitors` by index | `output.name` | monitor name |
| stacking | `_NET_CLIENT_LIST_STACKING`, marked **valid** | announcement order, marked **unreliable** | `j/clients` order, marked **unreliable** | `windowList()`, marked **valid** | extension's order, marked **valid** |

`stacking_valid` matters to WP-C3: the macOS switcher reads
`CGWindowListCopyWindowInfo` front-to-back and relies on that order. Two
backends cannot supply it, and say so instead of inventing one; on those the
switcher must fall back to its own MRU list.

## x11 — EWMH over libxcb

`libxcb` + `xcb-ewmh` + `xcb-icccm`. Lists `_NET_CLIENT_LIST_STACKING` (falling
back to `_NET_CLIENT_LIST`), filtered to `_NET_WM_WINDOW_TYPE_NORMAL` windows
that do not ask for `_NET_WM_STATE_SKIP_TASKBAR`. Activation is
`_NET_ACTIVE_WINDOW` with source indication 2 after an `XMapWindow`; closing is
`_NET_CLOSE_WINDOW`; minimizing is the ICCCM `WM_CHANGE_STATE` → `IconicState`
client message, and restoring is activation. Events come from
`PropertyChangeMask` on the root (client list, active window, current desktop)
and on each client window (`_NET_WM_NAME`, `_NET_WM_STATE`, `_NET_WM_DESKTOP`)
plus `StructureNotify`.

Geometry is **frame** geometry, matching what the macOS Accessibility API
reports: the client rectangle grown by `_NET_FRAME_EXTENTS`. `move_resize`
sends `_NET_MOVERESIZE_WINDOW` with gravity 0 and the *client* size, because
EWMH's x,y is the frame reference point while width,height is the client size.
An earlier attempt using `StaticGravity` and a client-relative origin landed
one pixel off under openbox; gravity 0 with a frame origin lands exactly.

**Measured** (Ubuntu 24.04, Xvfb 1280x800, openbox, xterm and xmessage):

```
$ ctest --test-dir build -R window_x11 --output-on-failure
backend	x11
can_list	yes
can_activate	yes
can_close	yes
can_minimize	yes
can_move_resize	yes
can_workspace_switch	yes
has_live_events	yes
has_previews	no

6291468	xterm	vorssaint-gamma	24727	397,229 486x341	on-current-workspace,has-geometry,has-pid,on-screen	0	-	0
4194316	xterm	vorssaint-beta	24726	0,459 486x341	on-current-workspace,has-geometry,has-pid,on-screen	0	-	1
8388620	xterm	vorssaint-alpha	24725	794,459 486x341	focused,on-current-workspace,has-geometry,has-pid,on-screen	0	-	2

moved to 120,90 640x480
xwininfo client rect: 121,110 638x455; extents left=1 right=1 top=20 bottom=5

event	added	14680076	vorssaint-delta	-1
event	activated	14680076	vorssaint-delta	-1
event	removed	14680076	-	-1
PASS: x11
```

The move is confirmed twice: once by the backend's own read-back and once by
`xwininfo` plus `xprop _NET_FRAME_EXTENTS`, which must agree exactly
(`120 + left`, `90 + top`, `640 - left - right`, `480 - top - bottom`).

The suite also asserts the *failure* path, because that is where a backend
starts lying: `xterm` quantises its size to whole character cells, so an
arbitrary frame is unreachable, and `move_resize` must answer
`VS_ERR_NOT_APPLIED` rather than success while still having moved the window's
origin.

Known gaps: `output` is not populated (no Xinerama/RandR query yet — WP-C3
needs it for per-display filtering), and `_NET_WM_PID` is absent on clients
that do not set it (`xmessage` is one), which is why `VS_WINDOW_HAS_PID` is a
flag rather than an assumption.

## wlr — `zwlr_foreign_toplevel_management_v1`, plus sway IPC

`zwlr_foreign_toplevel_management_v1` (vendored XML, interface version 3) is
the only Wayland protocol that lets an ordinary client list, activate,
minimize and close other clients' windows. `ext_foreign_toplevel_list_v1` is
bound when the compositor offers it, but it has *no* control verbs at all, so
it contributes only the stable per-toplevel identifier WP-C3 will key previews
on. It is read from the installed wayland-protocols (staging, 1.32+), not
vendored; sway 1.9 / wlroots 0.17 does not advertise it, so that path is
compiled but not exercised here.

Neither protocol reports geometry, pid or workspace, and **no Wayland protocol
lets one client move another client's window**. On sway the i3 IPC socket
(`SWAYSOCK`) supplies all four: `get_tree` for geometry, pid, workspace and
output, and `run_command` for placement. `can_move_resize` and
`can_workspace_switch` are therefore advertised only when `SWAYSOCK` is
present; on bare wlroots or another wlr-based compositor they are off and the
feature degrades instead of failing.

Toplevel handles carry no compositor id, so the bridge to the IPC tree is the
`(app_id, title)` pair, and an ambiguous pair gets no enrichment rather than
another window's geometry.

Two behaviours measured here bind the implementation:

- **`floating enable` must not share a command list with `resize set`.** sway
  commits a comma-separated list as one transaction and the float's own default
  size lands last: 800x600 requested, 700x500 applied. A criteria selector is
  also only accepted once per list, so repeating `[con_id=N]` is a parse error.
  The backend issues `floating enable` on its own and then re-issues
  `resize set` + `move position` until the window settles, the same shape as
  the macOS `WindowLayoutService.attempt(_:)` loop.
- **`close` needs a round trip, not a flush.** Flushing and disconnecting loses
  the close often enough to be seen in the suite.

`set_minimized` stays advertised because the protocol carries the request, but
sway acknowledges it and does nothing — wlroots has no minimized state. The
call therefore reads the compositor's own `state` event back and answers
`VS_ERR_NOT_APPLIED`, the same contract `move_resize` keeps, instead of
returning a success it cannot back up. The suite asserts that, and still lets a
future wlroots that grows a real minimized state pass:

```
minimize: sway acknowledged and ignored it, reported as not applied
```

**Globals going away.** `wl_output` removal (a monitor unplug) is handled: the
proxy is released, the entry drops out of the table, and any toplevel still
naming that output is cleared and re-announced as changed. The headless backend
can do this for real, so the suite exercises it rather than reasoning about it —
`swaymsg create_output`, move a window there, `swaymsg output HEADLESS-2
unplug`, then assert no window names the dead output, the window survives, and
the backend still answers. Withdrawal of the *manager* global (or its `finished`
event) drops the backend to listing only: `capabilities` shrinks to what still
works, `VS_WINDOW_EVENT_BACKEND_LOST` is queued, and placement stays if the sway
IPC socket is still there. **No compositor available here can withdraw a
global**, so that path is covered by a unit test of the shared decision
(`vs_window_capabilities_without_control`) plus the output-removal path that
exercises the same `global_remove` wiring; it is not end-to-end verified.

**Measured** (headless sway 1.9, `WLR_BACKENDS=headless WLR_RENDERER=pixman
WLR_LIBINPUT_NO_DEVICES=1`, one 1920x1080 output, `foot` windows):

```
$ ctest --test-dir build -R window_sway --output-on-failure
sway up: WAYLAND_DISPLAY=wayland-1 {"human_readable": "1.9", "variant": "sway", ...}
backend	wlr
can_list	yes
can_activate	yes
can_close	yes
can_minimize	yes
can_move_resize	yes
can_workspace_switch	yes
has_live_events	yes
has_previews	no

3	vorssaint-alpha	vorssaint-alpha	27877	0,0 640x1080	on-current-workspace,has-geometry,has-pid,on-screen	1	HEADLESS-1	0?
2	vorssaint-gamma	vorssaint-gamma	27879	640,0 640x1080	on-current-workspace,has-geometry,has-pid,on-screen	1	HEADLESS-1	1?
1	vorssaint-beta	vorssaint-beta	27878	1280,0 640x1080	focused,on-current-workspace,has-geometry,has-pid,on-screen	1	HEADLESS-1	2?

moved to 200,150 800x600      (confirmed independently from swaymsg -t get_tree)
gamma minimized after request: no

event	added	4	vorssaint-delta	-1
event	removed	4	-	-1
PASS: sway
```

The trailing `?` on the stacking column is the backend saying its order is not
a stacking order.

**Window ids are per connection.** A foreign-toplevel handle is numbered by the
client that bound it, so an id printed by one `vs-window list` means nothing to
the next invocation. The CLI harness therefore accepts `@app_id` (or `@title`)
and resolves it inside the process that acts on it; the switcher, being one
long-lived process, is unaffected.

## hyprland — the two IPC sockets

`$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock` for
request/response (one request per connection, the reply is the stream up to
EOF; `j/`-prefixed commands answer in JSON) and `.socket2.sock` for the
`EVENT>>data` line stream. Listing is `j/clients` enriched with `j/monitors`
and `j/activeworkspace`; the window id is the `address` field parsed out of its
`0x…` form. Commands are `dispatch focuswindow|closewindow|setfloating|
movewindowpixel exact X Y,address:0x…|resizewindowpixel exact W H,address:0x…|
workspace N`. Events acted on: `openwindow`, `closewindow`, `activewindowv2`,
`windowtitle(v2)`, `movewindow(v2)`, `fullscreen`, `workspace(v2)`.

`can_minimize` is **off by design**. Hyprland has no minimized state. The usual
substitute — moving the window to a special workspace — loses the window's own
workspace and is not something the compositor will undo, so the backend
declines rather than pretending. Windows Hyprland reports as `hidden` are still
*read* as minimized, so the switcher shows them correctly; it just cannot put
one there.

**Designed, not verified.** Hyprland needs a DRM device and cannot run in this
container or in CI. The suite drives the backend against
`linux/platform/window/tests/fake_hyprland.py`, which serves both sockets and
whose fixture is written from Hyprland's documented `hyprctl -j` shapes — it is
**not** a recording from a live Hyprland. The dispatch verbs mutate the fixture,
so the backend's read-back is a real read-back: a fake that answered `ok` and
changed nothing would fail the test.

```
$ ctest --test-dir build -R window_hyprland_fake --output-on-failure
backend	hyprland
can_list	yes
can_activate	yes
can_close	yes
can_minimize	no
can_move_resize	yes
can_workspace_switch	yes
has_live_events	yes
has_previews	no

94152977273856	vorssaint-alpha	vorssaint-alpha	4101	0,0 1280x1440	focused,on-current-workspace,has-geometry,has-pid,on-screen	1	DP-1	0?
94152977274880	vorssaint-beta	vorssaint-beta	4102	1280,0 1280x720	on-current-workspace,has-geometry,has-pid,on-screen	1	DP-1	1?
94152977275904	vorssaint-gamma	vorssaint-gamma	4103	1280,720 1280x720	minimized,has-geometry,has-pid	2	HDMI-A-1	2?

event	added	94152977276928	-	-1
event	changed	94152977276928	-	-1
event	removed	94152977276928	-	-1
PASS: hyprland (against fake_hyprland.py; unverified on a live Hyprland)
```

**First thing to check on real hardware:** that `j/clients` field names and the
`socket2` line formats still match, and whether `focusHistoryID == 0` is a
reliable "focused" test on a multi-monitor session.

## kwin — a KWin script over `org.kde.KWin /Scripting`

KWin exposes no foreign-toplevel protocol and no window API of its own on
D-Bus; the supported route is its scripting engine.

**The constraint that shapes this backend: a KWin script cannot own a D-Bus
name.** KWin's JS engine offers `callDBus` for calling *out* and nothing for
registering a service, so the interface in the work package — a script that
exports `org.vorssaint.KWinBridge` with `list()`, `activate(id)` and so on —
cannot be built as literally described. The two directions are therefore
asymmetric, and this is the deviation the lead should confirm:

| Direction | Mechanism |
|---|---|
| requests → KWin | the backend writes `kwin/vorssaint-window.js` with a `VORSSAINT_COMMAND` object prepended, loads it with `org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript(path, name)`, runs `/Scripting/Script<n>.run()`, then `unloadScript`. One command, one short-lived script. This is the mechanism `kdotool` uses, and it needs nothing from KWin that is not documented. |
| answers → us | the script calls `org.vorssaint.KWinBridge.Result(token, ok, json, message)`, a sink **this backend** owns, and the reply is matched to the request by token. |
| events → us | a second, persistent copy of the same script stays loaded and pushes `org.vorssaint.KWinBridge.Event(type, id, workspace)` from `windowAdded`, `windowRemoved`, `windowActivated` and `currentDesktopChanged`. |

Payloads are JSON strings rather than D-Bus structs because `callDBus`
marshals only the simple QVariant types; an array of structs is not one of
them. That is the one place the KWin path diverges from the GNOME one, which
does use the binary `a(tsssiiiiiuis)` encoding.

Window ids are KWin `internalId` UUIDs; the portable `vs_window_id` is their
FNV-1a hash, with the reverse map kept for the session. The suite checks the id
of the same window is identical across two invocations.

Every call out to KWin is asynchronous. A blocking `sd_bus_call` queues
incoming method calls instead of serving them, so the script's answer would
deadlock against the request that produced it; the backend waits by running the
bus (`sd_bus_process` / `sd_bus_wait`) instead.

**Designed, not verified.** KWin needs a Plasma session. The suite drives the
backend against `linux/platform/window/tests/fake_kwin.py`, which owns
`org.kde.KWin` on a private session bus, takes the `VORSSAINT_COMMAND` object
off the generated script and executes it against a fixture window set — what
`vorssaint-window.js` does inside KWin. The JavaScript itself is not executed;
`node --check` lints it separately.

```
$ ctest --test-dir build -R window_kwin_fake --output-on-failure
lint: node --check vorssaint-window.js ok
backend	kwin
can_list	yes
can_activate	yes
can_close	yes
can_minimize	yes
can_move_resize	yes
can_workspace_switch	yes
has_live_events	yes
has_previews	no

18196983065232973707	vorssaint-alpha	vorssaint-alpha	5101	0,0 1280x1440	focused,on-current-workspace,has-geometry,has-pid,on-screen	0	DP-1	0
15810614110926521051	vorssaint-beta	vorssaint-beta	5102	1280,0 1280x720	on-current-workspace,has-geometry,has-pid,on-screen	0	DP-1	1
9348938837508710315	vorssaint-gamma	vorssaint-gamma	5103	0,0 800x600	has-geometry,has-pid	1	HDMI-A-1	2

event	added	15613860601688712843	-	-1
event	activated	15613860601688712843	-	-1
event	removed	15613860601688712843	-	-1
event	workspace	0	-	1
PASS: kwin (against fake_kwin.py; unverified on a live KWin)
```

**First things to check on a real Plasma 6 session:** that `loadScript` still
accepts an arbitrary path outside the plugin directories (KWin has tightened
this before), that `window.frameGeometry = {…}` is honoured on a window that
was quick-tiled, and how expensive a load/run/unload cycle really is — if it is
slow, the switcher should push a whole window list from the persistent script
rather than ask per command.

## gnome — `org.vorssaint.WindowBridge`

GNOME (Mutter) implements no layer-shell and no foreign-toplevel protocols, so
window control goes through the `vorssaint-bridge` Shell extension (WP-C2).
GJS *can* export a D-Bus object, so unlike KWin the GNOME bridge really is the
service and this backend is a plain client of it:

```
org.vorssaint.WindowBridge  at  /org/vorssaint/WindowBridge
  List()                   -> a(tsssiiiiiuis)
  Activate(t)  Close(t)  SetMinimized(t, b)
  MoveResize(t, i, i, i, i)  Geometry(t) -> (iiii)
  CurrentWorkspace() -> i    SetWorkspace(i)
  signals: WindowAdded(t) WindowRemoved(t) WindowActivated(t)
           WindowChanged(t) WorkspaceChanged(i)
```

The struct is `id, app_id, app_name, title, pid, x, y, width, height, flags,
workspace, output`, with `flags` the same `vs_window_flag` bitmask the C header
defines. WP-C2 implements this interface; nothing else in the app talks to the
extension.

`MoveResize` returns without error whether or not Mutter honoured the request,
so only the backend's read-back through `Geometry` can tell the caller the
truth; that path is what the fake's "Text Editor refuses to resize" fixture
exercises.

**Designed, not verified.** GNOME Shell cannot run here. The suite drives the
backend against `linux/platform/window/tests/fake_window_bridge.py`, which
implements the interface exactly as the extension must — it is the executable
specification WP-C2 is written against.

```
$ ctest --test-dir build -R window_gnome_fake --output-on-failure
backend	gnome
can_list	yes
...
101	org.gnome.Nautilus	Home	6101	0,0 1280x800	on-current-workspace,has-geometry,has-pid,on-screen	0	XWAYLAND0	0
102	firefox	Vorssaint on GNOME	6102	100,60 1600x900	focused,on-current-workspace,has-geometry,has-pid,on-screen	0	XWAYLAND0	1
103	org.gnome.TextEditor	notes.md	6103	40,40 800x600	has-geometry,has-pid	1	XWAYLAND0	2

event	added	104	-	-1
event	activated	104	-	-1
event	removed	104	-	-1
event	workspace	0	-	1
PASS: gnome (against fake_window_bridge.py; unverified on a live GNOME Shell)
```

With no bridge on the bus the backend declines rather than guessing, which is
what makes `switcher` on GNOME show as unavailable until the extension is
installed.

## What this gives the features

- **`switcher` (WP-C3)** gets id, app_id, app_name, title, pid, minimized,
  focused, fullscreen, on-screen, workspace, output and geometry — every field
  `SwitcherItem` carries except the icon and the preview. Two backends cannot
  supply a stacking order, so MRU must come from the app's own history there.
  `VS_WINDOW_HAS_PID` is clear often enough on X11 that "group by app" must
  fall back to `app_id`.
- **`windowLayout` (WP-C4)** gets `move_resize` with a tolerance and a
  read-back, and `geometry` for the layout history, on every backend except
  bare wlroots without sway IPC. Edge-drag snapping is still X11-only, as the
  triage says: nothing here intercepts a drag.
- **`autoQuit` (WP-C5)** gets `VS_WINDOW_EVENT_ADDED` / `REMOVED` and the pid,
  which is the whole macOS `AXObserver` contract. On X11 a window without
  `_NET_WM_PID` cannot be attributed to a process, and autoQuit must skip it
  rather than guess.

## Running it by hand

```
$ vs-window backends                       # what is compiled in
$ vs-window probe                          # what this session selected, and its capabilities
$ vs-window list                           # id, app_id, title, pid, frame, flags, workspace, output, stacking
$ vs-window watch 10                       # events for ten seconds
$ vs-window activate @firefox              # @NAME resolves an app_id or title in-process
$ vs-window move 0x2a00003 100 100 800 600 4
$ vs-window close @firefox
```

`--backend NAME` forces one backend; `VS_WINDOW_DEBUG=1` turns on the
backends' own logging.
