// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// KWin bridge script.
//
// KWin's scripting engine can *call* D-Bus (`callDBus`) but cannot own a bus
// name, so this script is not itself a D-Bus service: it pushes into
// org.vorssaint.KWinBridge, which the Vorssaint process owns. Requests come the
// other way: the C side writes a copy of this file with a VORSSAINT_COMMAND
// object prepended and loads it through org.kde.KWin /Scripting loadScript, so
// one command is one short-lived script.
//
// Two roles, chosen by the tail of the file:
//   - loaded plain, it installs the workspace signal handlers and pushes
//     windowAdded / windowRemoved / windowActivated / workspace changes;
//   - loaded with VORSSAINT_COMMAND set, it performs that one command and
//     pushes the answer under the command's token.
//
// Payloads are JSON strings, not D-Bus structs: KWin's callDBus marshals only
// the simple QVariant types, and an array of structs is not one of them.
//
// KDE Plasma 6 API (KWin 6.x): `workspace.windowList()`, `window.frameGeometry`,
// `window.internalId`, `window.resourceClass`, `window.pid`,
// `workspace.currentDesktop`, `workspace.desktops`.

/* global workspace, callDBus, VORSSAINT_COMMAND, print */

var VORSSAINT_SERVICE = "org.vorssaint.KWinBridge";
var VORSSAINT_PATH = "/org/vorssaint/KWinBridge";
var VORSSAINT_IFACE = "org.vorssaint.KWinBridge";

// Mirrors vs_window_flag in linux/platform/include/vorssaint_platform.h.
var FLAG_MINIMIZED = 1;
var FLAG_FOCUSED = 2;
var FLAG_FULLSCREEN = 4;
var FLAG_MAXIMIZED = 8;
var FLAG_ON_CURRENT_WORKSPACE = 16;
var FLAG_HAS_GEOMETRY = 32;
var FLAG_HAS_PID = 64;
var FLAG_ON_SCREEN = 128;

function vorssaintWindows() {
    return workspace.windowList ? workspace.windowList() : workspace.clientList();
}

function vorssaintDesktopIndex(desktop) {
    if (desktop === null || desktop === undefined) {
        return -1;
    }
    if (typeof desktop === "number") {
        return desktop;
    }
    var desktops = workspace.desktops || [];
    for (var i = 0; i < desktops.length; i++) {
        if (desktops[i] === desktop || desktops[i].id === desktop.id) {
            return i;
        }
    }
    return -1;
}

function vorssaintCurrentDesktopIndex() {
    return vorssaintDesktopIndex(workspace.currentDesktop);
}

// KWin has no per-window "is it on the visible desktop" flag: a window is on
// every desktop (`onAllDesktops`) or on the ones its `desktops` array names.
function vorssaintWindowDesktop(window) {
    if (window.onAllDesktops) {
        return -1;
    }
    if (window.desktops && window.desktops.length > 0) {
        return vorssaintDesktopIndex(window.desktops[0]);
    }
    return vorssaintDesktopIndex(window.desktop);
}

function vorssaintDescribe(window, stackingIndex) {
    var geometry = window.frameGeometry || { x: 0, y: 0, width: 0, height: 0 };
    var current = vorssaintCurrentDesktopIndex();
    var desktop = vorssaintWindowDesktop(window);
    var flags = 0;
    if (window.minimized) {
        flags |= FLAG_MINIMIZED;
    }
    if (window.active) {
        flags |= FLAG_FOCUSED;
    }
    if (window.fullScreen) {
        flags |= FLAG_FULLSCREEN;
    }
    if (window.maximizedHorizontally && window.maximizedVertically) {
        flags |= FLAG_MAXIMIZED;
    }
    if (desktop < 0 || desktop === current) {
        flags |= FLAG_ON_CURRENT_WORKSPACE;
    }
    if (geometry.width > 0 && geometry.height > 0) {
        flags |= FLAG_HAS_GEOMETRY;
    }
    if (window.pid > 0) {
        flags |= FLAG_HAS_PID;
    }
    if (!window.minimized && (flags & FLAG_ON_CURRENT_WORKSPACE) !== 0) {
        flags |= FLAG_ON_SCREEN;
    }
    return {
        id: String(window.internalId),
        app_id: window.resourceClass || "",
        app_name: window.resourceName || window.resourceClass || "",
        title: window.caption || "",
        pid: window.pid > 0 ? window.pid : -1,
        x: geometry.x,
        y: geometry.y,
        width: geometry.width,
        height: geometry.height,
        flags: flags,
        workspace: desktop,
        output: window.output ? String(window.output.name) : "",
        stacking_index: stackingIndex
    };
}

