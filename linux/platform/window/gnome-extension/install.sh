#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# Installs vorssaint-bridge into the user's extension directory and enables it.
# The Capabilities page (WP-25) runs this; it is also usable by hand.
#
# No root, no system directory, no D-Bus activation file: the extension is
# per-user data under $XDG_DATA_HOME, and removing it is deleting a directory.
#
#   ./install.sh [--quiet]
#
# Exit codes: 0 installed (and enabled, or enabled on the next session),
#             1 installed but could not be enabled, 2 nothing was installed.

set -euo pipefail

UUID="vorssaint-bridge@vorssaint.com"
SOURCE=$(cd "$(dirname "$0")" && pwd)
TARGET="${XDG_DATA_HOME:-$HOME/.local/share}/gnome-shell/extensions/$UUID"
QUIET=0
[[ ${1:-} == --quiet ]] && QUIET=1

say() { [[ $QUIET -eq 1 ]] || echo "$@"; }
die() { echo "$*" >&2; exit 2; }

[[ -f "$SOURCE/metadata.json" ]] || die "metadata.json not next to install.sh"

# The directory is replaced rather than merged: an upgrade must not leave a
# file behind that this version no longer ships but gnome-shell would still
# load. Installed files only — the tests and the docs do not run in the Shell.
rm -rf "$TARGET"
mkdir -p "$TARGET/lib"
install -m 0644 "$SOURCE/metadata.json" "$SOURCE/extension.js" "$TARGET/"
install -m 0644 "$SOURCE"/lib/*.js "$TARGET/lib/"
say "installed to $TARGET"

if ! command -v gnome-extensions >/dev/null 2>&1; then
    say "gnome-extensions is not installed; enable $UUID by hand"
    exit 1
fi

if gnome-extensions enable "$UUID" 2>/dev/null; then
    say "enabled $UUID"
else
    # GNOME refuses to enable an extension it has not seen yet; it will be
    # picked up when the Shell next reads the extension directory.
    say "could not enable $UUID yet; it will be available after the session restart below"
fi

case "${XDG_SESSION_TYPE:-}" in
    wayland)
        say "GNOME on Wayland cannot reload the Shell: log out and back in to load the bridge."
        ;;
    x11)
        say "GNOME on X11: press Alt+F2, type r, Enter to restart the Shell and load the bridge."
        ;;
    *)
        say "Restart GNOME Shell to load the bridge (Alt+F2 r on X11; log out and back in on Wayland)."
        ;;
esac
