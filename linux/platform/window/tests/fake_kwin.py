#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# A stand-in for KWin's scripting D-Bus surface. KWin needs a Plasma session and
# cannot run in this project's container or CI, so the backend is driven against
# the documented surface instead:
#
#   org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript(s path, s name) -> i
#   org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript(s name)
#   org.kde.KWin /Scripting/Script<n> org.kde.kwin.Script.run()
#
# On `run` this reads the script the backend generated, takes the
# VORSSAINT_COMMAND object off its first line, and executes that command against
# a fixture window set — exactly what vorssaint-window.js does inside KWin —
# then answers on org.vorssaint.KWinBridge, which the backend owns.
#
# It therefore checks the backend's half of the contract and the *shape* of the
# generated script. It does not execute the JavaScript; `node --check` lints
# that separately. See docs/linux-port/WINDOW_BACKENDS.md.

import json
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

SINK_SERVICE = "org.vorssaint.KWinBridge"
SINK_PATH = "/org/vorssaint/KWinBridge"
SINK_INTERFACE = "org.vorssaint.KWinBridge"

# Mirrors vs_window_flag.
MINIMIZED, FOCUSED, FULLSCREEN, MAXIMIZED = 1, 2, 4, 8
ON_CURRENT_WORKSPACE, HAS_GEOMETRY, HAS_PID, ON_SCREEN = 16, 32, 64, 128

WINDOWS = [
    {
        "id": "{11111111-1111-1111-1111-111111111111}",
        "app_id": "vorssaint-alpha",
        "app_name": "Vorssaint Alpha",
        "title": "vorssaint-alpha",
        "pid": 5101,
        "x": 0, "y": 0, "width": 1280, "height": 1440,
        "workspace": 0,
        "minimized": False,
        "active": True,
        "output": "DP-1",
    },
    {
        "id": "{22222222-2222-2222-2222-222222222222}",
        "app_id": "vorssaint-beta",
        "app_name": "Vorssaint Beta",
        "title": "vorssaint-beta",
        "pid": 5102,
        "x": 1280, "y": 0, "width": 1280, "height": 720,
        "workspace": 0,
        "minimized": False,
        "active": False,
        "output": "DP-1",
    },
    {
        "id": "{33333333-3333-3333-3333-333333333333}",
        "app_id": "vorssaint-gamma",
        "app_name": "Vorssaint Gamma",
        "title": "vorssaint-gamma",
        "pid": 5103,
        "x": 0, "y": 0, "width": 800, "height": 600,
        "workspace": 1,
        "minimized": False,
        "active": False,
        "output": "HDMI-A-1",
    },
]


class Model:
    def __init__(self):
        self.windows = [dict(window) for window in WINDOWS]
        self.current_desktop = 0
        self.desktop_count = 2
        self.loaded = {}
        self.load_log = []

    def find(self, uuid):
        for window in self.windows:
            if window["id"] == uuid:
                return window
        return None

    def describe(self, window, stacking_index):
        flags = 0
        if window["minimized"]:
            flags |= MINIMIZED
        if window["active"]:
            flags |= FOCUSED
        if window["workspace"] == self.current_desktop:
            flags |= ON_CURRENT_WORKSPACE
        if window["width"] > 0 and window["height"] > 0:
            flags |= HAS_GEOMETRY
        if window["pid"] > 0:
            flags |= HAS_PID
        if not window["minimized"] and (flags & ON_CURRENT_WORKSPACE):
            flags |= ON_SCREEN
        record = {key: window[key] for key in
                  ("id", "app_id", "app_name", "title", "pid", "x", "y", "width", "height",
                   "workspace", "output")}
        record["flags"] = flags
        record["stacking_index"] = stacking_index
        return record

    def run(self, command):
        verb = command["verb"]
        args = command.get("args", [])
        if verb == "list":
            return True, [self.describe(w, i) for i, w in enumerate(self.windows)], ""
        if verb == "currentWorkspace":
            return True, self.current_desktop, ""
        if verb == "setWorkspace":
            index = args[0]
            if not 0 <= index < self.desktop_count:
                return False, None, "no such desktop"
            self.current_desktop = index
            return True, self.current_desktop, ""

        window = self.find(args[0])
        if window is None:
            return False, None, "no such window"
        if verb == "activate":
            for other in self.windows:
                other["active"] = False
            window["active"] = True
            window["minimized"] = False
            return True, None, ""
        if verb == "close":
            self.windows.remove(window)
            return True, None, ""
        if verb == "setMinimized":
            window["minimized"] = bool(args[1])
            return True, None, ""
        if verb == "geometry":
            return True, {key: window[key] for key in ("x", "y", "width", "height")}, ""
        if verb == "moveResize":
            # KWin honours frameGeometry on a normal window; a window with a
            # minimum size larger than the request keeps its own, which is what
            # the backend's read-back is for. gamma stands in for that case.
            if window["app_id"] == "vorssaint-gamma":
                return True, {key: window[key] for key in ("x", "y", "width", "height")}, ""
            window["x"], window["y"], window["width"], window["height"] = args[1:5]
            return True, {key: window[key] for key in ("x", "y", "width", "height")}, ""
        return False, None, "unknown verb " + verb


