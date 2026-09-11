#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# WP-00 spike: turn a swift build log into the error census the deliverable
# asks for — a count per category and ten representative errors — so the
# answer is readable straight from the job log through the Actions API
# instead of only from a 60 000-line artifact.
#
#   ./classify-errors.sh build.log

set -uo pipefail
log="${1:?usage: classify-errors.sh <build.log>}"

# One line per diagnostic, deduplicated: swift repeats each error once per
# emit-module pass and once per compile pass.
errors="$(grep -E ': error: ' "$log" | sed 's/^.*: error: /error: /' | sort | uniq -c | sort -rn)"
uniq_count="$(printf '%s\n' "$errors" | grep -c . || true)"
total="$(grep -cE ': error: ' "$log" || true)"

echo "=== error census for $log ==="
echo "raw diagnostics:    $total"
echo "distinct messages:  $uniq_count"
echo

bin() { # name, regex
    local n
    n="$(grep -E ': error: ' "$log" | grep -cE "$2" || true)"
    printf '%-26s %6s\n' "$1" "$n"
}

echo "--- by category (raw diagnostics) ---"
bin "Combine/OpenCombine"   "Combine|ObservableObject|@Published|AnyCancellable|Subject|objectWillChange"
bin "CoreGraphics types"    "\bCG[A-Z]"
bin "AppKit leak"           "\bNS[A-Z]|AppKit|NSApp"
bin "Foundation gap"        "FoundationXML|FoundationNetworking|XMLParser|XMLDocument|trashItem|NSAttributedString|is unavailable|has not been implemented|unavailable in Linux"
bin "Swift 6 concurrency"   "concurrency|Sendable|actor-isolated|main actor|nonisolated|global variable"
bin "unresolved in-repo"    "cannot find (type )?'[A-Za-z]"
bin "Darwin/libc"           "Darwin|sysctl|mach_|host_statistics|IOKit"
bin "circular/module"       "circular dependency|no such module"
echo

echo "--- 10 representative distinct errors ---"
printf '%s\n' "$errors" | head -10
echo

echo "--- files with the most diagnostics ---"
grep -E ': error: ' "$log" | grep -oE '/Vendored/[A-Za-z+]+\.swift' | sort | uniq -c | sort -rn | head -15
