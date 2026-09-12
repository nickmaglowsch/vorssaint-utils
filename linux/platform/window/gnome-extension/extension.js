// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// vorssaint-bridge: the GNOME half of org.vorssaint.WindowBridge.
//
// GNOME implements no foreign-toplevel protocol and no data-control protocol,
// so on GNOME this extension is the only path the app has to the window list
// and to the clipboard. It is deliberately a thin facade over Meta: it holds
// no state of its own beyond the id table, runs no code it is given, and
// touches no files.
//
// GNOME 45+ ESM style: `import`, and an `Extension` subclass whose enable()
// and disable() are exact mirrors of each other. disable() must leave nothing
// behind — GNOME runs it on lock screen too — so every signal handler, every
// timeout and the bus name are given back there.

import Gio from 'gi://Gio';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

import {WindowBridgeService} from './lib/service.js';
import {MutterClipboard, MutterWindowSource} from './lib/mutter.js';

export default class VorssaintBridgeExtension extends Extension {
    enable() {
        const log = message => console.log(`vorssaint-bridge: ${message}`);

        this._source = new MutterWindowSource({log});
        this._clipboard = new MutterClipboard({log});
        this._source.enable();
        this._clipboard.enable();

        this._service = new WindowBridgeService({
            source: this._source,
            clipboard: this._clipboard,
            log,
        });
        this._service.start(Gio.DBus.session, {
            onNameLost: () => log(
                'lost org.vorssaint.WindowBridge; another bridge is running'),
        });
    }

    disable() {
        this._service?.stop();
        this._service = null;
        this._clipboard?.disable();
        this._clipboard = null;
        this._source?.disable();
        this._source = null;
    }
}
