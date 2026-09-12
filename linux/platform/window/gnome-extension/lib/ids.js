// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// The `t` window id the bridge puts on the bus.
//
// Mutter numbers its windows itself (`meta_window_get_id()`, a monotonic
// guint64 counter on MetaDisplay), which is exactly the stable, opaque handle
// the C contract asks for, so that is the id whenever it is there. Older
// Shell versions and any window whose id cannot be read fall back to a
// counter of ours, allocated in a range no Mutter counter reaches inside a
// session (2^32 and up) so the two can never name the same window.
//
// The reverse map is what makes Activate(id) an O(1) lookup instead of a scan
// over every actor, and it is also the thing that must be cleared on disable():
// holding MetaWindow references after the extension is unloaded is the classic
// GNOME extension leak.

export const SYNTHETIC_ID_BASE = 4294967296; // 2^32

export class IdAllocator {
    /**
     * @param {object} options
     * @param {function} [options.nativeId] window => Number|null
     */
    constructor({nativeId = null} = {}) {
        this._nativeId = nativeId;
        this._byWindow = new Map();
        this._byId = new Map();
        this._nextSynthetic = SYNTHETIC_ID_BASE;
    }

    get size() {
        return this._byWindow.size;
    }

    idFor(window) {
        const known = this._byWindow.get(window);
        if (known !== undefined) return known;

        let id = null;
        if (this._nativeId) {
            const candidate = this._nativeId(window);
            if (Number.isFinite(candidate) && candidate > 0 && !this._byId.has(candidate))
                id = candidate;
        }
        if (id === null) id = this._nextSynthetic++;

        this._byWindow.set(window, id);
        this._byId.set(id, window);
        return id;
    }

    /** The window an id names, or null once it is gone. */
    windowFor(id) {
        const window = this._byId.get(Number(id));
        return window === undefined ? null : window;
    }

    /** True when the id was ever handed out and its window has since closed. */
    knows(id) {
        return this._byId.has(Number(id));
    }

    forget(window) {
        const id = this._byWindow.get(window);
        if (id === undefined) return null;
        this._byWindow.delete(window);
        this._byId.delete(id);
        return id;
    }

    clear() {
        this._byWindow.clear();
        this._byId.clear();
    }
}
