// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// Everything that touches Mutter. This is the only file in the extension that
// imports Meta, Shell or gnome-shell's own modules, which is what keeps the
// rest of the code testable under plain gjs.
//
// It implements the `source` and `clipboard` collaborators lib/service.js
// expects. Every Meta/Shell member used here is asserted to exist by
// test/check_mutter_api.js, which runs against the installed introspection
// data; a GNOME version bump has one file to re-check and one test to run.

import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import GLib from 'gi://GLib';
import Gio from 'gi://Gio';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {SELECTION} from './protocol.js';
import {IdAllocator} from './ids.js';

/** Window types the switcher should see. Everything else (docks, panels,
 *  tooltips, the Shell's own chrome) is not a user window. */
const LISTED_TYPES = new Set([
    Meta.WindowType.NORMAL,
    Meta.WindowType.DIALOG,
    Meta.WindowType.MODAL_DIALOG,
]);

const SELECTION_TYPES = {
    [SELECTION.CLIPBOARD]: Meta.SelectionType.SELECTION_CLIPBOARD,
    [SELECTION.PRIMARY]: Meta.SelectionType.SELECTION_PRIMARY,
};

/**
 * Monitor index -> connector name ("DP-1"), for the `output` field the
 * switcher filters displays on.
 *
 * Mutter's introspected MonitorManager maps a connector to an index
 * (`get_monitor_for_connector`) but not the other way, and GNOME 46's
 * introspection data exposes no monitor list at all. The connector names
 * therefore come from Mutter's own
 * `org.gnome.Mutter.DisplayConfig.GetCurrentState` and the index for each one
 * comes back from `get_monitor_for_connector`, so no ordering is assumed
 * between the two lists. Until that async answer arrives — and on any session
 * where the call fails — a monitor is named `monitor-N`, which is still stable
 * for the session and is all display filtering needs.
 */
class MonitorNames {
    constructor({log = () => {}} = {}) {
        this._log = log;
        this._byIndex = new Map();
        this._cancellable = null;
        this._handler = 0;
        this._manager = null;
    }

    enable() {
        this._manager = global.backend?.get_monitor_manager?.() ?? null;
        if (this._manager)
            this._handler = this._manager.connect('monitors-changed', () => this.refresh());
        this.refresh();
    }

    disable() {
        this._cancellable?.cancel();
        this._cancellable = null;
        if (this._manager && this._handler) this._manager.disconnect(this._handler);
        this._handler = 0;
        this._manager = null;
        this._byIndex.clear();
    }

    nameFor(index) {
        if (index < 0) return '';
        return this._byIndex.get(index) ?? `monitor-${index}`;
    }

