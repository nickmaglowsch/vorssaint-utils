// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// WP-00 static-linking probe: a small Foundation program that must survive
// `swift build --static-swift-stdlib`, so `ldd` can show what the produced
// binary still needs from the host.

import Foundation

struct Probe: Codable {
    let name: String
    let locales: [String]
    let started: Date
}

let probe = Probe(
    name: "vorssaint-wp00",
    locales: ["en", "de", "fr", "es", "pt-BR", "it", "nl", "ja", "ko", "zh-Hans", "ru", "pl", "tr"],
    started: Date(timeIntervalSince1970: 0)
)

let encoder = JSONEncoder()
encoder.outputFormatting = [.sortedKeys]
let data = try encoder.encode(probe)
let round = try JSONDecoder().decode(Probe.self, from: data)

var components = URLComponents(string: "https://example.com/a?utm_source=x&keep=1")!
components.queryItems = components.queryItems?.filter { !$0.name.hasPrefix("utm_") }

let formatter = ByteCountFormatter()
formatter.countStyle = .file

print("json=\(String(decoding: data, as: UTF8.self))")
print("roundtrip=\(round.locales.count) locales")
print("url=\(components.url!.absoluteString)")
print("bytes=\(formatter.string(fromByteCount: 1_234_567))")
print("regex=\(try NSRegularExpression(pattern: "^v\\d+$").numberOfCaptureGroups)")
print("ok")
