// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// A window source backed by a fixture instead of Mutter, with exactly the
// window set, ids, workspace and refusal behaviour of
// linux/platform/window/tests/fake_window_bridge.py — the executable
// specification WP-C1 wrote the C client against.
//
// Because lib/service.js is the same file the extension runs, driving this
// source from a plain gjs process makes the real C backend talk to the real
// bridge implementation. Only the compositor is substituted.

const FIXTURE = [
    // id, appId, appName, title, pid, x, y, width, height, workspace, output
    [101, 'org.gnome.Nautilus', 'Files', 'Home', 6101, 0, 0, 1280, 800, 0, 'XWAYLAND0'],
    [102, 'firefox', 'Firefox', 'Vorssaint on GNOME', 6102, 100, 60, 1600, 900, 0, 'XWAYLAND0'],
    [103, 'org.gnome.TextEditor', 'Text Editor', 'notes.md', 6103, 40, 40, 800, 600, 1,
        'XWAYLAND0'],
];

export const NEW_WINDOW = [104, 'org.gnome.Calculator', 'Calculator', 'Calculator', 6104, 10, 10,
    400, 500, 0, 'XWAYLAND0'];

export class FixtureWindowSource {
    constructor() {
        this._windows = FIXTURE.map(window => [...window]);
        this._workspace = 0;
        this._active = 102;
        this._minimized = new Set();
        this._listener = null;
    }

    setListener(listener) {
        this._listener = listener;
    }

    _find(id) {
        return this._windows.find(window => window[0] === Number(id)) ?? null;
    }

    _record(window) {
        return {
            id: window[0],
            appId: window[1],
            appName: window[2],
            title: window[3],
            pid: window[4],
            frame: {x: window[5], y: window[6], width: window[7], height: window[8]},
            minimized: this._minimized.has(window[0]),
            focused: window[0] === this._active,
            fullscreen: false,
            maximized: false,
            onCurrentWorkspace: window[9] === this._workspace,
            hasGeometry: true,
            onScreen: window[9] === this._workspace,
            workspace: window[9],
            output: window[10],
        };
    }

    listWindows() {
        return this._windows.map(window => this._record(window));
    }

    activate(id) {
        const window = this._find(id);
        if (!window) return false;
        this._active = window[0];
        this._minimized.delete(window[0]);
        this._listener?.windowActivated(window[0]);
        return true;
    }

    close(id) {
        const window = this._find(id);
        if (!window) return false;
        this._windows.splice(this._windows.indexOf(window), 1);
        this._listener?.windowRemoved(window[0]);
        return true;
    }

    setMinimized(id, minimized) {
        const window = this._find(id);
        if (!window) return false;
        if (minimized) this._minimized.add(window[0]);
        else this._minimized.delete(window[0]);
        this._listener?.windowChanged(window[0]);
        return true;
    }

    moveResize(id, x, y, width, height) {
        const window = this._find(id);
        if (!window) return false;
        // Mutter clamps a window to its minimum size and a client can refuse
        // outright; Text Editor stands in for one that does, so the backend's
        // read-back has something to catch.
        if (window[1] === 'org.gnome.TextEditor') return true;
        window[5] = x;
        window[6] = y;
        window[7] = width;
        window[8] = height;
        return true;
    }

    geometry(id) {
        const window = this._find(id);
        if (!window) return null;
        return {x: window[5], y: window[6], width: window[7], height: window[8]};
    }

    currentWorkspace() {
        return this._workspace;
    }

    setWorkspace(index) {
        this._workspace = Number(index);
        this._listener?.workspaceChanged(this._workspace);
    }

    // ------------------------------------------------- scripted event replay

    addFixtureWindow() {
        this._windows.push([...NEW_WINDOW]);
        this._listener?.windowAdded(NEW_WINDOW[0]);
    }

    activateFixtureWindow() {
        this._listener?.windowActivated(NEW_WINDOW[0]);
    }

    removeFixtureWindow() {
        const window = this._find(NEW_WINDOW[0]);
        if (window) this._windows.splice(this._windows.indexOf(window), 1);
        this._listener?.windowRemoved(NEW_WINDOW[0]);
    }
}

/** A clipboard with no compositor behind it, for the same reason. */
export class FixtureClipboard {
    constructor() {
        this._data = new Map();
        this._listener = null;
    }

    setListener(listener) {
        this._listener = listener;
    }

    mimeTypes(selection) {
        return [...(this._data.get(selection)?.keys() ?? [])];
    }

    read(selection, mimeType) {
        const bytes = this._data.get(selection)?.get(mimeType);
        return bytes
            ? Promise.resolve(bytes)
            : Promise.reject(new Error(`nothing of type ${mimeType}`));
    }

    write(selection, mimeType, data) {
        this._data.set(selection, new Map([[mimeType, data]]));
        this._listener?.clipboardChanged(selection, [mimeType]);
    }

    clear(selection) {
        this._data.delete(selection);
        this._listener?.clipboardChanged(selection, []);
    }
}
