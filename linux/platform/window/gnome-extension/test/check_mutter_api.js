#!/usr/bin/gjs -m
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// Asserts that every Meta and Shell member lib/mutter.js calls actually exists
// in the GNOME introspection data installed on this machine.
//
// This is the only automated defence against the risk PLAN.md § 9 names — the
// extension breaking on a new GNOME release — that does not need a session:
// gnome-shell cannot start here (no DRM, no seat), but its typelibs describe
// the API exactly, and a removed or renamed method fails this test instead of
// failing silently in front of a user. Run it against each Shell version CI
// covers.
//
// Exits 77 (ctest's skip) where no Mutter introspection data is installed.

import GIRepository from 'gi://GIRepository';
import GLib from 'gi://GLib';
import Gio from 'gi://Gio';
import system from 'system';

/** Private typelib directories Mutter and GNOME Shell install into. */
const SEARCH_GLOBS = [
    '/usr/lib64/mutter-*', '/usr/lib/mutter-*', '/usr/lib/*/mutter-*',
    '/usr/lib64/gnome-shell', '/usr/lib/gnome-shell', '/usr/lib/*/gnome-shell',
];

function expandGlob(pattern) {
    const slash = pattern.lastIndexOf('/');
    const parent = pattern.slice(0, slash);
    const leaf = pattern.slice(slash + 1);
    if (parent.includes('*')) {
        return expandGlob(parent).flatMap(dir => expandGlob(`${dir}/${leaf}`));
    }
    if (!GLib.file_test(parent, GLib.FileTest.IS_DIR)) return [];
    if (!leaf.includes('*')) {
        const path = `${parent}/${leaf}`;
        return GLib.file_test(path, GLib.FileTest.IS_DIR) ? [path] : [];
    }
    const prefix = leaf.slice(0, leaf.indexOf('*'));
    const names = [];
    const enumerator = Gio.File.new_for_path(parent).enumerate_children(
        'standard::name', Gio.FileQueryInfoFlags.NONE, null);
    let info;
    while ((info = enumerator.next_file(null)) !== null) {
        const path = `${parent}/${info.get_name()}`;
        if (info.get_name().startsWith(prefix) && GLib.file_test(path, GLib.FileTest.IS_DIR))
            names.push(path);
    }
    return names;
}

const TYPELIB_DIRS = SEARCH_GLOBS.flatMap(expandGlob);
for (const directory of TYPELIB_DIRS) GIRepository.Repository.prepend_search_path(directory);

// Mutter and GNOME Shell install their shared libraries beside their typelibs,
// off the default loader path, so the typelib loads and every call through it
// then fails. The loader reads LD_LIBRARY_PATH only at process start, so the
// one way to check the Shell API at all is to put it there and start again.
if (!GLib.getenv('VS_MUTTER_API_RELAUNCHED') && TYPELIB_DIRS.length > 0) {
    const path = [...TYPELIB_DIRS, GLib.getenv('LD_LIBRARY_PATH') ?? ''].filter(Boolean).join(':');
    const [, out, err, status] = GLib.spawn_sync(
        null, ['gjs', '-m', system.programPath],
        [...GLib.get_environ().filter(entry => !entry.startsWith('LD_LIBRARY_PATH=')),
            `LD_LIBRARY_PATH=${path}`, 'VS_MUTTER_API_RELAUNCHED=1'],
        GLib.SpawnFlags.SEARCH_PATH, null);
    const decoder = new TextDecoder();
    if (out.length) print(decoder.decode(out).trimEnd());
    if (err.length) printerr(decoder.decode(err).trimEnd());
    system.exit(status === 0 ? 0 : (status >> 8) || 1);
}

function typelibVersion(namespace) {
    const directories = [...SEARCH_GLOBS.flatMap(expandGlob), '/usr/lib64/girepository-1.0',
        '/usr/lib/girepository-1.0', ...expandGlob('/usr/lib/*/girepository-1.0')];
    const versions = [];
    for (const directory of directories) {
        if (!GLib.file_test(directory, GLib.FileTest.IS_DIR)) continue;
        const enumerator = Gio.File.new_for_path(directory).enumerate_children(
            'standard::name', Gio.FileQueryInfoFlags.NONE, null);
        let info;
        while ((info = enumerator.next_file(null)) !== null) {
            const match = /^(.+)-(.+)\.typelib$/.exec(info.get_name());
            if (match && match[1] === namespace) versions.push(match[2]);
        }
    }
    versions.sort((a, b) => Number(b) - Number(a));
    return versions[0] ?? null;
}

const metaVersion = typelibVersion('Meta');
const shellVersion = typelibVersion('Shell');
if (!metaVersion) {
    printerr('SKIP: no Meta typelib installed; nothing to check the Mutter API against');
    system.exit(77);
}

