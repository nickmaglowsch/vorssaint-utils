#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# Disables vorssaint-bridge and removes it from the user's extension directory.
# Nothing else was installed, so this leaves no trace: no system files, no
# D-Bus service file, no settings schema.
#
#   ./uninstall.sh [--quiet]

set -euo pipefail

UUID="vorssaint-bridge@vorssaint.com"
TARGET="${XDG_DATA_HOME:-$HOME/.local/share}/gnome-shell/extensions/$UUID"
QUIET=0
[[ ${1:-} == --quiet ]] && QUIET=1

say() { [[ $QUIET -eq 1 ]] || echo "$@"; }

if command -v gnome-extensions >/dev/null 2>&1; then
    gnome-extensions disable "$UUID" 2>/dev/null && say "disabled $UUID" || true
fi

if [[ -d $TARGET ]]; then
    rm -rf "$TARGET"
    say "removed $TARGET"
else
    say "$UUID was not installed"
fi

say "The bridge stops answering as soon as it is disabled; on Wayland a logout"
say "is still the way to unload it completely."
