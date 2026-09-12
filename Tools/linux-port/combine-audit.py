#!/usr/bin/env python3
"""WP-13 Combine audit.

Walks Sources/ for every file that touches the Combine surface (either an
explicit `import Combine`/`import OpenCombine`/`import VorssaintCombine`, or
one of the Combine-only symbols below reached transitively through
`import AppKit` or `import SwiftUI`, both of which re-export Combine on
Darwin), lists which operators each file uses, flags files that are candidates
for the WP-11 move into Sources/VorssaintCore (cross-checked against the
per-file census table in docs/linux-port/spikes/00-swift-core.md), and prints
an OpenCombine-0.14 support verdict per operator.

The OpenCombine verdict is this script's own knowledge table, not something
it can check offline (github.com is blocked in this environment and the
OpenCombine sources are not vendored locally) -- rows marked "verify on CI"
are exactly the ones WP-00 flagged as unmeasured (spikes/00-swift-core.md
Sec 6, condition 6): only Localization.swift's plain `import Combine` has
actually been built against OpenCombine so far.

Usage:
    python3 Tools/linux-port/combine-audit.py [--root Sources] [--csv OUT.csv]

Re-run after WP-11/WP-12 move files, to see the candidate list shrink and the
transitive-import list (files that will break silently when they land on
Linux without an explicit Combine import) shrink with it.
"""
from __future__ import annotations

import argparse
import csv
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CENSUS_DOC = REPO_ROOT / "docs/linux-port/spikes/00-swift-core.md"

# name -> (regex, OpenCombine 0.14 verdict, note)
# Verdict is one of: "supported", "supported (needs product)", "verify on CI".
OPERATORS: dict[str, tuple[str, str, str]] = {
    "ObservableObject": (r"\bObservableObject\b", "supported",
                          "core protocol, OpenCombine.ObservableObject"),
    "@Published": (r"@Published\b", "supported",
                    "OpenCombine.Published property wrapper"),
    "AnyCancellable": (r"\bAnyCancellable\b", "supported", "core type"),
    "PassthroughSubject": (r"\bPassthroughSubject\b", "supported", "core type"),
    "CurrentValueSubject": (r"\bCurrentValueSubject\b", "supported", "core type"),
    "objectWillChange": (r"\bobjectWillChange\b", "supported",
                          "ObservableObjectPublisher"),
    "Just": (r"\bJust\(", "supported", "core type"),
    "Future": (r"\bFuture[<(]", "supported", "core type"),
    ".eraseToAnyPublisher": (r"\.eraseToAnyPublisher\(", "supported", "core operator"),
    ".sink": (r"\.sink\(", "supported", "core operator"),
    ".assign": (r"\.assign\(", "verify on CI",
                "core assign(to:on:) is implemented; the assign(to:&$x) "
                "publish-to-Published overload has had parity gaps in older "
                "OpenCombine releases -- confirm on 0.14 with a real build"),
    ".receive(on:)": (r"\.receive\(on:", "supported (needs product)",
                       "Publishers.ReceiveOn; DispatchQueue as a Scheduler "
                       "needs OpenCombineDispatch, RunLoop/Timer scheduling "
                       "needs OpenCombineFoundation"),
    ".debounce": (r"\.debounce\(", "verify on CI", "implemented upstream; unexercised here"),
    ".throttle": (r"\.throttle\(", "verify on CI", "implemented upstream; unexercised here"),
    ".removeDuplicates": (r"\.removeDuplicates\(", "supported", "core operator"),
    ".dropFirst": (r"\.dropFirst\(", "supported", "core operator"),
    ".prepend": (r"\.prepend\(", "supported", "core operator"),
    ".combineLatest": (r"\.combineLatest\(", "supported", "core operator"),
    ".merge": (r"\.merge\(", "supported", "core operator"),
    ".zip": (r"\.zip\(", "supported", "core operator"),
    ".share": (r"\.share\(\)", "verify on CI", "implemented upstream; unexercised here"),
    ".multicast": (r"\.multicast\(", "verify on CI", "implemented upstream; unexercised here"),
    ".catch": (r"\.catch\(", "verify on CI", "implemented upstream; unexercised here"),
    ".retry": (r"\.retry\(", "verify on CI", "implemented upstream; unexercised here"),
    ".replaceError": (r"\.replaceError\(", "verify on CI", "implemented upstream; unexercised here"),
    ".timeout": (r"\.timeout\(", "verify on CI", "implemented upstream; unexercised here"),
    ".delay(for:)": (r"\.delay\(for:", "supported (needs product)",
                      "Scheduler-based; needs OpenCombineDispatch or "
                      "OpenCombineFoundation depending on the scheduler used"),
    "Timer.publish": (r"Timer\.publish\(", "supported (needs product)",
                       "Timer.TimerPublisher lives in OpenCombineFoundation, "
                       "not core OpenCombine"),
    "NotificationCenter.publisher": (r"NotificationCenter\.\w+\.publisher\(",
                                      "supported (needs product)",
                                      "NotificationCenter.Publisher lives in "
                                      "OpenCombineFoundation"),
    "RunLoop as Scheduler": (r"RunLoop\.(main|current)\.schedule\(",
                              "supported (needs product)",
                              "RunLoop: Scheduler conformance lives in "
                              "OpenCombineFoundation; NOTE: RunLoop.main.add(_:forMode:) "
                              "elsewhere in this codebase is plain Foundation "
                              "run-loop registration, not a Combine scheduler call, "
                              "and needs nothing from OpenCombine"),
    "DispatchQueue as Scheduler": (r"DispatchQueue\.\w+\.schedule\(",
                                    "supported (needs product)",
                                    "DispatchQueue: Scheduler conformance lives "
                                    "in OpenCombineDispatch"),
}