class Script(dbus.service.Object):
    def __init__(self, bus, path, model, source_path, sink):
        super().__init__(bus, path)
        self.model = model
        self.source_path = source_path
        self._sink = sink
        self.sources = []

    # Each vs-window invocation loads its own persistent copy, so an earlier
    # invocation's scripted events can still fire after that process is gone.
    # A bridge that has exited is not a test failure.
    def sink(self):
        class Quiet:
            def __init__(self, factory):
                self.factory = factory

            def __getattr__(self, name):
                def call(*args):
                    try:
                        getattr(self.factory(), name)(*args)
                    except dbus.exceptions.DBusException:
                        pass
                return call
        return Quiet(self._sink)

    @dbus.service.method("org.kde.kwin.Script", in_signature="", out_signature="")
    def run(self):
        # KWin returns from `run` as soon as the script has been started; the
        # script's own callBus reply arrives afterwards. Answering inline here
        # would instead deadlock against the caller, which is still blocked
        # waiting for this reply — so the answer is queued, exactly as KWin
        # would deliver it.
        with open(self.source_path, encoding="utf-8") as handle:
            source = handle.read()
        GLib.idle_add(self.execute, source)

    def execute(self, source):
        if "VORSSAINT_COMMAND" not in source.split("\n")[0]:
            # The persistent copy: install the event hooks, which here means
            # replaying a scripted window lifecycle on the sink.
            self.schedule_events()
            return False

        prologue = source.split("\n", 1)[0]
        payload = prologue[prologue.index("{"):prologue.rindex("}") + 1]
        command = json.loads(payload)
        ok, result, message = self.model.run(command)
        self.sink().Result(command["token"], ok, json.dumps(result), message)
        return False

    # The scripted lifecycle runs on the main loop, never on a thread:
    # dbus-python is not thread-safe without a threaded main loop, and the
    # backend is entitled to be served while it waits.
    def schedule_events(self):
        delta = {
            "id": "{44444444-4444-4444-4444-444444444444}",
            "app_id": "vorssaint-delta",
            "app_name": "Vorssaint Delta",
            "title": "vorssaint-delta",
            "pid": 5104,
            "x": 40, "y": 40, "width": 640, "height": 480,
            "workspace": 0,
            "minimized": False,
            "active": False,
            "output": "DP-1",
        }

        def added():
            self.model.windows.append(delta)
            self.sink().Event("added", delta["id"], -1)
            return False

        def activated():
            self.sink().Event("activated", delta["id"], -1)
            return False

        def removed():
            if delta in self.model.windows:
                self.model.windows.remove(delta)
            self.sink().Event("removed", delta["id"], -1)
            return False

        def workspace():
            self.model.current_desktop = 1
            self.sink().Event("workspace", "", 1)
            return False

        self.sources += [
            GLib.timeout_add(200, lambda: (self.sink().Event("ready", "", -1), False)[1]),
            GLib.timeout_add(1000, added),
            GLib.timeout_add(2000, activated),
            GLib.timeout_add(3000, removed),
            GLib.timeout_add(3500, workspace),
        ]

    def retire(self):
        for source in self.sources:
            GLib.source_remove(source)
        self.sources = []
        self.remove_from_connection()


class Scripting(dbus.service.Object):
    def __init__(self, bus, model):
        super().__init__(bus, "/Scripting")
        self.bus = bus
        self.model = model
        self.next_id = 0
        self.scripts = {}

    def sink(self):
        return dbus.Interface(
            dbus.SessionBus().get_object(SINK_SERVICE, SINK_PATH), SINK_INTERFACE)

    @dbus.service.method("org.kde.kwin.Scripting", in_signature="ss", out_signature="i")
    def loadScript(self, path, name):
        script_id = self.next_id
        self.next_id += 1
        self.model.load_log.append((str(name), str(path)))
        # KWin replaces a script loaded twice under the same name; so does this,
        # or an earlier copy would keep pushing events for a process that has
        # already exited.
        previous = self.scripts.pop(str(name), None)
        if previous is not None:
            previous.retire()
        self.scripts[str(name)] = Script(
            self.bus, "/Scripting/Script%d" % script_id, self.model, str(path), self.sink)
        return script_id

    @dbus.service.method("org.kde.kwin.Scripting", in_signature="s", out_signature="")
    def unloadScript(self, name):
        script = self.scripts.pop(str(name), None)
        if script is not None:
            script.retire()


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    name = dbus.service.BusName("org.kde.KWin", bus)
    model = Model()
    Scripting(bus, model)
    sys.stdout.write("ready\n")
    sys.stdout.flush()
    loop = GLib.MainLoop()
    try:
        loop.run()
    except KeyboardInterrupt:
        pass
    del name


if __name__ == "__main__":
    main()