    refresh() {
        if (!this._manager?.get_monitor_for_connector) return;
        this._cancellable?.cancel();
        this._cancellable = new Gio.Cancellable();
        Gio.DBus.session.call(
            'org.gnome.Mutter.DisplayConfig', '/org/gnome/Mutter/DisplayConfig',
            'org.gnome.Mutter.DisplayConfig', 'GetCurrentState', null, null,
            Gio.DBusCallFlags.NONE, 2000, this._cancellable,
            (connection, result) => {
                try {
                    const reply = connection.call_finish(result);
                    this._byIndex.clear();
                    for (const monitor of reply.get_child_value(1).deepUnpack()) {
                        const connector = monitor[0][0];
                        const index = this._manager.get_monitor_for_connector(connector);
                        if (index >= 0) this._byIndex.set(index, connector);
                    }
                } catch (error) {
                    if (!error.matches?.(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        this._log(`monitor names unavailable: ${error}`);
                }
            });
    }
}

export class MutterWindowSource {
    constructor({log = () => {}} = {}) {
        this._log = log;
        this._listener = null;
        this._monitors = new MonitorNames({log});
        this._ids = new IdAllocator({
            // meta_window_get_id() is Mutter's own stable handle for a window;
            // the allocator falls back to its own counter if a build drops it.
            nativeId: window => {
                if (typeof window.get_id !== 'function') return null;
                return Number(window.get_id());
            },
        });
        // window -> [handler ids], plus our handlers on display/workspaces.
        this._windowHandlers = new Map();
        this._globalHandlers = [];
    }

    // ------------------------------------------------------------ lifecycle

    enable() {
        const display = global.display;
        const workspaceManager = global.workspace_manager;
        this._monitors.enable();

        this._globalHandlers.push([display, display.connect(
            'window-created', (_display, window) => this._onWindowCreated(window))]);
        this._globalHandlers.push([display, display.connect(
            'notify::focus-window', () => this._onFocusChanged())]);
        this._globalHandlers.push([workspaceManager, workspaceManager.connect(
            'active-workspace-changed', () => this._listener?.workspaceChanged(
                workspaceManager.get_active_workspace_index()))]);

        for (const window of this._metaWindows()) this._track(window);
    }

    disable() {
        this._monitors.disable();
        for (const [object, handler] of this._globalHandlers) object.disconnect(handler);
        this._globalHandlers = [];
        for (const [window, handlers] of this._windowHandlers)
            for (const handler of handlers) window.disconnect(handler);
        this._windowHandlers.clear();
        this._ids.clear();
        this._listener = null;
    }

    setListener(listener) {
        this._listener = listener;
    }

    // ------------------------------------------------------------- tracking

    _onWindowCreated(window) {
        this._track(window);
        if (this._isListed(window)) this._listener?.windowAdded(this._ids.idFor(window));
    }

    _track(window) {
        if (this._windowHandlers.has(window)) return;
        const changed = () => {
            if (this._isListed(window)) this._listener?.windowChanged(this._ids.idFor(window));
        };
        const handlers = [
            window.connect('unmanaged', () => this._onUnmanaged(window)),
            window.connect('notify::title', changed),
            window.connect('notify::minimized', changed),
            window.connect('notify::wm-class', changed),
            window.connect('position-changed', changed),
            window.connect('size-changed', changed),
            window.connect('workspace-changed', changed),
        ];
        this._windowHandlers.set(window, handlers);
        // Give it an id now, so added and removed name the same window.
        this._ids.idFor(window);
    }

    _onUnmanaged(window) {
        const handlers = this._windowHandlers.get(window);
        if (handlers) {
            for (const handler of handlers) window.disconnect(handler);
            this._windowHandlers.delete(window);
        }
        const id = this._ids.forget(window);
        if (id !== null) this._listener?.windowRemoved(id);
    }

    _onFocusChanged() {
        const window = global.display.focus_window;
        if (!window || !this._isListed(window)) return;
        this._listener?.windowActivated(this._ids.idFor(window));
    }

    // ---------------------------------------------------------------- model

    /** Mutter's own bottom-to-top stacking order. Unlike the wlroots
     *  protocols, this is a real stacking order, which is why the GNOME
     *  backend marks `stacking_valid`. */
    _metaWindows() {
        return global.get_window_actors()
            .map(actor => actor.meta_window ?? actor.get_meta_window())
            .filter(window => !!window);
    }

    _isListed(window) {
        if (!LISTED_TYPES.has(window.get_window_type())) return false;
        return !window.is_skip_taskbar();
    }

    _find(id) {
        const window = this._ids.windowFor(id);
        if (!window) return null;
        // An id whose window closed between List() and this call is not
        // something the caller can act on differently from an unknown one.
        return this._windowHandlers.has(window) ? window : null;
    }

    _describe(window) {
        const app = Shell.WindowTracker.get_default().get_window_app(window);
        const frame = window.get_frame_rect();
        const workspace = window.get_workspace();
        const activeIndex = global.workspace_manager.get_active_workspace_index();
        const workspaceIndex = workspace ? workspace.index() : -1;
        const wmClass = window.get_wm_class_instance() || window.get_wm_class() || '';

        return {
            id: this._ids.idFor(window),
            // The desktop file id is what per-app rules key on and what the
            // icon is looked up by; WM_CLASS is the fallback for a window no
            // .desktop file claims.
            appId: app ? app.get_id().replace(/\.desktop$/, '') : wmClass,
            appName: app ? app.get_name() : (window.get_wm_class() || wmClass),
            title: window.get_title() || '',
            pid: window.get_pid() || -1,
            frame: {x: frame.x, y: frame.y, width: frame.width, height: frame.height},
            minimized: window.minimized,
            focused: window.has_focus(),
            fullscreen: window.is_fullscreen(),
            maximized: window.get_maximized() === Meta.MaximizeFlags.BOTH,
            onCurrentWorkspace: workspaceIndex < 0 || workspaceIndex === activeIndex,
            hasGeometry: true,
            onScreen: !window.minimized && !window.is_hidden(),
            workspace: workspaceIndex,
            output: this._monitors.nameFor(window.get_monitor()),
        };
    }

    listWindows() {
        return this._metaWindows().filter(w => this._isListed(w)).map(w => this._describe(w));
    }

    // ------------------------------------------------------------- commands

    /**
     * A D-Bus call carries no input event, so `global.get_current_time()` is 0
     * and Mutter's focus-stealing prevention would refuse the activation. The
     * roundtrip gives a real server timestamp, which is the documented way to
     * activate a window from outside an event handler.
     */
    _timestamp() {
        const time = global.get_current_time();
        return time !== 0 ? time : global.display.get_current_time_roundtrip();
    }

    activate(id) {
        const window = this._find(id);
        if (!window) return false;
        // activateWindow, not window.activate: it also switches to the
        // window's workspace and closes the overview, which is what the user
        // means by "switch to this window".
        Main.activateWindow(window, this._timestamp());
        return true;
    }

    close(id) {
        const window = this._find(id);
        if (!window) return false;
        window.delete(this._timestamp());
        return true;
    }

    setMinimized(id, minimized) {
        const window = this._find(id);
        if (!window) return false;
        if (minimized) window.minimize();
        else window.unminimize();
        return true;
    }

    moveResize(id, x, y, width, height) {
        const window = this._find(id);
        if (!window) return false;
        // Mutter silently ignores a move on a maximized, tiled or fullscreen
        // window, so those states go first. The caller's read-back through
        // Geometry() is still what decides whether it worked.
        if (window.get_maximized() !== 0) window.unmaximize(Meta.MaximizeFlags.BOTH);
        if (window.is_fullscreen()) window.unmake_fullscreen();
        // user_op = true: Mutter records it as the user's own rect, so the
        // frame survives a later unmaximize instead of snapping back.
        window.move_resize_frame(true, x, y, width, height);
        return true;
    }

    geometry(id) {
        const window = this._find(id);
        if (!window) return null;
        const frame = window.get_frame_rect();
        return {x: frame.x, y: frame.y, width: frame.width, height: frame.height};
    }

    currentWorkspace() {
        return global.workspace_manager.get_active_workspace_index();
    }

    setWorkspace(index) {
        const manager = global.workspace_manager;
        if (index < 0 || index >= manager.get_n_workspaces()) return;
        manager.get_workspace_by_index(index).activate(this._timestamp());
    }
}

/**
 * The clipboard half (WP-A8). GNOME implements no data-control protocol, so
 * `Meta.Selection` — the same path GPaste uses — is the only way to read the
 * selection without owning the focus. Mutter backs it with the Wayland
 * selection and, on an X11 session, with the X11 selection, so one code path
 * covers both.
 */
export class MutterClipboard {
    constructor({log = () => {}} = {}) {
        this._log = log;
        this._listener = null;
        this._selection = null;
        this._handler = 0;
        // Sources we set ourselves. ClipboardClear can only drop our own
        // ownership: Mutter's unset_owner takes the source, and there is no
        // introspected way to fetch somebody else's.
        this._owned = new Map();
    }

    enable() {
        this._selection = global.display.get_selection();
        this._handler = this._selection.connect('owner-changed', (_selection, type, source) => {
            let index = -1;
            if (type === Meta.SelectionType.SELECTION_CLIPBOARD) index = SELECTION.CLIPBOARD;
            else if (type === Meta.SelectionType.SELECTION_PRIMARY) index = SELECTION.PRIMARY;
            if (index < 0) return;
            this._listener?.clipboardChanged(index, source ? source.get_mimetypes() : []);
        });
    }

    disable() {
        if (this._selection && this._handler) this._selection.disconnect(this._handler);
        this._handler = 0;
        this._selection = null;
        this._listener = null;
        this._owned.clear();
    }

    setListener(listener) {
        this._listener = listener;
    }

    _type(selection) {
        const type = SELECTION_TYPES[selection];
        if (type === undefined) throw new Error(`unknown selection ${selection}`);
        return type;
    }

    mimeTypes(selection) {
        return this._selection.get_mimetypes(this._type(selection));
    }

    read(selection, mimeType) {
        return new Promise((resolve, reject) => {
            const stream = Gio.MemoryOutputStream.new_resizable();
            this._selection.transfer_async(
                this._type(selection), mimeType, -1, stream, null, (source, result) => {
                    try {
                        source.transfer_finish(result);
                        stream.close(null);
                        resolve(stream.steal_as_bytes().toArray());
                    } catch (error) {
                        reject(error);
                    }
                });
        });
    }

    write(selection, mimeType, data) {
        const type = this._type(selection);
        const source = Meta.SelectionSourceMemory.new(mimeType, GLib.Bytes.new(data));
        this._selection.set_owner(type, source);
        this._owned.set(type, source);
    }

    clear(selection) {
        const type = this._type(selection);
        const source = this._owned.get(type);
        if (!source) return;
        this._selection.unset_owner(type, source);
        this._owned.delete(type);
    }
}
