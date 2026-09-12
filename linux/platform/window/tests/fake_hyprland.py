#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# A stand-in for Hyprland's two IPC sockets. Hyprland needs a DRM device and
# cannot run in this project's container or CI, so the backend is driven against
# the documented protocol instead:
#
#   .socket.sock   one request per connection, the client half-closes, the reply
#                  is the whole stream up to EOF. `j/` commands answer in JSON,
#                  dispatches answer "ok".
#   .socket2.sock  a line stream of `EVENT>>data` notifications.
#
# The fixture below is written from Hyprland's documented `hyprctl -j` shapes
# (hyprland.org/Configuring/Using-hyprctl, IPC docs). It is NOT a recording from
# a live Hyprland; see docs/linux-port/WINDOW_BACKENDS.md.
#
# The dispatch verbs mutate the fixture, so the backend's read-back is a real
# read-back: a fake that answered "ok" and changed nothing would fail the test.

import json
import os
import socket
import sys
import threading
import time

CLIENTS = [
    {
        "address": "0x55a1b2c3d400",
        "mapped": True,
        "hidden": False,
        "at": [0, 0],
        "size": [1280, 1440],
        "workspace": {"id": 1, "name": "1"},
        "floating": False,
        "monitor": 0,
        "class": "vorssaint-alpha",
        "title": "vorssaint-alpha",
        "initialClass": "vorssaint-alpha",
        "initialTitle": "vorssaint-alpha",
        "pid": 4101,
        "xwayland": False,
        "pinned": False,
        "fullscreen": 0,
        "focusHistoryID": 0,
    },
    {
        "address": "0x55a1b2c3d800",
        "mapped": True,
        "hidden": False,
        "at": [1280, 0],
        "size": [1280, 720],
        "workspace": {"id": 1, "name": "1"},
        "floating": True,
        "monitor": 0,
        "class": "vorssaint-beta",
        "title": "vorssaint-beta",
        "initialClass": "vorssaint-beta",
        "initialTitle": "vorssaint-beta",
        "pid": 4102,
        "xwayland": False,
        "pinned": False,
        "fullscreen": 0,
        "focusHistoryID": 1,
    },
    {
        "address": "0x55a1b2c3dc00",
        "mapped": True,
        "hidden": True,
        "at": [1280, 720],
        "size": [1280, 720],
        "workspace": {"id": 2, "name": "2"},
        "floating": False,
        "monitor": 1,
        "class": "vorssaint-gamma",
        "title": "vorssaint-gamma",
        "initialClass": "vorssaint-gamma",
        "initialTitle": "vorssaint-gamma",
        "pid": 4103,
        "xwayland": False,
        "pinned": False,
        "fullscreen": 0,
        "focusHistoryID": 2,
    },
]

MONITORS = [
    {"id": 0, "name": "DP-1", "width": 2560, "height": 1440, "x": 0, "y": 0, "focused": True},
    {"id": 1, "name": "HDMI-A-1", "width": 2560, "height": 1440, "x": 2560, "y": 0,
     "focused": False},
]

VERSION = {"branch": "main", "commit": "fixture", "tag": "v0.45.0"}


