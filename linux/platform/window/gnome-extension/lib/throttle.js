// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// Rate limiting for WindowChanged. Mutter emits position-changed and
// size-changed on every frame of an interactive drag; unthrottled that is a
// D-Bus message per frame per window, which is what makes a naive bridge
// visibly slow down the drag it is watching.
//
// The policy is leading edge plus coalesced trailing edge, per key: the first
// change for a window goes out at once (the switcher stays responsive), further
// changes inside the window are collapsed into exactly one emission at the end
// of it. No GLib in here: the timeout source is injected, so the unit tests
// drive it with a counter instead of a main loop.

export const DEFAULT_CHANGE_INTERVAL_MS = 120;

export class Throttler {
    /**
     * @param {object} options
     * @param {number} options.intervalMs   coalescing window
     * @param {function} options.emit       called with the key
     * @param {function} options.schedule   (ms, callback) => token
     * @param {function} options.cancel     (token) => void
     */
    constructor({intervalMs = DEFAULT_CHANGE_INTERVAL_MS, emit, schedule, cancel}) {
        this._intervalMs = intervalMs;
        this._emit = emit;
        this._schedule = schedule;
        this._cancel = cancel;
        // key -> {token, pending}
        this._entries = new Map();
    }

    get pendingCount() {
        return this._entries.size;
    }

    push(key) {
        const entry = this._entries.get(key);
        if (entry) {
            entry.pending = true;
            return;
        }
        this._emit(key);
        this._entries.set(key, {token: this._armTimer(key), pending: false});
    }

    /** Drop a key without emitting: the window is gone. */
    forget(key) {
        const entry = this._entries.get(key);
        if (!entry) return;
        this._cancel(entry.token);
        this._entries.delete(key);
    }

    /** Every armed timer is cancelled; nothing is emitted. Called from disable(). */
    destroy() {
        for (const entry of this._entries.values()) this._cancel(entry.token);
        this._entries.clear();
    }

    _armTimer(key) {
        return this._schedule(this._intervalMs, () => {
            const entry = this._entries.get(key);
            if (!entry) return false;
            if (!entry.pending) {
                this._entries.delete(key);
                return false;
            }
            entry.pending = false;
            entry.token = this._armTimer(key);
            this._emit(key);
            return false;
        });
    }
}
