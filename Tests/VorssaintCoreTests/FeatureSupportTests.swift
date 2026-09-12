// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// WP-15. Hand-written; the selection tool only emits Generated*.swift.

import Foundation
import XCTest
@testable import VorssaintCore

final class FeatureSupportTests: XCTestCase {
    /// The nine features `FEATURE_TRIAGE.md` drops. Spelled out rather than
    /// derived, so a verdict changed in the table without a line in the triage
    /// document fails here.
    private let dropped: Set<String> = [
        "dockPreview", "dockClick", "windowMaximizer", "middleClick",
        "finderCutPaste", "finderRename", "diskImageInstaller", "musicBlock",
        "extraBrightness",
    ]

    private var everything: (PlatformCapability) -> Bool { { _ in true } }
    private var nothing: (PlatformCapability) -> Bool { { _ in false } }

    // MARK: - The catalog itself

    func testTheCatalogCoversEveryFeatureExactlyOnce() {
        let ids = FeatureSupportCatalog.allFeatureIDs
        XCTAssertEqual(ids.count, 57, "AppFeature has 57 cases")
        XCTAssertEqual(ids.count, Set(ids).count, "no id appears twice")
        XCTAssertTrue(ids.allSatisfy { !$0.isEmpty })
    }

    func testTheVerdictsMatchTheTriageTotals() {
        var totals: [FeaturePortVerdict: Int] = [:]
        for requirement in FeatureSupportCatalog.requirements {
            totals[requirement.verdict, default: 0] += 1
        }
        // FEATURE_TRIAGE.md "Totals": Port 32, Reduced 11, Re-imagine 5,
        // Drop 9.
        XCTAssertEqual(totals[.port], 32)
        XCTAssertEqual(totals[.reduced], 11)
        XCTAssertEqual(totals[.reimagine], 5)
        XCTAssertEqual(totals[.drop], 9)
    }

    func testTheLinuxCatalogIsTheTriageMinusTheDroppedFeatures() {
        let linux = Set(FeatureSupportCatalog.featureIDs(on: .linux))
        let all = Set(FeatureSupportCatalog.allFeatureIDs)
        XCTAssertEqual(linux.count, 48, "57 features less the 9 dropped ones")
        XCTAssertEqual(all.subtracting(linux), dropped,
                       "exactly the Drop rows of FEATURE_TRIAGE.md are hidden on Linux")
        for id in dropped {
            XCTAssertEqual(FeatureSupportCatalog.requirement(for: id).verdict, .drop, id)
            XCTAssertFalse(FeatureSupportCatalog.isSupported(id, on: .linux, has: everything), id)
            XCTAssertEqual(FeatureSupportCatalog.missingRequirements(id, on: .linux,
                                                                    has: everything), [],
                           "a dropped feature is a platform answer, not a capability one")
        }
    }

    /// The macOS half of WP-15 has to be a rename and nothing else: the macOS
    /// gate is what proves the product did not change, and it can only prove
    /// that if the catalog cannot say no on macOS for a new reason.
    func testMacOSSupportIsUnconditional() {
        for requirement in FeatureSupportCatalog.requirements {
            XCTAssertTrue(requirement.platforms.contains(.macOS), requirement.id)
            XCTAssertTrue(requirement.capabilities(on: .macOS).isEmpty,
                          "\(requirement.id) must need no capability on macOS")
            XCTAssertTrue(FeatureSupportCatalog.isSupported(requirement.id, on: .macOS,
                                                            has: nothing),
                          "\(requirement.id) is offered on macOS whatever the probes say")
            XCTAssertNil(FeatureSupportCatalog.unsupportedReason(requirement.id, on: .macOS,
                                                                 has: nothing, language: .enUS),
                         requirement.id)
        }
    }

    func testEveryLinuxCapabilityIsOneThePlatformLayerDeclares() {
        // The raw values of PLATFORM.md § 3. A typo here would make a feature
        // permanently unsupported on every session, silently.
        let known = Set(
            FeatureSupportCatalog.requirements.flatMap { $0.linuxCapabilities }.map(\.rawValue))
        let declared: Set<String> = [
            "window.list", "window.focus", "window.moveResize",
            "launch.quitApplication", "launch.applications", "launch.enumerateInstalled",
            "input.swallow", "input.synthesize",
            "clipboard.read", "clipboard.write", "clipboard.watch",
            "audio.streamVolume", "audio.sourceVolume", "audio.defaultDeviceSwitch",
            "power.inhibitSystemSleep", "power.internalBrightness", "power.lockSession",
            "power.fanControl",
            "session.sleepWakeEvents",
            "capture.area", "capture.display", "capture.stream",
            "files.trash",
            "packages.list", "packages.uninstall", "packages.refresh",
            "sensors.cpu", "sensors.memory", "sensors.network", "sensors.diskActivity",
            "sensors.battery", "sensors.fanSpeed",
        ]
        XCTAssertEqual(known, declared,
                       "every capability a feature names is one a backend declares")
    }

    // MARK: - What the hub tells the person

