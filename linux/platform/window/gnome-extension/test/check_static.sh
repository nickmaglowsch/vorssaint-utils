#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# Static checks for the extension: every file parses as an ES module, and the
# extension packs into the zip GNOME installs. extension.js and lib/mutter.js
# cannot be *executed* outside gnome-shell (they import Meta, Shell and
# resource:///org/gnome/shell/...), so they are parsed rather than run; the rest
# of the code is executed by test_units.js and run_bridge.js.

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

status=0
note() { echo "$*"; }
fail() { echo "FAIL: $*" >&2; status=1; }

# ------------------------------------------------------------- ESM syntax
# node parses a .js file as CommonJS, where `import` is a syntax error, so each
# file is checked under an .mjs name.
if command -v node >/dev/null 2>&1; then
    while IFS= read -r file; do
        relative=${file#"$ROOT"/}
        cp "$file" "$WORK/check.mjs"
        if node --check "$WORK/check.mjs"; then
            note "parse ok: $relative"
        else
            fail "$relative does not parse as an ES module"
        fi
    done < <(find "$ROOT" -name '*.js' | sort)
else
    note "SKIP: node missing, no ESM parse check"
fi

# --------------------------------------------------------------- gjs load
# The modules that do not need Mutter must also load under the interpreter
# that will actually run them.
if command -v gjs >/dev/null 2>&1; then
    for module in lib/protocol.js lib/throttle.js lib/ids.js lib/service.js; do
        cat >"$WORK/load.js" <<EOF
import 'file://$ROOT/$module';
EOF
        if gjs -m "$WORK/load.js"; then
            note "gjs load ok: $module"
        else
            fail "$module does not load under gjs"
        fi
    done
else
    note "SKIP: gjs missing, no load check"
fi

# ------------------------------------------------------------------ packing
if command -v gnome-extensions >/dev/null 2>&1; then
    if (cd "$ROOT" && gnome-extensions pack --force --extra-source=lib \
            --out-dir="$WORK" . >/dev/null); then
        zip="$WORK/vorssaint-bridge@vorssaint.com.shell-extension.zip"
        [[ -f $zip ]] || fail "pack produced no zip"
        for member in metadata.json extension.js lib/protocol.js lib/service.js lib/mutter.js \
                      lib/throttle.js lib/ids.js; do
            unzip -l "$zip" | grep -q " $member\$" || fail "$member missing from the pack"
        done
        note "pack ok: $(unzip -l "$zip" | tail -1 | tr -s ' ')"
    else
        fail "gnome-extensions pack refused the extension"
    fi
else
    note "SKIP: gnome-extensions missing, extension not packed"
fi

# ------------------------------------------------------------- no eval, no fs
# The security note promises a minimal surface; keep it true mechanically.
if grep -rnE '\beval\(|new Function\(|GLib\.spawn|Gio\.Subprocess|imports\.system' \
        "$ROOT/extension.js" "$ROOT/lib"; then
    fail "the extension must not evaluate code or spawn processes"
else
    note "surface ok: no eval, no Function(), no subprocess in the installed code"
fi

[[ $status -eq 0 ]] && echo "PASS: gnome-extension static checks"
exit $status
