#!/usr/bin/gjs -m
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// Runs the extension's D-Bus service in a plain gjs process, with a fixture in
// place of Mutter, so the C backend in linux/platform/window/bridge_client.c
// can talk to the real implementation on a machine where gnome-shell cannot
// start. This is what the `window_gnome_extension` ctest drives; it is a
// drop-in replacement for tests/fake_window_bridge.py and prints the same
// `ready` line.
//
//   gjs -m run_bridge.js [--events]

import GLib from 'gi://GLib';
import Gio from 'gi://Gio';

import {WindowBridgeService} from '../lib/service.js';
import {FixtureClipboard, FixtureWindowSource} from './fixture_source.js';

const source = new FixtureWindowSource();
const service = new WindowBridgeService({
    source,
    clipboard: new FixtureClipboard(),
    log: message => printerr(`run_bridge: ${message}`),
});

const loop = new GLib.MainLoop(null, false);

service.start(Gio.DBus.session, {
    onNameAcquired: () => {
        print('ready');
        if (ARGV.includes('--events')) scheduleEvents();
    },
    onNameLost: () => {
        printerr('run_bridge: lost the name');
        loop.quit();
    },
});

/** The same event script fake_window_bridge.py --events plays. */
function scheduleEvents() {
    const at = (ms, callback) => GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
        callback();
        return GLib.SOURCE_REMOVE;
    });
    at(800, () => source.addFixtureWindow());
    at(1800, () => source.activateFixtureWindow());
    at(2800, () => source.removeFixtureWindow());
    at(3400, () => source.setWorkspace(1));
}

loop.run();
service.stop();