const Meta = (await import(`gi://Meta?version=${metaVersion}`)).default;
let Shell = null;
try {
    Shell = (await import(`gi://Shell?version=${shellVersion}`)).default;
} catch (error) {
    printerr(`note: Shell-${shellVersion} not loadable here (${error.message.split('\n')[0]})`);
}

let failures = 0;
let checks = 0;

function has(present, what) {
    checks++;
    if (present) return;
    failures++;
    printerr(`FAIL: ${what}`);
}

function methods(klass, names, label) {
    for (const name of names)
        has(typeof klass?.prototype?.[name] === 'function', `${label}.${name}()`);
}

function statics(klass, names, label) {
    for (const name of names) has(typeof klass?.[name] === 'function', `${label}.${name}()`);
}

function enums(object, names, label) {
    for (const name of names) has(object?.[name] !== undefined, `${label}.${name}`);
}

// lib/mutter.js: MutterWindowSource
methods(Meta.Window, [
    'get_id', 'get_title', 'get_pid', 'get_frame_rect', 'get_wm_class', 'get_wm_class_instance',
    'get_window_type', 'is_skip_taskbar', 'has_focus', 'is_fullscreen', 'is_hidden',
    'get_maximized', 'get_workspace', 'get_monitor', 'minimize', 'unminimize', 'unmaximize',
    'unmake_fullscreen', 'move_resize_frame', 'delete',
], 'Meta.Window');
methods(Meta.WindowActor, ['get_meta_window'], 'Meta.WindowActor');
methods(Meta.Display, ['get_current_time_roundtrip', 'get_selection'], 'Meta.Display');
methods(Meta.WorkspaceManager, [
    'get_active_workspace_index', 'get_n_workspaces', 'get_workspace_by_index',
], 'Meta.WorkspaceManager');
methods(Meta.Workspace, ['index', 'activate'], 'Meta.Workspace');
methods(Meta.MonitorManager, ['get_monitor_for_connector'], 'Meta.MonitorManager');
methods(Meta.Backend, ['get_monitor_manager'], 'Meta.Backend');
enums(Meta.WindowType, ['NORMAL', 'DIALOG', 'MODAL_DIALOG'], 'Meta.WindowType');
enums(Meta.MaximizeFlags, ['BOTH'], 'Meta.MaximizeFlags');

// lib/mutter.js: MutterClipboard
methods(Meta.Selection, ['get_mimetypes', 'set_owner', 'unset_owner', 'transfer_async',
    'transfer_finish'], 'Meta.Selection');
methods(Meta.SelectionSource, ['get_mimetypes'], 'Meta.SelectionSource');
statics(Meta.SelectionSourceMemory, ['new'], 'Meta.SelectionSourceMemory');
enums(Meta.SelectionType, ['SELECTION_CLIPBOARD', 'SELECTION_PRIMARY'], 'Meta.SelectionType');

// Signals the extension connects to. A renamed signal is a silent failure at
// runtime — connect() on an unknown signal only warns — so it is checked here.
const GObject = (await import('gi://GObject')).default;
const signals = [
    [Meta.Display, 'window-created'], [Meta.Display, 'notify'],
    [Meta.WorkspaceManager, 'active-workspace-changed'],
    [Meta.Window, 'unmanaged'], [Meta.Window, 'position-changed'],
    [Meta.Window, 'size-changed'], [Meta.Window, 'workspace-changed'],
    [Meta.Selection, 'owner-changed'], [Meta.MonitorManager, 'monitors-changed'],
];
for (const [klass, name] of signals)
    has(GObject.signal_lookup(name, klass.$gtype) !== 0, `signal ${klass.name}::${name}`);

// notify:: handlers only work if the property is really there. gjs binds no
// g_object_class_find_property, but it does put an accessor for every GObject
// property on the prototype, which is an equivalent check (an unknown name is
// absent there).
for (const [klass, property] of [
    [Meta.Window, 'title'], [Meta.Window, 'minimized'], [Meta.Window, 'wm-class'],
    [Meta.Display, 'focus-window'],
]) {
    const snake = property.replace(/-/g, '_');
    has(snake in klass.prototype, `property ${klass.name}::${property}`);
}

if (Shell) {
    methods(Shell.WindowTracker, ['get_window_app'], 'Shell.WindowTracker');
    statics(Shell.WindowTracker, ['get_default'], 'Shell.WindowTracker');
    methods(Shell.App, ['get_id', 'get_name'], 'Shell.App');
}

print(`Meta-${metaVersion}${Shell ? `, Shell-${shellVersion}` : ''}: ` +
      `${checks - failures}/${checks} API members present`);
if (failures > 0) {
    printerr(`FAIL: ${failures} member(s) missing`);
    system.exit(1);
}
print('PASS: every Meta/Shell member the extension uses exists');
