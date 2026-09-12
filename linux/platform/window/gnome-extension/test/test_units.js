#!/usr/bin/gjs -m
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// Unit tests for the parts of the extension that do not need Mutter: the
// introspection XML (parsed and compared member by member against the
// signatures bridge_client.h reads), the flag and struct encoding, the change
// throttling, and the id allocator. Plain gjs, no GNOME Shell.
//
//   gjs -m test_units.js

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import system from 'system';

import {
    BRIDGE_INTERFACE, FLAG, INTERFACE_XML, SELECTION, WINDOW_ARRAY_SIGNATURE,
    encodeWindow, encodeWindowList, windowFlags,
} from '../lib/protocol.js';
import {Throttler} from '../lib/throttle.js';
import {IdAllocator, SYNTHETIC_ID_BASE} from '../lib/ids.js';

let failures = 0;
let checks = 0;

function check(condition, what) {
    checks++;
    if (condition) return;
    failures++;
    printerr(`FAIL: ${what}`);
}

function equal(actual, expected, what) {
    const a = JSON.stringify(actual);
    const b = JSON.stringify(expected);
    check(a === b, `${what}: expected ${b}, got ${a}`);
}

// --------------------------------------------------------------- interface

function signatureOf(args) {
    return args.map(arg => arg.signature).join('');
}

function testInterfaceXml() {
    const node = Gio.DBusNodeInfo.new_for_xml(INTERFACE_XML);
    const iface = node.lookup_interface(BRIDGE_INTERFACE);
    check(iface !== null, 'the XML declares org.vorssaint.WindowBridge');

    // Exactly the members bridge_client.c calls, with exactly its signatures.
    const expected = {
        List: ['', WINDOW_ARRAY_SIGNATURE],
        Activate: ['t', ''],
        Close: ['t', ''],
        SetMinimized: ['tb', ''],
        MoveResize: ['tiiii', ''],
        Geometry: ['t', '(iiii)'],
        CurrentWorkspace: ['', 'i'],
        SetWorkspace: ['i', ''],
        ClipboardMimeTypes: ['u', 'as'],
        ClipboardRead: ['us', 'ay'],
        ClipboardWrite: ['usay', ''],
        ClipboardClear: ['u', ''],
    };
    for (const [name, [inSignature, outSignature]] of Object.entries(expected)) {
        const method = iface.lookup_method(name);
        check(method !== null, `${name} is declared`);
        if (!method) continue;
        equal(signatureOf(method.in_args), inSignature, `${name} in signature`);
        equal(signatureOf(method.out_args), outSignature, `${name} out signature`);
    }
    equal(iface.methods.map(m => m.name).sort(), Object.keys(expected).sort(),
        'no member beyond the interface WP-C1 defined');

    const signals = {
        WindowAdded: 't', WindowRemoved: 't', WindowActivated: 't', WindowChanged: 't',
        WorkspaceChanged: 'i', ClipboardChanged: 'uas',
    };
    for (const [name, signature] of Object.entries(signals)) {
        const signal = iface.lookup_signal(name);
        check(signal !== null, `${name} is declared`);
        if (signal) equal(signatureOf(signal.args), signature, `${name} signature`);
    }
    equal(iface.signals.map(s => s.name).sort(), Object.keys(signals).sort(),
        'no signal beyond the interface WP-C1 defined');
}

// -------------------------------------------------------------------- flags

function testFlags() {
    equal(windowFlags({}), FLAG.HAS_GEOMETRY, 'a bare record claims only geometry');
    equal(windowFlags({pid: 42}), FLAG.HAS_GEOMETRY | FLAG.HAS_PID, 'a real pid sets HAS_PID');
    equal(windowFlags({pid: -1}), FLAG.HAS_GEOMETRY, 'the -1 placeholder does not set HAS_PID');
    equal(windowFlags({hasGeometry: false}), 0, 'a record without a frame says so');

    // A minimized window is never on screen, whatever the source claims.
    equal(windowFlags({minimized: true, onScreen: true, hasGeometry: false}), FLAG.MINIMIZED,
        'minimized clears ON_SCREEN');

    equal(windowFlags({
        focused: true, fullscreen: true, maximized: true, onCurrentWorkspace: true,
        onScreen: true, pid: 7,
    }), FLAG.FOCUSED | FLAG.FULLSCREEN | FLAG.MAXIMIZED | FLAG.ON_CURRENT_WORKSPACE |
        FLAG.HAS_GEOMETRY | FLAG.HAS_PID | FLAG.ON_SCREEN, 'every flag together');
}

// ------------------------------------------------------------------ encoding

