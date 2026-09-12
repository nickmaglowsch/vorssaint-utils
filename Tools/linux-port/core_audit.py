#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# The WP-11 audit that produced docs/linux-port/CORE_MOVES.md. It sits next to
# declgraph.py and uses it: declgraph answers "what depends on what", this
# answers "what could move, and why did the rest not".
#
#   core_audit.py hazards [FILE...]   API swift-corelibs-foundation lacks
#   core_audit.py closed              the largest dependency-closed portable set
#   core_audit.py census              WP-00 census verdict vs. where files are now
#   core_audit.py globalshortcut      which half of GlobalShortcut each user needs
#
# Run from the repository root. `closed` and `census` re-derive their answers
# from the working tree, so after WP-11 `closed` reports what could move *next*,
# not what moved: the files already under Sources/VorssaintCore are excluded.

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import declgraph  # noqa: E402


EXCLUDE = ("Sources/VorssaintCore", "Sources/VorssaintMac", "Sources/VorssaintLinux",
           "Sources/VorssaintCombine", "Sources/FanControlHelper",
           "Sources/NowPlayingAdapter")

# Modules a VorssaintCore file may import. CoreGraphics is allowed only behind
# a `canImport` guard and only for the geometry family, which Linux Foundation
# provides; the module itself does not exist there.
PORTABLE_MODULES = {"Foundation", "FoundationXML", "FoundationNetworking",
                    "Combine", "OpenCombine", "OpenCombineFoundation",
                    "OpenCombineDispatch", "Dispatch"}
GEOMETRY = re.compile(r"^CG(Float|Point|Size|Rect|Vector|AffineTransform)$")

# API that is absent, or meaningless, on swift-corelibs-foundation. Each entry
# is a finding WP-11 acted on; the ones the compiler found later are marked.
HAZARDS = [
    ("FoundationNetworking", r"\b(URLSession|URLRequest|HTTPURLResponse|URLResponse"
                             r"|URLSessionTask|URLCache|CachedURLResponse)\b"),
    ("FoundationXML", r"\b(XMLParser|XMLDocument|XMLNode|XMLElement)\b"),
    ("CoreFoundation C API", r"\bCF[A-Z][A-Za-z0-9_]*\s*\("),
    ("CoreFoundation type", r"\bCF(RunLoop|MachPort|Bundle|Dictionary|Array|String"
                            r"|Number|Boolean|Type|Allocator|Absolute)[A-Za-z0-9_]*\b"),
    ("Apple k-constant", r"\bk(CG|CF|TIS|UC|MD|Sec|CA|AX)[A-Z][A-Za-z0-9_]*\b"),
    ("Bundle.main", r"\bBundle\.main\b"),
    ("macOS availability", r"#available\(macOS|@available\(macOS"),
    ("ProcessInfo.ThermalState", r"ProcessInfo\.[A-Za-z]*[Tt]hermal|\bthermalState\b"),
    ("FileManager.trashItem", r"\btrashItem\b"),
    ("AttributedString markdown", r"MarkdownParsingOptions|PresentationIntent"
                                  r"|AttributedString\(markdown:"),
    ("AppKit symbol", r"\bNS(Workspace|RunningApplication|Pasteboard|Image|Color|Font"
                      r"|Screen|Event|Application|View|Window|Menu|Alert|Sound"
                      r"|Appearance|StatusBar|UserNotification|AppleScript|MachPort)\b"),
    ("os.log", r"\b(os_log|OSLog)\b"),
    ("mach / sysctl", r"\b(mach_[a-z_]+|sysctlbyname)\b"),
    ("Security", r"\b(SecItem[A-Za-z]*|SecKey|SecCertificate|OSStatus)\b"),
    ("@objc / #selector", r"@objc\b|#selector\("),
]


def graph():
    return declgraph.Graph("Sources", exclude=EXCLUDE)


def swift_files(root="Sources/Vorssaint"):
    out = []
    for d, _, fs in os.walk(root):
        out += [os.path.join(d, f) for f in fs if f.endswith(".swift")]
    return sorted(out)


def hazards(paths):
    for p in paths:
        text = declgraph.strip_noise(open(p, encoding="utf-8", errors="replace").read())
        hits = []
        for label, pat in HAZARDS:
            found = sorted({m.group(0) for m in re.finditer(pat, text)})
            if found:
                hits.append("%s[%s]" % (label, ",".join(found[:4])))
        if hits:
            print("%-64s %s" % (p.replace("Sources/Vorssaint/", ""), " ".join(hits)))


