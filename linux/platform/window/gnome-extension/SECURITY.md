# vorssaint-bridge: what installing it exposes

The extension runs **inside gnome-shell**, in the same process and with the
same privileges as your whole desktop. It exports
`org.vorssaint.WindowBridge` on the **session bus**. There is no access
control on a session bus name: every process running as you — including
anything you install from a browser, a Flatpak with `--socket=session-bus`,
and any script in your shell — can call every method on it.

That is the trade the GNOME port makes, and it should be stated plainly to the
user in the Capabilities page (WP-25), not buried here.

## What a caller on the session bus can do through the bridge

| Member | What it gives a caller |
|---|---|
| `List` | every window's app id, title, pid, geometry, workspace and monitor |
| `Activate`, `Close`, `SetMinimized` | focus, close or minimize any window |
| `MoveResize`, `SetWorkspace` | move or resize any window, switch workspace |
| `ClipboardMimeTypes`, `ClipboardRead` | **read the clipboard and the primary selection at any time, without focus** |
| `ClipboardWrite`, `ClipboardClear` | replace the clipboard contents |
| `ClipboardChanged` | a signal on every clipboard change, with its mime types |

The clipboard members are the sharp edge: GNOME deliberately has no
`ext-data-control`/`wlr-data-control`, precisely so that a background process
cannot read the clipboard. This extension reintroduces exactly that ability
for anything on the session bus. Window titles are nearly as sensitive — they
carry document names, URLs and message subjects.

None of this is a privilege the caller did not already have in a different
form: any process in the session can already drive the clipboard by faking
focus, take a screenshot through the portal with one consent dialog, or read
`~/.bash_history`. But the bridge makes it silent, immediate and scriptable,
which is a real change in exposure. Anyone who considers that unacceptable
should not install the extension: without it Vorssaint hides the switcher,
window layout, auto-quit and clipboard history on GNOME, and everything else
still works.

## What the extension deliberately does not do

- **No code execution.** There is no `eval`, no `new Function`, no
  `GLib.spawn`, no `Gio.Subprocess`, and no member that takes a script, a
  command, a path or a D-Bus destination from the caller. The only strings a
  caller supplies are a mime type and clipboard bytes.
  `test/check_static.sh` greps the installed files for exactly these and
  fails the build if one appears.
- **No file access.** The extension reads and writes no file at runtime. It
  has no `schemas/` directory and no settings, so there is no GSettings state
  to tamper with either.
- **No network.** Nothing is opened, nothing is fetched.
- **No lock-screen presence.** `session-modes` is `["user"]` only, so GNOME
  unloads the extension on the lock screen: the bridge cannot be used to list
  windows or read the clipboard of a locked session.
- **No privilege escalation path.** The extension calls nothing as root, adds
  no polkit action and no udev rule, and is not reachable by `vorssaint-helper`.
  It is installed into `~/.local/share/gnome-shell/extensions/` by
  `install.sh` as an ordinary user, and `uninstall.sh` deletes that directory.
- **No arbitrary window targeting by name.** A caller addresses a window by an
  id the bridge itself handed out; an id the bridge does not know is an error,
  not a scan.

## Denial of service

`List` and the commands are cheap and bounded, and `WindowChanged` is
throttled (`lib/throttle.js`) so a caller cannot make the bridge flood the bus
by dragging a window. A caller *can* still call `List` in a tight loop; the
work per call is proportional to the window count and happens on the Shell's
main loop, so a hostile caller can make the desktop stutter. That is true of
every Shell extension with a D-Bus surface and is not mitigated here — a
rate limit would have to be tuned against the switcher's own polling, and the
switcher is the only intended caller.

## Reviewing a change to this extension

Anything that adds a member taking a *name* rather than an id, or that reads a
path, spawns a process, or touches a file, is out of the design and needs the
lead's sign-off per `AGENT_PLAYBOOK.md` ("Privilege is a design item").
