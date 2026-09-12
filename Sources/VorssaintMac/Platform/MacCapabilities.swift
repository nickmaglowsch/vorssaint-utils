// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

#if os(macOS)
import AppKit
import Foundation

/// The macOS `Capabilities`: the aggregate every feature asks before it
/// offers itself.
///
/// It probes by *asking the other implementations*, never by hard-coding a
/// per-OS table — the same rule the helper's `GetCapabilities` follows
/// (`docs/linux-port/PRIVILEGES.md` § 4.1: answer by trying). So as each
/// per-feature work package wires a real implementation behind one of the
/// thirteen protocols, this page gets truer without being edited.
final class MacCapabilities: Capabilities {
    private let services: [PlatformService]
    private var cached: [CapabilityProbe] = []
    var onChange: (() -> Void)?

    init(services: [PlatformService]) {
        self.services = services
        refresh()
    }

    /// The default set: every macOS implementation WP-12 wrote.
    convenience init() {
        self.init(services: [
            MacWindowSystem(),
            MacClipboardAccess(),
            MacScreenCapturer(),
            MacAudioGraph(),
            MacSystemSensors(),
            MacPowerControl(),
            MacInputInterceptor(),
            MacAppLauncher(),
            MacNotifier(),
            MacTrashAndFiles(),
            MacPackageManager(),
            MacSessionEvents(),
            MacShortcutRegistrar(),
        ])
    }

    var session: SessionDescription {
        SessionDescription(desktop: "macos",
                           displayProtocol: "quartz",
                           desktopVersion: ProcessInfo.processInfo
                               .operatingSystemVersionString,
                           isSandboxed: ProcessInfo.processInfo
                               .environment["APP_SANDBOX_CONTAINER_ID"] != nil)
    }

    var probes: [CapabilityProbe] { cached }

    var platformCapabilities: PlatformCapabilitySet {
        PlatformCapabilitySet(
            supported: Set(cached.filter(\.isAvailable).map(\.id)),
            unavailableReasons: cached.filter { !$0.isAvailable }
                .reduce(into: [:]) { $0[$1.id] = $1.detail })
    }

    func has(_ capability: PlatformCapability) -> Bool {
        cached.first { $0.id == capability }?.isAvailable ?? false
    }

    func explanation(for capability: PlatformCapability) -> CapabilityProbe? {
        guard let probe = cached.first(where: { $0.id == capability }) else {
            return CapabilityProbe(id: capability, isAvailable: false,
                                   detail: "no implementation reports this capability",
                                   remedy: nil)
        }
        return probe.isAvailable ? nil : probe
    }

    func refresh() {
        var probes: [CapabilityProbe] = []
        for service in services {
            let set = service.platformCapabilities
            for capability in set.supported.sorted(by: { $0.rawValue < $1.rawValue }) {
                probes.append(CapabilityProbe(id: capability, isAvailable: true,
                                              detail: nil, remedy: nil))
            }
            for (capability, reason) in set.unavailableReasons
                .sorted(by: { $0.key.rawValue < $1.key.rawValue })
                where capability != .blanket {
                probes.append(CapabilityProbe(id: capability, isAvailable: false,
                                              detail: reason, remedy: nil))
            }
        }
        cached = probes
        onChange?()
    }
}
#endif
