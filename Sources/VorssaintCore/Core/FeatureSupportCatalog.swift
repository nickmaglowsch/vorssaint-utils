// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// Which product a feature can appear in at all.
public enum FeaturePlatform: String, CaseIterable, Codable {
    case macOS
    case linux

    /// The one this build is.
    public static var current: FeaturePlatform {
        #if canImport(Darwin)
        return .macOS
        #else
        return .linux
        #endif
    }
}

/// The triage verdict from `docs/linux-port/FEATURE_TRIAGE.md`, carried in
/// code so the hub and the docs cannot drift apart silently.
public enum FeaturePortVerdict: String, Codable {
    /// Same behaviour, new backend.
    case port
    /// Ships with a documented subset that depends on the desktop session.
    case reduced
    /// Same user need, different mechanism.
    case reimagine
    /// No Linux counterpart; hidden from the hub there.
    case drop
}

/// What one feature needs before it may be offered.
///
/// Three independent gates, in the order they are cheapest to answer:
/// the platform (a `Drop` feature simply is not on Linux), the capabilities
/// the running session probed (`PLATFORM.md` § 3), and the hardware check
/// that already existed for fan control and stays in the Mac layer because
/// its answer comes from a service.
public struct FeatureSupportRequirement: Equatable {
    /// `AppFeature.rawValue`. A string rather than the enum because
    /// `FeatureCatalog.swift` could not move to the core — it is behind
    /// `RadialMenuSupport` and `GlobalShortcut` (`CORE_MOVES.md` appendix A),
    /// and the raw value is the stable identity anyway: it is what the
    /// availability key and every settings backup are written with.
    public let id: String

    /// Where the feature exists at all.
    public let platforms: Set<FeaturePlatform>

    /// Capabilities every one of which the session must have before the
    /// feature is offered **on Linux**. macOS has none by construction: WP-15
    /// must not change what the macOS product offers, and the macOS gate is
    /// the proof. macOS support is the platform gate plus the hardware check,
    /// exactly as before.
    public let linuxCapabilities: [PlatformCapability]

    /// The triage verdict, for the hub's explanation and for the docs check.
    public let verdict: FeaturePortVerdict

    public init(id: String,
                platforms: Set<FeaturePlatform>,
                linuxCapabilities: [PlatformCapability] = [],
                verdict: FeaturePortVerdict) {
        self.id = id
        self.platforms = platforms
        self.linuxCapabilities = linuxCapabilities
        self.verdict = verdict
    }

    /// Capabilities required on `platform`. Always empty on macOS.
    public func capabilities(on platform: FeaturePlatform) -> [PlatformCapability] {
        platform == .linux ? linuxCapabilities : []
    }
}

/// Every feature's platform support, in one table.
///
/// This replaces the fan-control-only `isHardwareSupported`: a feature is
/// offered when it exists on this platform, the session has what it needs,
/// and the hardware is there. The Linux column is `FEATURE_TRIAGE.md`, which
/// stays the source of truth; the WP-15 section there maps each row to the
/// capabilities spelled here.
public enum FeatureSupportCatalog {
    private static let onBoth: Set<FeaturePlatform> = [.macOS, .linux]
    private static let macOnly: Set<FeaturePlatform> = [.macOS]

    /// Shorthand: every entry is on macOS, because the macOS product is the
    /// status quo and WP-15 changes nothing about it.
    private static func entry(_ id: String,
                              _ verdict: FeaturePortVerdict,
                              _ capabilities: [PlatformCapability] = []) -> FeatureSupportRequirement {
        FeatureSupportRequirement(id: id,
                                  platforms: verdict == .drop ? macOnly : onBoth,
                                  linuxCapabilities: verdict == .drop ? [] : capabilities,
                                  verdict: verdict)
    }

