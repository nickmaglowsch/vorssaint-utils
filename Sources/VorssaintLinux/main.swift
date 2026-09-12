// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

// Placeholder entry point for the Linux executable (WP-10).
//
// SwiftPM has no way to declare a target Linux-only, so the body is guarded:
// on macOS this compiles to an executable that does nothing, which keeps
// `swift build` on macOS green without a second package manifest.

#if os(Linux)
import Foundation
import VorssaintCore

print(VorssaintCoreVersion.banner)
print("VorssaintLinux \(VorssaintCoreVersion.string) placeholder — no UI yet")

// WP-12 probe: does swift-corelibs-foundation's ICU actually carry the
// Mandarin-Latin transliterator `CommandBarSearch.pinyinKeywords` needs, or
// must the Linux build declare that capability off? Printed, not asserted, so
// the answer is recorded in the CI log of every run.
let transliterator = FoundationTransliterator()
print("transliterator: \(transliterator.capabilities)")
print("transliterator mandarinLatin(\"\u{4e2d}\u{6587}\") = "
      + String(describing: transliterator.mandarinLatin("\u{4e2d}\u{6587}")))
print("transliterator StringTransform(\"Any-Latin\") = "
      + String(describing: "\u{4e2d}\u{6587}".applyingTransform(StringTransform("Any-Latin"), reverse: false)))
print("transliterator StringTransform(\"Mandarin-Latin\") = "
      + String(describing: "\u{4e2d}\u{6587}".applyingTransform(StringTransform("Mandarin-Latin"), reverse: false)))
print("platform window id width: \(MemoryLayout<PlatformWindowID>.size * 8) bits")
#endif