class FakeHyprland:
    def __init__(self, directory):
        self.directory = directory
        self.clients = [dict(client) for client in CLIENTS]
        self.active_workspace = 1
        self.lock = threading.Lock()
        self.event_clients = []
        self.commands = []

    # ------------------------------------------------------------- helpers

    def find(self, address):
        wanted = address.lower().removeprefix("0x")
        for client in self.clients:
            if client["address"].lower().removeprefix("0x") == wanted:
                return client
        return None

    def broadcast(self, line):
        with self.lock:
            targets = list(self.event_clients)
        for connection in targets:
            try:
                connection.sendall((line + "\n").encode())
            except OSError:
                pass

    # ------------------------------------------------------------ requests

    def handle(self, request):
        self.commands.append(request)
        if request == "j/version":
            return json.dumps(VERSION)
        if request == "j/clients":
            return json.dumps(self.clients)
        if request == "j/monitors":
            return json.dumps(MONITORS)
        if request == "j/activeworkspace":
            return json.dumps({"id": self.active_workspace, "name": str(self.active_workspace)})
        if request.startswith("dispatch "):
            return self.dispatch(request[len("dispatch "):])
        return "unknown request"

    def dispatch(self, command):
        verb, _, rest = command.partition(" ")
        if verb == "workspace":
            try:
                self.active_workspace = int(rest.strip())
            except ValueError:
                return "invalid workspace"
            self.broadcast("workspacev2>>%d,%d" % (self.active_workspace, self.active_workspace))
            return "ok"

        # `movewindowpixel exact X Y,address:0x..` and the resize equivalent put
        # the target last; `focuswindow address:0x..` and `closewindow
        # address:0x..` take it alone.
        address = None
        for part in rest.split(","):
            part = part.strip()
            if part.startswith("address:"):
                address = part[len("address:"):]
        if address is None:
            return "no address given"
        client = self.find(address)
        if client is None:
            return "window not found"

        if verb == "focuswindow":
            for other in self.clients:
                other["focusHistoryID"] += 1
            client["focusHistoryID"] = 0
            self.broadcast("activewindowv2>>" + client["address"].removeprefix("0x"))
            return "ok"
        if verb == "closewindow":
            self.clients.remove(client)
            self.broadcast("closewindow>>" + client["address"].removeprefix("0x"))
            return "ok"
        if verb == "setfloating":
            client["floating"] = True
            return "ok"
        if verb in ("movewindowpixel", "resizewindowpixel"):
            arguments = rest.split(",")[0].strip().split()
            if len(arguments) != 3 or arguments[0] != "exact":
                return "expected: exact X Y"
            if not client["floating"]:
                # A tiled window keeps its layout geometry; Hyprland accepts the
                # dispatch and the tiling algorithm wins. This is exactly the
                # "success without effect" the backend reads back for.
                return "ok"
            first, second = int(arguments[1]), int(arguments[2])
            if verb == "movewindowpixel":
                client["at"] = [first, second]
            else:
                client["size"] = [first, second]
            self.broadcast("movewindowv2>>%s,%d,%d" %
                           (client["address"].removeprefix("0x"), client["workspace"]["id"],
                            client["workspace"]["id"]))
            return "ok"
        return "unknown dispatch " + verb

    # ------------------------------------------------------------- servers

    def serve_requests(self, server):
        while True:
            try:
                connection, _ = server.accept()
            except OSError:
                return
            with connection:
                chunks = []
                while True:
                    chunk = connection.recv(4096)
                    if not chunk:
                        break
                    chunks.append(chunk)
                request = b"".join(chunks).decode(errors="replace").strip()
                try:
                    connection.sendall(self.handle(request).encode())
                except OSError:
                    pass

    def serve_events(self, server):
        while True:
            try:
                connection, _ = server.accept()
            except OSError:
                return
            with self.lock:
                self.event_clients.append(connection)


def main():
    directory = sys.argv[1]
    os.makedirs(directory, exist_ok=True)
    fake = FakeHyprland(directory)

    servers = []
    for name, handler in (
        (".socket.sock", fake.serve_requests),
        (".socket2.sock", fake.serve_events),
    ):
        path = os.path.join(directory, name)
        if os.path.exists(path):
            os.unlink(path)
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(path)
        server.listen(16)
        servers.append(server)
        threading.Thread(target=handler, args=(server,), daemon=True).start()

    # Announce readiness only once both sockets accept, so the test never races
    # the backend's probe.
    sys.stdout.write("ready\n")
    sys.stdout.flush()

    # A scripted window appearing after the watcher attaches, then closing: the
    # two events the switcher and autoQuit actually consume.
    def scripted_events():
        time.sleep(float(os.environ.get("VS_FAKE_HYPR_DELAY", "2.0")))
        fake.clients.append({
            "address": "0x55a1b2c3e000",
            "mapped": True,
            "hidden": False,
            "at": [100, 100],
            "size": [640, 480],
            "workspace": {"id": 1, "name": "1"},
            "floating": True,
            "monitor": 0,
            "class": "vorssaint-delta",
            "title": "vorssaint-delta",
            "initialClass": "vorssaint-delta",
            "initialTitle": "vorssaint-delta",
            "pid": 4104,
            "xwayland": False,
            "pinned": False,
            "fullscreen": 0,
            "focusHistoryID": 3,
        })
        fake.broadcast("openwindow>>55a1b2c3e000,1,vorssaint-delta,vorssaint-delta")
        time.sleep(1.0)
        fake.broadcast("windowtitlev2>>55a1b2c3e000,vorssaint-delta renamed")
        time.sleep(1.0)
        fake.clients = [c for c in fake.clients if c["address"] != "0x55a1b2c3e000"]
        fake.broadcast("closewindow>>55a1b2c3e000")

    threading.Thread(target=scripted_events, daemon=True).start()

    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