    /// In `AppFeature.allCases` order, so the two lists read side by side.
    public static let requirements: [FeatureSupportRequirement] = [
        // Windows and Dock
        entry("switcher", .reduced, [.windowList, .windowFocus]),
        entry("dockPreview", .drop),
        entry("dockClick", .drop),
        entry("windowMaximizer", .drop),
        entry("windowLayout", .reduced, [.windowList, .windowMoveResize]),
        entry("autoQuit", .reduced, [.windowList, .launchQuitApplication]),
        // Mouse and keyboard. Everything that modifies input rides the
        // InputRelay in vorssaint-helper, so it needs the helper's verbs.
        entry("scrollInverter", .port, [.inputSwallow, .inputSynthesize]),
        entry("focusFollowsMouse", .reimagine),
        entry("smoothScroll", .port, [.inputSwallow, .inputSynthesize]),
        entry("mouseAcceleration", .reimagine),
        entry("mouseNavigation", .reduced, [.inputSwallow, .inputSynthesize]),
        entry("mouseButtonShortcuts", .port, [.inputSwallow, .inputSynthesize]),
        entry("middleClick", .drop),
        entry("mouseClickDebounce", .port, [.inputSwallow]),
        entry("keyboardDebounce", .port, [.inputSwallow]),
        entry("textSnippets", .port, [.inputSwallow, .inputSynthesize]),
        entry("superKey", .port, [.inputSwallow, .inputSynthesize]),
        entry("quitWindowProtection", .port, [.inputSwallow]),
        // Clipboard and files
        entry("clipboardHistory", .reduced, [.clipboardRead, .clipboardWatch]),
        entry("pastePlain", .reduced, [.clipboardRead, .clipboardWrite, .inputSynthesize]),
        entry("finderCutPaste", .drop),
        entry("finderRename", .drop),
        entry("shelf", .reduced),
        entry("urlCleaner", .port, [.clipboardRead, .clipboardWrite, .clipboardWatch]),
        entry("diskImageInstaller", .drop),
        // Sound
        entry("mixer", .port, [.audioStreamVolume]),
        entry("soundOutputSwitcher", .port, [.audioDefaultDeviceSwitch]),
        entry("micMute", .port, [.audioSourceVolume]),
        entry("musicBlock", .drop),
        // Energy and display
        entry("keepAwake", .port, [.powerInhibitSystemSleep]),
        entry("brightness", .port, [.powerInternalBrightness]),
        entry("extraBrightness", .drop),
        entry("bluetoothSleep", .port, [.sessionSleepWakeEvents]),
        // Tools
        entry("quickLauncher", .port, [.launchApplications, .launchEnumerateInstalled]),
        entry("quickToggles", .reduced, [.powerLockSession]),
        entry("colorPicker", .port, [.captureArea]),
        entry("screenOCR", .port, [.captureArea]),
        entry("cleaningMode", .port, [.inputSwallow]),
        entry("mediaTools", .port),
        entry("cleaner", .port, [.filesTrash]),
        entry("uninstaller", .reimagine, [.packagesList, .packagesUninstall]),
        entry("homebrew", .reimagine, [.packagesList]),
        entry("appUpdates", .reimagine, [.packagesList, .packagesRefresh]),
        entry("screenshot", .port, [.captureDisplay, .captureArea]),
        entry("cameraPreview", .port),
        entry("radialMenu", .port),
        entry("scratchpad", .port),
        entry("commandBar", .reduced, [.launchApplications, .launchEnumerateInstalled]),
        entry("screenRecorder", .port, [.captureStream]),
        entry("killProcess", .port),
        // System monitor. monitorGPU is Reduced for a reason no capability
        // can carry: coverage depends on the vendor driver (amdgpu sysfs,
        // NVML, i915), which the sensors backend reports per machine.
        entry("monitorCPU", .port, [.sensorsCPU]),
        entry("monitorGPU", .reduced),
        entry("monitorMemory", .port, [.sensorsMemory]),
        entry("monitorNetwork", .port, [.sensorsNetwork]),
        // Reduced, not Port: rates and capacity are unprivileged and
        // complete, but SMART and NVMe health need ioctls on the raw device
        // (SENSORS_BACKEND.md, and the lead's decision recorded on the
        // monitorDisk row of FEATURE_TRIAGE.md). The health rows are absent
        // until the helper grows a path for them, so there is no capability
        // to name for them yet.
        entry("monitorDisk", .reduced, [.sensorsDiskActivity]),
        entry("monitorPower", .port, [.sensorsBattery]),
        entry("fanControl", .reduced, [.powerFanControl, .sensorsFanSpeed]),
    ]

    private static let byID: [String: FeatureSupportRequirement] =
        Dictionary(uniqueKeysWithValues: requirements.map { ($0.id, $0) })

    /// Every feature id the catalog knows, in declaration order.
    public static var allFeatureIDs: [String] { requirements.map(\.id) }

    /// The ids a platform offers, in declaration order.
    public static func featureIDs(on platform: FeaturePlatform) -> [String] {
        requirements.filter { $0.platforms.contains(platform) }.map(\.id)
    }

    /// A feature the table does not know is treated as present on macOS with
    /// nothing required. A case added to `AppFeature` and forgotten here
    /// therefore cannot change what the macOS product offers; it is caught by
    /// `Tools/linux-port/check-feature-catalog.py` in CI instead.
    public static func requirement(for id: String) -> FeatureSupportRequirement {
        byID[id] ?? FeatureSupportRequirement(id: id, platforms: [.macOS], verdict: .port)
    }

    /// The gate every install surface passes.
    public static func isSupported(_ id: String,
                                   on platform: FeaturePlatform = .current,
                                   has capability: (PlatformCapability) -> Bool = { _ in true }) -> Bool {
        missingRequirements(id, on: platform, has: capability) == nil
    }

    /// `nil` when the feature is offerable. Otherwise what is standing in the
    /// way: an empty array means the platform itself has no counterpart.
    public static func missingRequirements(
        _ id: String,
        on platform: FeaturePlatform = .current,
        has capability: (PlatformCapability) -> Bool = { _ in true }
    ) -> [PlatformCapability]? {
        let requirement = self.requirement(for: id)
        guard requirement.platforms.contains(platform) else { return [] }
        let missing = requirement.capabilities(on: platform).filter { !capability($0) }
        return missing.isEmpty ? nil : missing
    }