// Only windows a person would switch to. KWin marks its own furniture with
// `specialWindow`, and `skipTaskbar` is the window's own request to be left out.
function vorssaintIsSwitchable(window) {
    return window.normalWindow === true && !window.specialWindow && !window.skipTaskbar;
}

function vorssaintFind(id) {
    var windows = vorssaintWindows();
    for (var i = 0; i < windows.length; i++) {
        if (String(windows[i].internalId) === String(id)) {
            return windows[i];
        }
    }
    return null;
}

function vorssaintList() {
    var windows = vorssaintWindows();
    var result = [];
    for (var i = 0; i < windows.length; i++) {
        if (!vorssaintIsSwitchable(windows[i])) {
            continue;
        }
        result.push(vorssaintDescribe(windows[i], result.length));
    }
    return result;
}

function vorssaintPushResult(token, ok, payload, message) {
    callDBus(VORSSAINT_SERVICE, VORSSAINT_PATH, VORSSAINT_IFACE, "Result",
             String(token), ok === true, JSON.stringify(payload === undefined ? null : payload),
             String(message || ""));
}

function vorssaintPushEvent(type, id, workspaceIndex) {
    callDBus(VORSSAINT_SERVICE, VORSSAINT_PATH, VORSSAINT_IFACE, "Event",
             String(type), String(id || ""), workspaceIndex === undefined ? -1 : workspaceIndex);
}

function vorssaintRunCommand(command) {
    var verb = command.verb;
    var args = command.args || [];
    var window = null;
    try {
        if (verb === "list") {
            vorssaintPushResult(command.token, true, vorssaintList());
            return;
        }
        if (verb === "currentWorkspace") {
            vorssaintPushResult(command.token, true, vorssaintCurrentDesktopIndex());
            return;
        }
        if (verb === "setWorkspace") {
            var desktops = workspace.desktops || [];
            var index = args[0];
            if (index < 0 || index >= desktops.length) {
                vorssaintPushResult(command.token, false, null, "no such desktop");
                return;
            }
            workspace.currentDesktop = desktops[index];
            vorssaintPushResult(command.token, true, vorssaintCurrentDesktopIndex());
            return;
        }

        window = vorssaintFind(args[0]);
        if (window === null) {
            vorssaintPushResult(command.token, false, null, "no such window");
            return;
        }
        if (verb === "activate") {
            if (window.minimized) {
                window.minimized = false;
            }
            workspace.activeWindow = window;
            vorssaintPushResult(command.token, true, null);
            return;
        }
        if (verb === "close") {
            window.closeWindow();
            vorssaintPushResult(command.token, true, null);
            return;
        }
        if (verb === "setMinimized") {
            window.minimized = args[1] === true;
            vorssaintPushResult(command.token, true, null);
            return;
        }
        if (verb === "geometry") {
            var frame = window.frameGeometry;
            vorssaintPushResult(command.token, true,
                                { x: frame.x, y: frame.y, width: frame.width, height: frame.height });
            return;
        }
        if (verb === "moveResize") {
            // A maximized or quick-tiled window ignores frameGeometry, so the
            // state has to come off first; the C side reads the frame back and
            // reports VS_ERR_NOT_APPLIED if KWin still refused.
            window.setMaximize(false, false);
            window.frameGeometry = { x: args[1], y: args[2], width: args[3], height: args[4] };
            var applied = window.frameGeometry;
            vorssaintPushResult(command.token, true, {
                x: applied.x, y: applied.y, width: applied.width, height: applied.height
            });
            return;
        }
        vorssaintPushResult(command.token, false, null, "unknown verb " + verb);
    } catch (error) {
        vorssaintPushResult(command.token, false, null, String(error));
    }
}

function vorssaintInstallEvents() {
    workspace.windowAdded.connect(function (window) {
        if (vorssaintIsSwitchable(window)) {
            vorssaintPushEvent("added", window.internalId);
        }
    });
    workspace.windowRemoved.connect(function (window) {
        vorssaintPushEvent("removed", window.internalId);
    });
    workspace.windowActivated.connect(function (window) {
        vorssaintPushEvent("activated", window ? window.internalId : "");
    });
    workspace.currentDesktopChanged.connect(function () {
        vorssaintPushEvent("workspace", "", vorssaintCurrentDesktopIndex());
    });
    vorssaintPushEvent("ready", "");
}

if (typeof VORSSAINT_COMMAND !== "undefined" && VORSSAINT_COMMAND !== null) {
    vorssaintRunCommand(VORSSAINT_COMMAND);
} else {
    vorssaintInstallEvents();
}
