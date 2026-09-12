#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# WP-15: the drift guard between the Mac layer's `AppFeature` and the core's
# `FeatureSupportCatalog`.
#
# `Core/FeatureCatalog.swift` could not move to `VorssaintCore` — it is behind
# `RadialMenuSupport` and `GlobalShortcut` (`CORE_MOVES.md` appendix A) — so
# the platform table is keyed by `AppFeature.rawValue` rather than by the enum,
# and no compiler checks that the two agree. This does, on both CI legs:
#
#     python3 Tools/linux-port/check-feature-catalog.py
#
# It also checks the macOS preset mirror. `FeaturePresets.swift` stays the
# macOS hub's surface (the Mac harness pins which files may write the
# availability key), so `FeaturePresetCatalog` carries a copy of those three
# sets and this is what keeps the copy honest.

import re
import sys

CATALOG = "Sources/Vorssaint/Core/FeatureCatalog.swift"
PRESETS = "Sources/Vorssaint/Core/FeaturePresets.swift"
SUPPORT = "Sources/VorssaintCore/Core/FeatureSupportCatalog.swift"


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def app_features(text):
    """The `case a, b, c` lines of `enum AppFeature: String, CaseIterable`."""
    start = text.index("enum AppFeature: String, CaseIterable {")
    body = text[start:text.index("\n}", start)]
    names = []
    for line in body.split("\n"):
        stripped = line.strip()
        if stripped.startswith("case ") or (names and stripped and stripped[0].islower()
                                            and "=" not in stripped
                                            and not stripped.startswith("//")):
            stripped = stripped[5:] if stripped.startswith("case ") else stripped
            for piece in stripped.split(","):
                piece = piece.strip()
                if re.fullmatch(r"[A-Za-z][A-Za-z0-9]*", piece):
                    names.append(piece)
    return names


def catalog_entries(text):
    """`entry("id", .verdict, …)` rows, in order."""
    return re.findall(r'entry\("([A-Za-z0-9]+)",\s*\.([a-z]+)', text)


def mac_presets(text):
    """`case .x: return [.a, .b]` inside `var features: Set<AppFeature>`."""
    start = text.index("var features: Set<AppFeature> {")
    body = text[start:text.index("\n    }", start)]
    presets, current = {}, None
    for line in body.split("\n"):
        stripped = line.strip()
        matched = re.match(r"case \.([A-Za-z]+):", stripped)
        if matched:
            current = matched.group(1)
            presets[current] = []
            stripped = stripped[matched.end():].strip()
        if current is None:
            continue
        presets[current].extend(re.findall(r"\.([a-z][A-Za-z0-9]*)[,\]]", stripped))
    return presets


def core_presets(text):
    out = {}
    for match in re.finditer(
        r'FeaturePresetDefinition\(id: "([A-Za-z]+)", platform: \.([A-Za-z]+), featureIDs: \[(.*?)\]\)',
        text, re.S
    ):
        out[match.group(1)] = (match.group(2),
                               re.findall(r'"([A-Za-z0-9]+)"', match.group(3)))
    return out


def main():
    catalog_text = read(CATALOG)
    support_text = read(SUPPORT)
    presets_text = read(PRESETS)

    features = app_features(catalog_text)
    entries = catalog_entries(support_text)
    entry_ids = [name for name, _ in entries]
    problems = []

    if len(set(entry_ids)) != len(entry_ids):
        problems.append("FeatureSupportCatalog has a duplicate id")
    missing = [name for name in features if name not in entry_ids]
    extra = [name for name in entry_ids if name not in features]
    if missing:
        problems.append("not in FeatureSupportCatalog: %s" % ", ".join(missing))
    if extra:
        problems.append("in FeatureSupportCatalog but not in AppFeature: %s"
                        % ", ".join(extra))
    if not problems and features != entry_ids:
        problems.append("FeatureSupportCatalog is not in AppFeature.allCases order")

    core = core_presets(support_text)
    for name, ids in mac_presets(presets_text).items():
        mirrored = core.get(name)
        if mirrored is None:
            problems.append("macOS preset %r is not mirrored in FeaturePresetCatalog" % name)
        elif mirrored[0] != "macOS":
            problems.append("preset %r is mirrored on the wrong platform" % name)
        elif sorted(mirrored[1]) != sorted(ids):
            problems.append("preset %r differs: FeaturePresets.swift has %s, "
                            "FeaturePresetCatalog has %s"
                            % (name, sorted(ids), sorted(mirrored[1])))

    for name, (platform, ids) in core.items():
        unknown = [one for one in ids if one not in features]
        if unknown:
            problems.append("preset %r names unknown features: %s"
                            % (name, ", ".join(unknown)))
        if platform not in ("macOS", "linux"):
            problems.append("preset %r has platform %r" % (name, platform))

    if problems:
        for problem in problems:
            print("error: %s" % problem, file=sys.stderr)
        sys.exit(1)

    linux = [name for name, verdict in entries if verdict != "drop"]
    print("feature catalog OK: %d features, %d on Linux, %d presets"
          % (len(features), len(linux), len(core)))


if __name__ == "__main__":
    main()