# Symbols that, if present without an explicit Combine-family import, mean the
# file only compiles today because AppKit/SwiftUI re-export Combine on Darwin.
# (Populated below from STRONG_OPERATORS once that set exists.)
TRANSITIVE_SIGNAL: re.Pattern[str]

# Operators that are unambiguously Combine (either Combine-only names, or
# names Combine defines that nothing else in Foundation/Swift shadows).
STRONG_OPERATORS = {
    "ObservableObject", "@Published", "AnyCancellable", "PassthroughSubject",
    "CurrentValueSubject", "objectWillChange", "Just", "Future",
    "Timer.publish", "NotificationCenter.publisher",
}

# Operators with the same spelling on Sequence/Collection/String
# (.dropFirst, .prepend, .merge, .zip...) or that read as Combine only in
# context (.share(), .catch(), .retry() are also plain English method names
# some services use for non-Combine purposes). These are counted for a file
# ONLY when that file already shows a STRONG_OPERATORS signal or an explicit
# Combine-family import -- otherwise they are almost always false positives
# (verified for .dropFirst against this codebase: every non-Combine hit is
# String.dropFirst/Array.dropFirst, e.g. RadialMenuSupport.swift's
# `value.dropFirst("button:".count)`).
AMBIGUOUS_OPERATORS = set(OPERATORS.keys()) - STRONG_OPERATORS
TRANSITIVE_SIGNAL = re.compile("|".join(OPERATORS[name][0] for name in STRONG_OPERATORS))

IMPORT_RE = re.compile(r"^\s*(?:@_exported\s+)?import\s+(\w[\w.]*)", re.MULTILINE)
COMBINE_IMPORT_NAMES = {"Combine", "OpenCombine", "VorssaintCombine"}
REEXPORTS_COMBINE_ON_DARWIN = {"AppKit", "SwiftUI"}


@dataclass
class FileAudit:
    path: Path
    lines: int
    imports: list[str]
    explicit_combine_import: bool
    transitive_only: bool
    operators: list[str] = field(default_factory=list)
    wp11_candidate: bool = False
    census_classification: str | None = None


def load_census_candidates() -> dict[str, str]:
    """Parse the WP-00 per-file census table: relative path -> classification."""
    candidates: dict[str, str] = {}
    if not CENSUS_DOC.exists():
        return candidates
    text = CENSUS_DOC.read_text(encoding="utf-8")
    # Rows look like: | `Core/Localization.swift` | 3193 | Combine | ... | needs OpenCombine only |
    row_re = re.compile(r"^\|\s*`([^`]+\.swift)`\s*\|.*\|\s*([^|]+?)\s*\|\s*$")
    for line in text.splitlines():
        m = row_re.match(line.strip())
        if m:
            candidates[m.group(1)] = m.group(2).strip()
    return candidates


