#!/usr/bin/env python3.12
"""
A stand-in org.freedesktop.impl.portal.{Screenshot,Access} backend.

It was written to isolate *why* xdg-desktop-portal 1.18.4 refuses to export
the Screenshot portal on top of xdg-desktop-portal-wlr 0.7.1, by varying one
thing at a time: the advertised impl `version` (--version, default 2) and
whether the Access interface is offered at all (via the .portal file that
names it). The answer, recorded in docs/linux-port/spikes/02-capture.md § 3:
the version is irrelevant; what is missing is an
org.freedesktop.impl.portal.Access backend -- xdpw implements none, and the
Screenshot frontend needs one for its consent step.

Screenshot is served for real, by shelling out to `grim`, which goes through
zwlr_screencopy_v1 -- the same wlroots protocol xdpw itself uses -- so the
pixels come from the real compositor rather than from a fixture. AccessDialog
auto-approves, since nobody can click a dialog on a headless box.

  ./mock-screenshot-impl.py [--version N]

Install alongside it a .portal file pointing at
`org.freedesktop.impl.portal.desktop.wp02mock` and restart xdg-desktop-portal;
`run-stack.sh` does exactly that when WP02_ACCESS_SHIM=1 (the default).
"""

import argparse
import os
import subprocess
import sys
import tempfile

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

BUS_NAME = "org.freedesktop.impl.portal.desktop.wp02mock"
OBJ_PATH = "/org/freedesktop/portal/desktop"

NODE_XML = """
<node>
  <interface name='org.freedesktop.impl.portal.Access'>
    <method name='AccessDialog'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='s' name='title' direction='in'/>
      <arg type='s' name='subtitle' direction='in'/>
      <arg type='s' name='body' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
    </method>
    <property name='version' type='u' access='read'/>
  </interface>
  <interface name='org.freedesktop.impl.portal.Screenshot'>
    <method name='Screenshot'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
    </method>
    <method name='PickColor'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
    </method>
    <property name='version' type='u' access='read'/>
  </interface>
</node>
"""


class Impl:
    def __init__(self, version: int, outdir: str):
        self.version = version
        self.outdir = outdir
        os.makedirs(outdir, exist_ok=True)

    def on_call(self, conn, sender, path, iface, method, params, invocation):
        if method == "AccessDialog":
            # Auto-approve: there is nobody to click a dialog on a headless box.
            handle, app_id, parent, title, subtitle, body, options = params.unpack()
            print(f"[mock] AccessDialog {title!r} -> auto-allow", flush=True)
            invocation.return_value(GLib.Variant("(ua{sv})", (0, {})))
        elif method == "Screenshot":
            handle, app_id, parent, options = params.unpack()
            interactive = bool(options.get("interactive", False))
            print(
                f"[mock] Screenshot handle={handle} app_id={app_id!r} "
                f"interactive={interactive} options={options}",
                flush=True,
            )
            fd, png = tempfile.mkstemp(suffix=".png", dir=self.outdir)
            os.close(fd)
            rc = subprocess.run(["grim", png], capture_output=True)
            if rc.returncode != 0:
                print(f"[mock] grim failed: {rc.stderr!r}", flush=True)
                invocation.return_value(GLib.Variant("(ua{sv})", (2, {})))
                return
            uri = GLib.filename_to_uri(png, None)
            invocation.return_value(
                GLib.Variant("(ua{sv})", (0, {"uri": GLib.Variant("s", uri)}))
            )
        elif method == "PickColor":
            invocation.return_value(
                GLib.Variant(
                    "(ua{sv})",
                    (
                        0,
                        {
                            "color": GLib.Variant(
                                "(ddd)", (0.11764, 0.37254, 0.70588)
                            )
                        },
                    ),
                )
            )
        else:
            invocation.return_dbus_error(
                "org.freedesktop.DBus.Error.UnknownMethod", method
            )

    def on_get(self, conn, sender, path, iface, prop):
        if prop == "version":
            return GLib.Variant("u", self.version)
        return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", type=int, default=2)
    ap.add_argument("--outdir", default="/tmp/wp02/mock-shots")
    args = ap.parse_args()

    impl = Impl(args.version, args.outdir)
    node = Gio.DBusNodeInfo.new_for_xml(NODE_XML)
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    for iface in node.interfaces:
        bus.register_object(OBJ_PATH, iface, impl.on_call, impl.on_get, None)
    Gio.bus_own_name_on_connection(
        bus,
        BUS_NAME,
        Gio.BusNameOwnerFlags.NONE,
        lambda *a: print(f"[mock] took {BUS_NAME}, version={impl.version}", flush=True),
        lambda *a: (print("[mock] lost name", flush=True), sys.exit(1)),
    )
    GLib.MainLoop().run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
