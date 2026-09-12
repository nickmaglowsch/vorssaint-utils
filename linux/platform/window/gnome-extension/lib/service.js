// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// The bus-facing half of the bridge: it owns org.vorssaint.WindowBridge,
// answers the interface in protocol.js, and pushes the signals. It knows
// nothing about Mutter — everything compositor-shaped is behind the two
// collaborators it is handed:
//
//   source     listWindows/activate/close/setMinimized/moveResize/geometry/
//              currentWorkspace/setWorkspace, plus setListener() so it can
//              push events back. lib/mutter.js implements it over Meta;
//              test/fixture_source.js implements it over a fixture.
//   clipboard  mimeTypes/read/write/clear/setListener, or null when the
//              session has no clipboard path.
//
// That split is what lets a plain `gjs` process run this exact file and answer
// the real C client (test/run_bridge.js), on a machine where gnome-shell
// cannot start.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import {
    BRIDGE_NAME, BRIDGE_PATH, BRIDGE_VERSION, INTERFACE_XML, encodeWindowList,
} from './protocol.js';
import {DEFAULT_CHANGE_INTERVAL_MS, Throttler} from './throttle.js';

function noSuchWindow() {
    return Gio.DBusError.new_for_dbus_error(
        'org.freedesktop.DBus.Error.InvalidArgs', 'no such window');
}

function notSupported(what) {
    return Gio.DBusError.new_for_dbus_error(
        'org.freedesktop.DBus.Error.NotSupported', what);
}

function toByteArray(data) {
    if (data instanceof Uint8Array) return data;
    if (data === null || data === undefined) return new Uint8Array(0);
    return Uint8Array.from(data);
}

export class WindowBridgeService {
    constructor({source, clipboard = null, changeIntervalMs = DEFAULT_CHANGE_INTERVAL_MS,
                 log = () => {}}) {
        this._source = source;
        this._clipboard = clipboard;
        this._log = log;
        this._exported = null;
        this._nameId = 0;
        this._connection = null;

        this._changes = new Throttler({
            intervalMs: changeIntervalMs,
            emit: id => this._emit('WindowChanged', '(t)', [id]),
            schedule: (ms, callback) => GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, callback),
            cancel: token => GLib.Source.remove(token),
        });
    }

    /**
     * Export the object on `connection` and take the name. `onNameLost` fires
     * when another process takes it away, which is how the Capabilities page
     * notices a second copy of the extension.
     */
    start(connection, {onNameAcquired = null, onNameLost = null} = {}) {
        this._connection = connection;
        this._exported = Gio.DBusExportedObject.wrapJSObject(INTERFACE_XML, this);
        this._exported.export(connection, BRIDGE_PATH);

        this._source.setListener({
            windowAdded: id => this._emit('WindowAdded', '(t)', [id]),
            windowRemoved: id => {
                this._changes.forget(id);
                this._emit('WindowRemoved', '(t)', [id]);
            },
            windowActivated: id => this._emit('WindowActivated', '(t)', [id]),
            windowChanged: id => this._changes.push(id),
            workspaceChanged: index => this._emit('WorkspaceChanged', '(i)', [index]),
        });

        if (this._clipboard) {
            this._clipboard.setListener({
                clipboardChanged: (selection, mimeTypes) =>
                    this._emit('ClipboardChanged', '(uas)', [selection, mimeTypes]),
            });
        }

        this._nameId = Gio.bus_own_name_on_connection(
            connection, BRIDGE_NAME, Gio.BusNameOwnerFlags.REPLACE,
            () => onNameAcquired?.(), () => onNameLost?.());
    }

    /**
     * Full teardown, in the order that leaves nothing behind: timers first (a
     * timeout that fires into an unexported object is a crash), then the
     * listeners the collaborators hold on us, then the name, then the object.
     * Idempotent, because disable() can follow a failed enable().
     */
    stop() {
        this._changes.destroy();
        this._source?.setListener(null);
        this._clipboard?.setListener(null);
        if (this._nameId) {
            Gio.bus_unown_name(this._nameId);
            this._nameId = 0;
        }
        if (this._exported) {
            this._exported.unexport();
            this._exported = null;
        }
        this._connection = null;
    }

    _emit(name, signature, values) {
        if (!this._exported) return;
        try {
            this._exported.emit_signal(name, new GLib.Variant(signature, values));
        } catch (error) {
            this._log(`emit ${name} failed: ${error}`);
        }
    }

    // ------------------------------------------------------------ properties

    get Version() {
        return BRIDGE_VERSION;
    }

    // --------------------------------------------------------------- windows

    List() {
        return encodeWindowList(this._source.listWindows());
    }

    Activate(id) {
        if (!this._source.activate(Number(id))) throw noSuchWindow();
    }

    Close(id) {
        if (!this._source.close(Number(id))) throw noSuchWindow();
    }

    SetMinimized(id, minimized) {
        if (!this._source.setMinimized(Number(id), !!minimized)) throw noSuchWindow();
    }

    MoveResize(id, x, y, width, height) {
        if (!this._source.moveResize(Number(id), x, y, width, height)) throw noSuchWindow();
    }

    Geometry(id) {
        const frame = this._source.geometry(Number(id));
        if (!frame) throw noSuchWindow();
        return [frame.x, frame.y, frame.width, frame.height];
    }

    CurrentWorkspace() {
        return this._source.currentWorkspace();
    }

    SetWorkspace(index) {
        this._source.setWorkspace(index);
    }

    // ------------------------------------------------------------- clipboard

    ClipboardMimeTypes(selection) {
        if (!this._clipboard) throw notSupported('no clipboard on this session');
        return this._clipboard.mimeTypes(Number(selection));
    }

    /** Async: reading a selection is a transfer from the owning client. */
    ClipboardReadAsync([selection, mimeType], invocation) {
        if (!this._clipboard) {
            invocation.return_gerror(notSupported('no clipboard on this session'));
            return;
        }
        this._clipboard.read(Number(selection), mimeType)
            .then(bytes => invocation.return_value(
                new GLib.Variant('(ay)', [toByteArray(bytes)])))
            .catch(error => {
                this._log(`ClipboardRead failed: ${error}`);
                invocation.return_gerror(error instanceof GLib.Error
                    ? error
                    : Gio.DBusError.new_for_dbus_error(
                        'org.freedesktop.DBus.Error.Failed', String(error)));
            });
    }

    ClipboardWrite(selection, mimeType, data) {
        if (!this._clipboard) throw notSupported('no clipboard on this session');
        this._clipboard.write(Number(selection), mimeType, toByteArray(data));
    }

    ClipboardClear(selection) {
        if (!this._clipboard) throw notSupported('no clipboard on this session');
        this._clipboard.clear(Number(selection));
    }
}
