#!/usr/bin/env python3
"""Count how many macOS test vectors the Linux relay's rule tests carry over.

`docs/linux-port/RELAY_RULES.md` claims a number -- "147 assertions carried
over from Tests/MetricsTests.swift, 137 of them verbatim". This is the script
that produces it, so the claim is reproducible instead of asserted, and so it
goes down if someone drops a vector.

How it decides, for each assertion `linux/helper/tests/test_rules.c` prints:

  verbatim  its message appears character-for-character in MetricsTests.swift.
            That is deliberate: the C tests reuse the Swift's own wording so
            the two suites can be diffed by eye, and so a rename on either
            side shows up here rather than silently drifting.

  adapted   its message becomes a Swift message after applying ONE of the
            named substitutions in ADAPTATIONS below, each of which is a real
            platform difference with a reason attached. Nothing is guessed at:
            a rule that does not produce an exact Swift message does not
            count, and the rule that matched is printed.

  new       neither. A case the Swift does not have, because the Linux
            surface has a hazard or a shape macOS does not.

usage:
  Tools/linux-port/count_ported_vectors.py [--test-output FILE] [--verbose]

With no --test-output it builds nothing and expects the suite's output on
stdin, or re-runs a already-built binary if one is named in $TEST_RULES.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

SWIFT = "Tests/MetricsTests.swift"
CTEST = "linux/helper/tests/test_rules.c"

# Every phrase the Linux tests say differently from the Mac ones, and why.
# A vector counts as "adapted" only if applying these turns its message into
# an exact Swift message -- so this table is the whole of what "adapted" is
# allowed to mean.
ADAPTATIONS: list[tuple[str, str, str]] = [
    ("the per-app overview", "App Exposé",
     "App Exposé is a macOS feature name; the gesture asks the compositor for "
     "the per-application overview, whatever the desktop calls it"),
    ("the overview", "Mission Control",
     "Mission Control is a macOS feature name; on Linux the notice says "
     "\"overview\" and the app maps it per compositor (WP-C1)"),
    ("the workspace", "the Space",
     "a macOS Space is a Linux workspace"),
    ("switching workspaces", "switching Spaces",
     "same rename, in the progressive form"),
    ("Ctrl+", "Command-",
     "Command is Control on every Linux desktop; the shortcut is the same "
     "shortcut"),
    ("Alt-", "Control-",
     "quit protection's extra modifier: macOS can use Control because Command "
     "is the base, Linux cannot because Control IS the base, so the third "
     "choice is Alt"),
    ("an app with no identifier", "an app without a bundle identifier",
     "a bundle identifier is a macOS app id; SetContext carries a desktop "
     "app_id"),
    ("the configured-key action", "the input-source action",
     "choosing a keyboard layout is a desktop setting a root daemon must not "
     "call, so the rule emits a configured key and the app binds it "
     "(RELAY_RULES.md § 5)"),
]


def swift_text() -> str:
    """MetricsTests.swift with its multi-line string concatenations joined.

    The Swift wraps long messages as `"a "` + `"b"`, which is one message to a
    reader and two literals to a parser."""
    with open(SWIFT, encoding="utf-8") as f:
        text = f.read()
    return re.sub(r'"\s*\+\s*"', "", text)


def suite_output(path: str | None) -> str:
    if path:
        with open(path, encoding="utf-8") as f:
            return f.read()
    binary = os.environ.get("TEST_RULES")
    if binary:
        return subprocess.run([binary], capture_output=True, text=True,
                              check=False).stdout
    if not sys.stdin.isatty():
        return sys.stdin.read()
    sys.exit("no test output: pass --test-output FILE, set $TEST_RULES, or pipe it in")


def assertions(out: str) -> list[tuple[str, str]]:
    """(section, message) for every assertion the suite printed."""
    found: list[tuple[str, str]] = []
    section = "(none)"
    for line in out.splitlines():
        if line.startswith("  ok ") or line.startswith("  FAIL "):
            msg = line.split(None, 1)[1]
            if msg.startswith("ok"):
                msg = msg[2:]
            msg = msg.strip()
            # Some pass lines append a measured detail: "… (21 frames, …)".
            msg = re.sub(r"\s*\((?:\d+ frames|\d+ bytes|\d+ ms)[^)]*\)$", "", msg)
            found.append((section, msg))
        elif line.strip() and not line.startswith(" "):
            section = line.strip()
    return found


def _sub(msg: str, linux_phrase: str, mac_phrase: str) -> str:
    """Replace on a word boundary.

    Without the boundary, "the overview" rewrites "the overviews never swap"
    too, and the vector that needs only the Space -> workspace rename is
    mangled by a rule that should not have applied to it."""
    return re.sub(re.escape(linux_phrase) + r"\b", mac_phrase, msg)


def classify(msg: str, swift: str) -> tuple[str, str]:
    if msg in swift:
        return "verbatim", ""

    for linux_phrase, mac_phrase, why in ADAPTATIONS:
        candidate = _sub(msg, linux_phrase, mac_phrase)
        if candidate != msg and candidate in swift:
            return "adapted", f"{linux_phrase!r} -> {mac_phrase!r}: {why}"

    # Exactly one vector needs two renames at once: the Alt-Ctrl+W extra
    # modifier, which is Control-Command-W on the Mac. Tried second, so a
    # single rule is always preferred and the reason stays specific.
    for a1, b1, w1 in ADAPTATIONS:
        once = _sub(msg, a1, b1)
        if once == msg:
            continue
        for a2, b2, w2 in ADAPTATIONS:
            if a2 == a1:
                continue
            twice = _sub(once, a2, b2)
            if twice != once and twice in swift:
                return "adapted", (f"{a1!r} -> {b1!r} and {a2!r} -> {b2!r}: "
                                   f"{w1}; {w2}")
    return "new", ""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--test-output", help="captured output of linux/helper's test_rules")
    ap.add_argument("--verbose", action="store_true",
                    help="print every adapted and new assertion")
    args = ap.parse_args()

    if not os.path.exists(SWIFT):
        sys.exit(f"run me from the repository root: {SWIFT} not found")

    swift = swift_text()
    rows = assertions(suite_output(args.test_output))
    if not rows:
        sys.exit("no assertions found in the test output")

    order: list[str] = []
    per: dict[str, dict[str, list[tuple[str, str]]]] = {}
    for section, msg in rows:
        if section not in per:
            per[section] = {"verbatim": [], "adapted": [], "new": []}
            order.append(section)
        kind, why = classify(msg, swift)
        per[section][kind].append((msg, why))

    total = {"verbatim": 0, "adapted": 0, "new": 0}
    print(f"{'section':<62} {'verb':>4} {'adap':>4} {'new':>4}")
    print("-" * 78)
    for section in order:
        d = per[section]
        for kind in total:
            total[kind] += len(d[kind])
        print(f"{section[:62]:<62} {len(d['verbatim']):>4} "
              f"{len(d['adapted']):>4} {len(d['new']):>4}")
        if args.verbose:
            for msg, why in d["adapted"]:
                print(f"      adapted: {msg}\n               {why}")
            for msg, _ in d["new"]:
                print(f"      new:     {msg}")
    print("-" * 78)
    print(f"{'TOTAL':<62} {total['verbatim']:>4} {total['adapted']:>4} "
          f"{total['new']:>4}")
    carried = total["verbatim"] + total["adapted"]
    print(f"\n{carried} assertions carried over from {SWIFT} "
          f"({total['verbatim']} verbatim, {total['adapted']} adapted)")
    print(f"{total['new']} Linux-specific assertions")
    print(f"{sum(total.values())} assertions in {CTEST}")

    if total["new"] and not args.verbose:
        print("\n(--verbose lists every adapted and Linux-specific assertion)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