function testEncoding() {
    const encoded = encodeWindow({
        id: 102, appId: 'firefox', appName: 'Firefox', title: 'Vorssaint on GNOME', pid: 6102,
        frame: {x: 100, y: 60, width: 1600, height: 900},
        focused: true, onCurrentWorkspace: true, onScreen: true, workspace: 0,
        output: 'XWAYLAND0',
    });
    equal(encoded, [102, 'firefox', 'Firefox', 'Vorssaint on GNOME', 6102, 100, 60, 1600, 900,
        FLAG.FOCUSED | FLAG.ON_CURRENT_WORKSPACE | FLAG.HAS_GEOMETRY | FLAG.HAS_PID |
        FLAG.ON_SCREEN, 0, 'XWAYLAND0'], 'the struct is positional and in the header order');

    // The encoding must survive the marshaller the C client reads from.
    const variant = new GLib.Variant(WINDOW_ARRAY_SIGNATURE, encodeWindowList([
        {id: 1, appId: 'a', title: 't', pid: 2, frame: {x: 1, y: 2, width: 3, height: 4},
            workspace: 0, output: 'DP-1'},
    ]));
    equal(variant.get_type_string(), WINDOW_ARRAY_SIGNATURE, 'List() marshals as a(tsssiiiiiuis)');

    const missing = encodeWindow({id: 5});
    equal(missing[1], '', 'a missing app_id is an empty string, not null');
    equal(missing[2], '', 'app_name falls back to app_id');
    equal(missing[4], -1, 'a missing pid is the -1 placeholder');
    equal(missing[10], -1, 'a missing workspace is -1');

    equal(encodeWindow({id: 5, appId: 'x'})[2], 'x', 'app_name defaults to app_id');

    // A title Mutter hands over as null must not become "null" on the bus.
    equal(encodeWindow({id: 5, title: null})[3], '', 'a null title encodes as empty');

    equal(SELECTION.CLIPBOARD, 0, 'selection 0 is the clipboard');
    equal(SELECTION.PRIMARY, 1, 'selection 1 is the primary selection');
}

// ---------------------------------------------------------------- throttling

function testThrottle() {
    const emitted = [];
    const timers = new Map();
    let nextToken = 1;
    const throttler = new Throttler({
        intervalMs: 100,
        emit: key => emitted.push(key),
        schedule: (_ms, callback) => {
            const token = nextToken++;
            timers.set(token, callback);
            return token;
        },
        cancel: token => timers.delete(token),
    });
    const fire = () => {
        for (const [token, callback] of [...timers]) {
            timers.delete(token);
            callback();
        }
    };

    // Leading edge: the first change is immediate, so the switcher is not lagging.
    throttler.push('a');
    equal(emitted, ['a'], 'the first change goes out at once');

    // A drag: many changes inside one interval collapse to one trailing emission.
    for (let i = 0; i < 50; i++) throttler.push('a');
    equal(emitted, ['a'], 'changes inside the interval are coalesced');
    fire();
    equal(emitted, ['a', 'a'], 'exactly one trailing emission for the whole burst');

    // Quiet interval: the entry retires instead of emitting forever.
    fire();
    equal(emitted, ['a', 'a'], 'a quiet interval emits nothing');
    equal(throttler.pendingCount, 0, 'the entry retires when the burst ends');

    // Keys are independent.
    throttler.push('b');
    throttler.push('c');
    equal(emitted, ['a', 'a', 'b', 'c'], 'each window throttles on its own');

    // A window that closes mid-burst must not emit a change after its removal.
    throttler.push('b');
    throttler.forget('b');
    fire();
    equal(emitted, ['a', 'a', 'b', 'c'], 'a forgotten key never emits again');

    // disable() leaves no armed timer behind.
    throttler.push('d');
    throttler.push('d');
    throttler.destroy();
    equal(timers.size, 0, 'destroy cancels every armed timer');
    equal(throttler.pendingCount, 0, 'destroy drops every entry');
}

// ------------------------------------------------------------------ id table

function testIds() {
    const windows = [{name: 'a'}, {name: 'b'}, {name: 'c'}];
    const native = new Map([[windows[0], 17], [windows[1], 18]]);
    const ids = new IdAllocator({nativeId: window => native.get(window) ?? null});

    equal(ids.idFor(windows[0]), 17, "Mutter's own id is used when there is one");
    equal(ids.idFor(windows[0]), 17, 'the same window keeps the same id');
    equal(ids.idFor(windows[2]), SYNTHETIC_ID_BASE,
        'a window with no native id gets one from the synthetic range');
    check(ids.windowFor(17) === windows[0], 'the reverse map resolves an id');
    check(ids.windowFor(999) === null, 'an unknown id resolves to nothing');

    equal(ids.forget(windows[0]), 17, 'forget returns the id that went away');
    check(ids.windowFor(17) === null, 'a forgotten id no longer resolves');
    equal(ids.forget(windows[0]), null, 'forgetting twice is harmless');

    // A reused Mutter id must not hand two live windows the same id.
    const reused = {name: 'd'};
    native.set(reused, 18);
    ids.idFor(windows[1]);
    equal(ids.idFor(reused), SYNTHETIC_ID_BASE + 1,
        'a native id already in use falls back to the synthetic range');

    ids.clear();
    equal(ids.size, 0, 'clear drops every window reference');
}


testInterfaceXml();
testFlags();
testEncoding();
testThrottle();
testIds();

print(`${checks - failures}/${checks} checks passed`);
if (failures > 0) {
    printerr(`FAIL: ${failures} check(s) failed`);
    system.exit(1);
}
print('PASS: gnome-extension unit tests');
