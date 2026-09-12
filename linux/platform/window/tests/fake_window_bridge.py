#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# A stand-in for the vorssaint-bridge GNOME Shell extension (WP-C2): it owns
# org.vorssaint.WindowBridge and implements the interface in
# linux/platform/window/bridge_client.h exactly as the extension must. GNOME
# Shell cannot run in this project's container, so this is what pins the
# interface down before WP-C2 writes the other half.

import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

BRIDGE_SERVICE = "org.vorssaint.WindowBridge"
BRIDGE_PATH = "/org/vorssaint/WindowBridge"
BRIDGE_INTERFACE = "org.vorssaint.WindowBridge"

WINDOW_SIGNATURE = "a(tsssiiiiiuis)"

# Mirrors vs_window_flag.
MINIMIZED, FOCUSED, FULLSCREEN, MAXIMIZED = 1, 2, 4, 8
ON_CURRENT_WORKSPACE, HAS_GEOMETRY, HAS_PID, ON_SCREEN = 16, 32, 64, 128

WINDOWS = [
    # id, app_id, app_name, title, pid, x, y, w, h, workspace, output
    [101, "org.gnome.Nautilus", "Files", "Home", 6101, 0, 0, 1280, 800, 0, "XWAYLAND0"],
    [102, "firefox", "Firefox", "Vorssaint on GNOME", 6102, 100, 60, 1600, 900, 0, "XWAYLAND0"],
    [103, "org.gnome.TextEditor", "Text Editor", "notes.md", 6103, 40, 40, 800, 600, 1,
     "XWAYLAND0"],
]


class Bridge(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, BRIDGE_PATH)
        self.windows = [list(window) for window in WINDOWS]
        self.current_workspace = 0
        self.active = 102

    def find(self, window_id):
        for window in self.windows:
            if window[0] == window_id:
                return window
        return None

    def flags(self, window):
        flags = HAS_GEOMETRY | HAS_PID
        if window[0] == self.active:
            flags |= FOCUSED
        if window[9] == self.current_workspace:
            flags |= ON_CURRENT_WORKSPACE | ON_SCREEN
        if window[0] in self.minimized:
            flags |= MINIMIZED
            flags &= ~ON_SCREEN
        return flags

    minimized = set()

    @dbus.service.method(BRIDGE_INTERFACE, in_signature="", out_signature=WINDOW_SIGNATURE)
    def List(self):
        return [
            (dbus.UInt64(w[0]), w[1], w[2], w[3], dbus.Int32(w[4]), dbus.Int32(w[5]),
             dbus.Int32(w[6]), dbus.Int32(w[7]), dbus.Int32(w[8]), dbus.UInt32(self.flags(w)),
             dbus.Int32(w[9]), w[10])
            for w in self.windows
        ]

    @dbus.service.method(BRIDGE_INTERFACE, in_signature="t", out_signature="")
    def Activate(self, window_id):
        window = self.find(int(window_id))
        if window is None:
            raise dbus.exceptions.DBusException(
                "no such window", name="org.freedesktop.DBus.Error.InvalidArgs")
        self.active = window[0]
        self.minimized.discard(window[0])
        self.WindowActivated(dbus.UInt64(window[0]))

    @dbus.service.method(BRIDGE_INTERFACE, in_signature="t", out_signature="")
    def Close(self, window_id):
        window = self.find(int(window_id))
        if window is None:
            raise dbus.exceptions.DBusException(
                "no such window", name="org.freedesktop.DBus.Error.InvalidArgs")
        self.windows.remove(window)
        self.WindowRemoved(dbus.UInt64(window[0]))

    @dbus.service.method(BRIDGE_INTERFACE, in_signature="tb", out_signature="")
    def SetMinimized(self, window_id, minimized):
        window = self.find(int(window_id))
        if window is None:
            raise dbus.exceptions.DBusException(
                "no such window", name="org.freedesktop.DBus.Error.InvalidArgs")
        if minimized:
            self.minimized.add(window[0])
        else:
            self.minimized.discard(window[0])
        self.WindowChanged(dbus.UInt64(window[0]))

    @dbus.service.method(BRIDGE_INTERFACE, in_signature="tiiii", out_signature="")
    def MoveResize(self, window_id, x, y, width, height):
        window = self.find(int(window_id))
        if window is None:
            raise dbus.exceptions.DBusException(
                "no such window", name="org.freedesktop.DBus.Error.InvalidArgs")
        # Mutter clamps a window to its minimum size; Text Editor stands in for
        # an app that refuses, so the backend's read-back has something to
        # catch.
        if window[1] == "org.gnome.TextEditor":
            return
        window[5:9] = [int(x), int(y), int(width), int(height)]

    @dbus.service.method(BRIDGE_INTERFACE, in_signature="t", out_signature="(iiii)")
    def Geometry(self, window_id):
        window = self.find(int(window_id))
        if window is None:
            raise dbus.exceptions.DBusException(
                "no such window", name="org.freedesktop.DBus.Error.InvalidArgs")
        return (dbus.Int32(window[5]), dbus.Int32(window[6]), dbus.Int32(window[7]),
                dbus.Int32(window[8]))

    @dbus.service.method(BRIDGE_INTERFACE, in_signature="", out_signature="i")
    def CurrentWorkspace(self):
        return dbus.Int32(self.current_workspace)

    @dbus.service.method(BRIDGE_INTERFACE, in_signature="i", out_signature="")
    def SetWorkspace(self, workspace):
        self.current_workspace = int(workspace)
        self.WorkspaceChanged(dbus.Int32(self.current_workspace))

    @dbus.service.signal(BRIDGE_INTERFACE, signature="t")
    def WindowAdded(self, window_id):
        pass

    @dbus.service.signal(BRIDGE_INTERFACE, signature="t")
    def WindowRemoved(self, window_id):
        pass

    @dbus.service.signal(BRIDGE_INTERFACE, signature="t")
    def WindowActivated(self, window_id):
        pass

    @dbus.service.signal(BRIDGE_INTERFACE, signature="t")
    def WindowChanged(self, window_id):
        pass

    @dbus.service.signal(BRIDGE_INTERFACE, signature="i")
    def WorkspaceChanged(self, workspace):
        pass

    def schedule_events(self):
        new = [104, "org.gnome.Calculator", "Calculator", "Calculator", 6104, 10, 10, 400, 500, 0,
               "XWAYLAND0"]

        def added():
            self.windows.append(new)
            self.WindowAdded(dbus.UInt64(new[0]))
            return False

        def removed():
            if new in self.windows:
                self.windows.remove(new)
            self.WindowRemoved(dbus.UInt64(new[0]))
            return False

        GLib.timeout_add(800, added)
        GLib.timeout_add(1800, lambda: (self.WindowActivated(dbus.UInt64(new[0])), False)[1])
        GLib.timeout_add(2800, removed)
        GLib.timeout_add(3400, lambda: (self.WorkspaceChanged(dbus.Int32(1)), False)[1])


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    name = dbus.service.BusName(BRIDGE_SERVICE, bus)
    bridge = Bridge(bus)
    if "--events" in sys.argv:
        bridge.schedule_events()
    sys.stdout.write("ready\n")
    sys.stdout.flush()
    try:
        GLib.MainLoop().run()
    except KeyboardInterrupt:
        pass
    del name


if __name__ == "__main__":
    main()