def audit_file(path: Path, census: dict[str, str]) -> FileAudit | None:
    try:
        text = path.read_text(encoding="utf-8")
    except (UnicodeDecodeError, OSError):
        return None

    imports = IMPORT_RE.findall(text)
    explicit = any(i in COMBINE_IMPORT_NAMES for i in imports)
    reexported = any(i in REEXPORTS_COMBINE_ON_DARWIN for i in imports)
    has_signal = bool(TRANSITIVE_SIGNAL.search(text))

    strong_ops = [name for name in STRONG_OPERATORS
                  if re.search(OPERATORS[name][0], text)]
    is_combine_file = explicit or bool(strong_ops)
    if not is_combine_file:
        return None  # ambiguous-only hits (e.g. String.dropFirst) are noise

    ambiguous_ops = [name for name in AMBIGUOUS_OPERATORS
                      if re.search(OPERATORS[name][0], text)]
    matched_ops = strong_ops + ambiguous_ops
    if not matched_ops:
        # imports Combine but doesn't hit any tracked operator/symbol -- still
        # worth listing, e.g. a file that only forwards a publisher type.
        matched_ops = ["(import only -- no tracked operator matched)"]

    transitive_only = has_signal and not explicit and reexported

    rel = path.relative_to(REPO_ROOT)
    # Census keys are relative to Sources/Vorssaint/.
    census_key = None
    try:
        census_key = str(rel.relative_to("Sources/Vorssaint"))
    except ValueError:
        pass
    classification = census.get(census_key) if census_key else None
    wp11_candidate = classification is not None and classification != "not portable"

    return FileAudit(
        path=rel,
        lines=text.count("\n") + 1,
        imports=imports,
        explicit_combine_import=explicit,
        transitive_only=transitive_only,
        operators=sorted(matched_ops),
        wp11_candidate=wp11_candidate,
        census_classification=classification,
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default="Sources", help="directory to scan (default: Sources)")
    ap.add_argument("--csv", help="also write the per-file table as CSV to this path")
    args = ap.parse_args()

    root = REPO_ROOT / args.root
    census = load_census_candidates()

    audits: list[FileAudit] = []
    for path in sorted(root.rglob("*.swift")):
        a = audit_file(path, census)
        if a:
            audits.append(a)

    print(f"# Combine audit ({len(audits)} files touch the Combine surface)\n")
    print("| file | lines | explicit import | transitive only | WP-11 candidate | operators |")
    print("|---|---:|---|---|---|---|")
    for a in audits:
        print(
            f"| `{a.path}` | {a.lines} | {'yes' if a.explicit_combine_import else 'no'} "
            f"| {'yes' if a.transitive_only else 'no'} "
            f"| {'yes' if a.wp11_candidate else 'no'}"
            f"{' (' + a.census_classification + ')' if a.census_classification else ''} "
            f"| {', '.join(a.operators)} |"
        )

    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["file", "lines", "explicit_import", "transitive_only",
                        "wp11_candidate", "census_classification", "operators"])
            for a in audits:
                w.writerow([str(a.path), a.lines, a.explicit_combine_import,
                            a.transitive_only, a.wp11_candidate,
                            a.census_classification or "", ";".join(a.operators)])

    explicit_n = sum(1 for a in audits if a.explicit_combine_import)
    transitive_n = sum(1 for a in audits if a.transitive_only)
    wp11_n = sum(1 for a in audits if a.wp11_candidate)

    print("\n## Totals\n")
    print(f"- files touching the Combine surface: {len(audits)}")
    print(f"- explicit `import Combine`: {explicit_n}")
    print(f"- transitive only (via AppKit/SwiftUI re-export, no explicit import): {transitive_n}")
    print(f"- WP-11 core-move candidates (per the WP-00 census): {wp11_n}")

    print("\n## OpenCombine 0.14 verdict per operator (this script's knowledge table)\n")
    print("| operator | files using it | verdict | note |")
    print("|---|---:|---|---|")
    for name, (pat, verdict, note) in OPERATORS.items():
        n = sum(1 for a in audits if name in a.operators)
        if n == 0:
            continue
        print(f"| {name} | {n} | {verdict} | {note} |")

    return 0


if __name__ == "__main__":
    sys.exit(main())