    func testAFeatureWhoseCapabilitiesAreAbsentSaysWhy() {
        let missing = FeatureSupportCatalog.missingRequirements("mixer", on: .linux, has: nothing)
        XCTAssertEqual(missing, [.audioStreamVolume])

        for language in AppLanguage.allCases {
            let text = FeatureSupportCatalog.unsupportedReason(
                "mixer", on: .linux, has: nothing, language: language) ?? ""
            XCTAssertTrue(text.contains("audio.streamVolume"),
                          "the reason names the capability (\(language.rawValue)): \(text)")
            XCTAssertFalse(text.contains("%@"),
                           "the format was filled in (\(language.rawValue))")

            let droppedReason = FeatureSupportCatalog.unsupportedReason(
                "dockClick", on: .linux, has: everything, language: language)
            XCTAssertEqual(droppedReason, FeatureStrings.hub(language).unsupportedOnThisSystem,
                           language.rawValue)
        }
    }

    func testASessionWithEverythingOffersEveryLinuxFeature() {
        for id in FeatureSupportCatalog.featureIDs(on: .linux) {
            XCTAssertTrue(FeatureSupportCatalog.isSupported(id, on: .linux, has: everything), id)
            XCTAssertNil(FeatureSupportCatalog.unsupportedReason(id, on: .linux,
                                                                 has: everything, language: .enUS),
                         id)
        }
    }

    func testASessionWithNothingKeepsOnlyWhatNeedsNothing() {
        let offered = FeatureSupportCatalog.featureIDs(on: .linux)
            .filter { FeatureSupportCatalog.isSupported($0, on: .linux, has: nothing) }
        // A bare session with no helper, no portal and no compositor bridge
        // still has the features that stand on nothing but the file system,
        // plus the two the port re-imagines as a desktop-setting switch.
        XCTAssertEqual(Set(offered), [
            "focusFollowsMouse", "mouseAcceleration", "shelf", "mediaTools",
            "cameraPreview", "radialMenu", "scratchpad", "killProcess", "monitorGPU",
        ])
    }

    func testEveryHubStringForWP15IsSetInEveryLanguage() {
        for language in AppLanguage.allCases {
            let hub = FeatureStrings.hub(language)
            let new = [hub.presetLinuxEssentialsName, hub.presetLinuxEssentialsDesc,
                       hub.presetLinuxWindowsName, hub.presetLinuxWindowsDesc,
                       hub.presetLinuxBatteryQuietName, hub.presetLinuxBatteryQuietDesc,
                       hub.unsupportedOnThisSystem, hub.unsupportedMissingCapabilitiesFormat]
            XCTAssertTrue(new.allSatisfy { !$0.isEmpty }, language.rawValue)
            XCTAssertTrue(new.allSatisfy { !$0.contains("—") },
                          "human punctuation (\(language.rawValue))")
            XCTAssertTrue(hub.unsupportedMissingCapabilitiesFormat.contains("%@"),
                          "the capability list has somewhere to go (\(language.rawValue))")
        }
    }

    // MARK: - Presets

    func testPresetsResolveToInstallableSetsOnBothPlatforms() {
        for platform in FeaturePlatform.allCases {
            let presets = FeaturePresetCatalog.presets(on: platform)
            XCTAssertEqual(presets.count, 3, "three presets on \(platform.rawValue)")
            for preset in presets {
                XCTAssertFalse(preset.featureIDs.isEmpty, preset.id)
                XCTAssertEqual(preset.featureIDs.count, Set(preset.featureIDs).count, preset.id)
                XCTAssertTrue(preset.resolves(),
                              "\(preset.id) installs on \(platform.rawValue)")
                for id in preset.featureIDs {
                    XCTAssertTrue(
                        FeatureSupportCatalog.requirement(for: id).platforms.contains(platform),
                        "\(preset.id) names \(id), which is not on \(platform.rawValue)")
                }
            }
        }
    }

    func testTheThreeLinuxPresetsAreTheOnesTheWorkPackageNames() {
        let byID = Dictionary(uniqueKeysWithValues:
            FeaturePresetCatalog.presets(on: .linux).map { ($0.id, Set($0.featureIDs)) })
        XCTAssertEqual(byID["linuxEssentials"], [
            "monitorCPU", "monitorGPU", "monitorMemory", "monitorNetwork",
            "monitorDisk", "monitorPower", "mixer", "keepAwake",
            "clipboardHistory", "textSnippets", "screenshot",
        ])
        XCTAssertEqual(byID["linuxWindows"], ["switcher", "windowLayout", "autoQuit"])
        XCTAssertEqual(byID["linuxBatteryQuiet"],
                       ["monitorPower", "keepAwake", "bluetoothSleep", "brightness"])
    }

    func testEveryPresetIsNamedAndDescribedInEveryLanguage() {
        for preset in FeaturePresetCatalog.presets {
            for language in AppLanguage.allCases {
                XCTAssertFalse(preset.name(language).isEmpty,
                               "\(preset.id)/\(language.rawValue)")
                XCTAssertNotEqual(preset.name(language), preset.id,
                                  "\(preset.id)/\(language.rawValue) falls through to the id")
                XCTAssertFalse(preset.summary(language).isEmpty,
                               "\(preset.id)/\(language.rawValue)")
            }
        }
    }

    /// A preset whose features need a helper that is not installed is still a
    /// preset; it just cannot be applied whole. The Windows preset on a GNOME
    /// session with no bridge is the real case.
    func testAPresetOnASessionThatCannotRunItDoesNotResolve() {
        let windows = FeaturePresetCatalog.preset("linuxWindows")
        XCTAssertNotNil(windows)
        XCTAssertFalse(windows?.resolves(has: { $0 != .windowMoveResize }) ?? true,
                       "no window.moveResize means the layout half cannot install")
        XCTAssertTrue(windows?.resolves() ?? false)
    }
}