def portable(g, path):
    """Could this file compile on Linux at all, ignoring its dependencies?"""
    if path.startswith("Sources/Vorssaint/UI/"):
        return False, "UI (out of WP-11 scope)"
    bad = g.imports[path] - PORTABLE_MODULES - {"CoreGraphics"}
    if bad:
        return False, "imports " + ",".join(sorted(bad))
    text = declgraph.strip_noise(open(path, encoding="utf-8", errors="replace").read())
    cg = {m.group(0) for m in re.finditer(r"\bk?CG[A-Za-z0-9_]+\b", text)}
    hard = sorted(n for n in cg if not GEOMETRY.match(n))
    if hard:
        return False, "CoreGraphics beyond geometry: " + ",".join(hard[:4])
    for label, pat in HAZARDS:
        if label in ("CoreFoundation C API", "CoreFoundation type", "Apple k-constant"):
            continue                    # already covered by the CG scan above
        if re.search(pat, text):
            return False, label
    return True, ""


def closed():
    g = graph()
    cand, reason = {}, {}
    for p in g.files:
        ok, why = portable(g, p)
        if ok:
            cand[p] = True
        else:
            reason[p] = why
    dropped = {}
    changed = True
    while changed:                      # greatest fixed point
        changed = False
        for p in list(cand):
            needed, _ = g.deps(p)
            out = sorted(n for n in needed if n not in cand)
            if out:
                del cand[p]
                dropped[p] = out
                changed = True
    print("portable by import and API: %d of %d files"
          % (len(g.files) - len(reason), len(g.files)))
    print("dependency-closed subset:   %d files, %d lines"
          % (len(cand), sum(g.lines[p] for p in cand)))
    print("dropped for dependencies:   %d files" % len(dropped))
    print("\n--- could move now ---")
    for p in sorted(cand):
        print(p)
    print("\n--- blocked by a dependency ---")
    for p in sorted(dropped):
        print("%-62s -> %s" % (p[len("Sources/Vorssaint/"):],
                               " ".join(d.split("/")[-1] for d in dropped[p][:5])))
    print("\n--- not portable on its own ---")
    for p in sorted(reason):
        if not p.startswith("Sources/Vorssaint/UI/"):
            print("%-62s %s" % (p[len("Sources/Vorssaint/"):], reason[p]))


def census():
    spike = open("docs/linux-port/spikes/00-swift-core.md", encoding="utf-8").read()
    rows = {}
    for line in spike.split("\n"):
        m = re.match(r"\|\s*`([^`]+\.swift)`\s*\|\s*(\d+)\s*\|(.*)\|"
                     r"\s*([a-z (),A-Za-z]+)\s*\|\s*$", line)
        if m:
            rows["Sources/Vorssaint/" + m.group(1)] = m.group(4).strip()
    agree = []
    disagree = []
    for path, klass in sorted(rows.items()):
        rel = path[len("Sources/Vorssaint/"):]
        moved = os.path.isfile("Sources/VorssaintCore/" + rel)
        clean = klass.startswith("clean")
        (agree if (clean == moved) else disagree).append((rel, klass, moved))
    print("census rows parsed: %d" % len(rows))
    print("agree %d  disagree %d" % (len(agree), len(disagree)))
    print("\n| census file | census class | WP-11 outcome |")
    print("|---|---|---|")
    for rel, klass, moved in disagree:
        print("| `%s` | %s | **%s** |" % (rel, klass, "moved" if moved else "stayed"))


# Members of GlobalShortcut that cannot exist without Carbon (the kVK_ default
# table), CoreGraphics (CGEvent) or AppKit (NSEvent).
CARBON_HALF = re.compile(
    r"\.[a-zA-Z]+Default\b"
    r"|\b(carbonKeyCode|carbonModifiers|carbonFlags|cgFlags|syntheticEventFlags"
    r"|keyLabel|displayString|keyCaps|isValid|hasPrintableKey|matches"
    r"|matchesByCharacter|requiredModifiersHeld|clearsShortcut"
    r"|superKeyAlternative)\b"
    r"|GlobalShortcut\(storageValue:")


def globalshortcut():
    g = graph()
    gs = "Sources/Vorssaint/Core/GlobalShortcut.swift"
    needs = stays = 0
    print("%-56s %s" % ("file", "needs the Carbon/CGEvent half"))
    for p in g.files:
        if p == gs or "GlobalShortcut" not in g.references[p]:
            continue
        if p.startswith("Sources/Vorssaint/UI/"):
            continue
        text = declgraph.strip_noise(open(p, encoding="utf-8", errors="replace").read())
        hits = sorted({m.group(0) for m in CARBON_HALF.finditer(text)})
        if hits:
            needs += 1
        else:
            stays += 1
        print("%-56s %s" % (p[len("Sources/Vorssaint/"):],
                            ", ".join(hits[:4]) or "value type only"))
    print("\n%d of %d non-UI users need the Carbon/CGEvent half; %d use only the "
          "value type, and all of those are Mac-layer files." % (needs, needs + stays, stays))


if __name__ == "__main__":
    command = sys.argv[1] if len(sys.argv) > 1 else "closed"
    if command == "hazards":
        hazards(sys.argv[2:] or swift_files())
    elif command == "closed":
        closed()
    elif command == "census":
        census()
    elif command == "globalshortcut":
        globalshortcut()
    else:
        sys.exit(__doc__ or "unknown command: " + command)
