// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// The wire format of org.vorssaint.WindowBridge, with nothing from GNOME in
// it. This module is the half of the extension that can be unit-tested under
// plain gjs: the introspection XML, the flag bitmask, and the encoding of a
// window record into the `(tsssiiiiiuis)` struct the C client reads in
// linux/platform/window/bridge_client.c.
//
// The interface is fixed by WP-C1, which shipped the client first. Do not
// reorder or retype a member here without changing bridge_client.h in the same
// commit: the C side reads the struct positionally.

export const BRIDGE_NAME = 'org.vorssaint.WindowBridge';
export const BRIDGE_PATH = '/org/vorssaint/WindowBridge';
export const BRIDGE_INTERFACE = 'org.vorssaint.WindowBridge';

/** Bumped when a member is added. The Capabilities page (WP-25) reads it to
 *  tell a stale installed extension from a current one. */
export const BRIDGE_VERSION = 1;

/** Mirrors vs_window_flag in linux/platform/include/vorssaint_platform.h. */
export const FLAG = {
    MINIMIZED: 1 << 0,
    FOCUSED: 1 << 1,
    FULLSCREEN: 1 << 2,
    MAXIMIZED: 1 << 3,
    ON_CURRENT_WORKSPACE: 1 << 4,
    HAS_GEOMETRY: 1 << 5,
    HAS_PID: 1 << 6,
    ON_SCREEN: 1 << 7,
};

/** `selection` argument of the clipboard members. */
export const SELECTION = {
    CLIPBOARD: 0,
    PRIMARY: 1,
};

export const WINDOW_SIGNATURE = 'tsssiiiiiuis';
export const WINDOW_ARRAY_SIGNATURE = `a(${WINDOW_SIGNATURE})`;

export const INTERFACE_XML = `
<node>
  <interface name="${BRIDGE_INTERFACE}">
    <!-- Every window Mutter would show in the overview, in Mutter's own
         stacking order, bottom to top. -->
    <method name="List">
      <arg type="${WINDOW_ARRAY_SIGNATURE}" direction="out" name="windows"/>
    </method>
    <method name="Activate">
      <arg type="t" direction="in" name="window"/>
    </method>
    <method name="Close">
      <arg type="t" direction="in" name="window"/>
    </method>
    <method name="SetMinimized">
      <arg type="t" direction="in" name="window"/>
      <arg type="b" direction="in" name="minimized"/>
    </method>
    <!-- Returns as soon as Mutter has been asked. Mutter may clamp or refuse
         the frame, so only a Geometry read-back tells the caller the truth. -->
    <method name="MoveResize">
      <arg type="t" direction="in" name="window"/>
      <arg type="i" direction="in" name="x"/>
      <arg type="i" direction="in" name="y"/>
      <arg type="i" direction="in" name="width"/>
      <arg type="i" direction="in" name="height"/>
    </method>
    <method name="Geometry">
      <arg type="t" direction="in" name="window"/>
      <arg type="(iiii)" direction="out" name="frame"/>
    </method>
    <method name="CurrentWorkspace">
      <arg type="i" direction="out" name="workspace"/>
    </method>
    <method name="SetWorkspace">
      <arg type="i" direction="in" name="workspace"/>
    </method>

    <!-- Clipboard (WP-A8). GNOME implements neither ext-data-control nor
         wlr-data-control, so on GNOME this extension is also the clipboard
         path. selection: 0 = CLIPBOARD, 1 = PRIMARY. -->
    <method name="ClipboardMimeTypes">
      <arg type="u" direction="in" name="selection"/>
      <arg type="as" direction="out" name="mime_types"/>
    </method>
    <method name="ClipboardRead">
      <arg type="u" direction="in" name="selection"/>
      <arg type="s" direction="in" name="mime_type"/>
      <arg type="ay" direction="out" name="data"/>
    </method>
    <method name="ClipboardWrite">
      <arg type="u" direction="in" name="selection"/>
      <arg type="s" direction="in" name="mime_type"/>
      <arg type="ay" direction="in" name="data"/>
    </method>
    <method name="ClipboardClear">
      <arg type="u" direction="in" name="selection"/>
    </method>

    <signal name="WindowAdded"><arg type="t" name="window"/></signal>
    <signal name="WindowRemoved"><arg type="t" name="window"/></signal>
    <signal name="WindowActivated"><arg type="t" name="window"/></signal>
    <signal name="WindowChanged"><arg type="t" name="window"/></signal>
    <signal name="WorkspaceChanged"><arg type="i" name="workspace"/></signal>
    <signal name="ClipboardChanged">
      <arg type="u" name="selection"/>
      <arg type="as" name="mime_types"/>
    </signal>

    <property name="Version" type="u" access="read"/>
  </interface>
</node>`;

const INT32_MIN = -2147483648;
const INT32_MAX = 2147483647;

function int32(value) {
    const n = Math.trunc(Number(value));
    if (!Number.isFinite(n)) return 0;
    return Math.min(INT32_MAX, Math.max(INT32_MIN, n));
}

function text(value) {
    return typeof value === 'string' ? value : '';
}

/**
 * Bitmask for one window record. Every caller goes through this so the flag
 * rules live in one place: a minimized window is never on screen, and a window
 * without a real frame or pid says so rather than shipping a placeholder the
 * switcher would believe.
 */
export function windowFlags(window) {
    let flags = 0;
    if (window.minimized) flags |= FLAG.MINIMIZED;
    if (window.focused) flags |= FLAG.FOCUSED;
    if (window.fullscreen) flags |= FLAG.FULLSCREEN;
    if (window.maximized) flags |= FLAG.MAXIMIZED;
    if (window.onCurrentWorkspace) flags |= FLAG.ON_CURRENT_WORKSPACE;
    if (window.hasGeometry !== false) flags |= FLAG.HAS_GEOMETRY;
    if (window.pid !== undefined && window.pid !== null && window.pid > 0) flags |= FLAG.HAS_PID;
    if (window.onScreen && !window.minimized) flags |= FLAG.ON_SCREEN;
    return flags >>> 0;
}

/** One window record as the `(tsssiiiiiuis)` struct, positionally. */
export function encodeWindow(window) {
    const frame = window.frame || {};
    return [
        Number(window.id) || 0,
        text(window.appId),
        text(window.appName) || text(window.appId),
        text(window.title),
        int32(window.pid === undefined || window.pid === null ? -1 : window.pid),
        int32(frame.x),
        int32(frame.y),
        int32(frame.width),
        int32(frame.height),
        windowFlags(window),
        int32(window.workspace === undefined || window.workspace === null ? -1 : window.workspace),
        text(window.output),
    ];
}

/** The `a(tsssiiiiiuis)` payload of List(). */
export function encodeWindowList(windows) {
    return windows.map(encodeWindow);
}