    /// Why the feature cannot be offered, ready to show in the hub. `nil`
    /// when it can.
    ///
    /// The capability names are not translated: they are the probe identities
    /// the Capabilities page lists, and a person reporting "portal
    /// ScreenCast is missing" is better served by the name the rest of the
    /// desktop uses than by a localized paraphrase of it.
    static func unsupportedReason(_ id: String,
                                  on platform: FeaturePlatform = .current,
                                  has capability: (PlatformCapability) -> Bool = { _ in true },
                                  language: AppLanguage) -> String? {
        guard let missing = missingRequirements(id, on: platform, has: capability) else {
            return nil
        }
        let strings = FeatureStrings.hub(language)
        guard !missing.isEmpty else { return strings.unsupportedOnThisSystem }
        let names = missing.map(\.rawValue).sorted().joined(separator: ", ")
        return String(format: strings.unsupportedMissingCapabilitiesFormat, names)
    }
}

/// A one-click starting point for the hub, per platform.
///
/// The macOS rows mirror `Sources/Vorssaint/Core/FeaturePresets.swift`, which
/// stays the macOS surface until the hub itself is ported: the Mac harness
/// pins which files may write the availability key, so moving the writer is a
/// change for the hub's own work package, not for this one. The mirror is
/// checked against the real one by
/// `Tools/linux-port/check-feature-catalog.py`.
public struct FeaturePresetDefinition: Equatable {
    public let id: String
    public let platform: FeaturePlatform
    public let featureIDs: [String]

    public init(id: String, platform: FeaturePlatform, featureIDs: [String]) {
        self.id = id
        self.platform = platform
        self.featureIDs = featureIDs
    }
}

public enum FeaturePresetCatalog {
    public static let presets: [FeaturePresetDefinition] = [
        // macOS, mirroring FeaturePresets.swift.
        FeaturePresetDefinition(id: "essential", platform: .macOS, featureIDs: [
            "mixer", "keepAwake",
            "monitorCPU", "monitorGPU", "monitorMemory",
            "monitorNetwork", "monitorDisk", "monitorPower",
        ]),
        FeaturePresetDefinition(id: "windows", platform: .macOS, featureIDs: [
            "switcher", "windowLayout", "dockPreview", "dockClick", "windowMaximizer",
        ]),
        FeaturePresetDefinition(id: "battery", platform: .macOS, featureIDs: [
            "monitorCPU", "monitorMemory", "monitorPower",
        ]),
        // Linux (WP-15). Essentials is the set someone can use on the first
        // launch of a fresh install, before any helper or extension exists.
        FeaturePresetDefinition(id: "linuxEssentials", platform: .linux, featureIDs: [
            "monitorCPU", "monitorGPU", "monitorMemory",
            "monitorNetwork", "monitorDisk", "monitorPower",
            "mixer", "keepAwake", "clipboardHistory", "textSnippets", "screenshot",
        ]),
        FeaturePresetDefinition(id: "linuxWindows", platform: .linux, featureIDs: [
            "switcher", "windowLayout", "autoQuit",
        ]),
        FeaturePresetDefinition(id: "linuxBatteryQuiet", platform: .linux, featureIDs: [
            "monitorPower", "keepAwake", "bluetoothSleep", "brightness",
        ]),
    ]

    public static func presets(on platform: FeaturePlatform) -> [FeaturePresetDefinition] {
        presets.filter { $0.platform == platform }
    }

    public static func preset(_ id: String) -> FeaturePresetDefinition? {
        presets.first { $0.id == id }
    }
}

extension FeaturePresetDefinition {
    /// The hub's name for the preset. The macOS rows keep the strings the
    /// macOS hub already renders; the three Linux rows are WP-15's own.
    func name(_ language: AppLanguage) -> String {
        let strings = FeatureStrings.hub(language)
        switch id {
        case "essential": return strings.presetEssentialName
        case "windows": return strings.presetWindowsName
        case "battery": return strings.presetBatteryName
        case "linuxEssentials": return strings.presetLinuxEssentialsName
        case "linuxWindows": return strings.presetLinuxWindowsName
        case "linuxBatteryQuiet": return strings.presetLinuxBatteryQuietName
        default: return id
        }
    }

    func summary(_ language: AppLanguage) -> String {
        let strings = FeatureStrings.hub(language)
        switch id {
        case "essential": return strings.presetEssentialDesc
        case "windows": return strings.presetWindowsDesc
        case "battery": return strings.presetBatteryDesc
        case "linuxEssentials": return strings.presetLinuxEssentialsDesc
        case "linuxWindows": return strings.presetLinuxWindowsDesc
        case "linuxBatteryQuiet": return strings.presetLinuxBatteryQuietDesc
        default: return ""
        }
    }

    /// A preset is only worth offering when every feature in it can actually
    /// be installed on the platform it belongs to.
    public func resolves(has capability: (PlatformCapability) -> Bool = { _ in true }) -> Bool {
        !featureIDs.isEmpty && featureIDs.allSatisfy {
            FeatureSupportCatalog.isSupported($0, on: platform, has: capability)
        }
    }
}
