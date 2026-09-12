// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

// Hand-written, not generated: `Tools/linux-port/port-tests.py` only writes
// files named `Generated*.swift`, so this one survives a regeneration.
//
// What it checks is the part of WP-12 that has no macOS equivalent to compare
// against: that the Platform protocols can be conformed to and driven from
// outside `VorssaintCore`, and that the capability contract actually degrades
// instead of quietly succeeding.

import XCTest
import VorssaintCore
import VorssaintCoreTestSupport

final class PlatformProtocolTests: XCTestCase {

    // MARK: Capability contract

    func testAMissingCapabilityIsRefusedRatherThanIgnored() {
        // A window backend that can list and nothing else — a bare
        // foreign-toplevel session.
        let system = FakeWindowSystem(capabilities: [.canList],
                                      windows: [.fake(id: 1)])

        XCTAssertEqual(try system.list().count, 1,
                       "listing works when the capability is present")
        XCTAssertThrowsError(try system.activate(1),
                             "a backend that cannot activate says so") { error in
            XCTAssertEqual(error as? PlatformError, .unsupported(.windowList))
        }
        XCTAssertTrue(system.calls.isEmpty,
                      "a refused verb never reaches the backend")
    }

    func testTheUniformCapabilityFormMirrorsTheCBitmask() {
        let system = FakeWindowSystem(capabilities: [.canList, .canMoveResize])
        let reported = system.platformCapabilities

        XCTAssertTrue(reported.has(.windowList))
        XCTAssertTrue(reported.has(.windowMoveResize))
        XCTAssertFalse(reported.has(.windowClose),
                       "a bit that is clear is absent in the uniform form too")
    }

    func testAnUnavailableCapabilityCarriesAReason() {
        let set = PlatformCapabilitySet(
            supported: [.windowList],
            unavailableReasons: [.windowMoveResize: "this compositor cannot move windows"])

        XCTAssertNil(set.reason(for: .windowList),
                     "a capability that is present needs no explanation")
        XCTAssertEqual(set.reason(for: .windowMoveResize),
                       "this compositor cannot move windows")
    }

    // MARK: Read back after writing

    func testARequestThatIsAcknowledgedAndNotAppliedIsAFailure() {
        let system = FakeWindowSystem(windows: [.fake(id: 7)])
        system.moveResizeLands = false
        let wanted = WindowRect(x: 10, y: 20, width: 300, height: 400)

        XCTAssertThrowsError(try system.moveResize(7, frame: wanted, tolerance: 4)) { error in
            XCTAssertEqual(error as? PlatformError, .notApplied,
                           "a compositor that agrees and does nothing is not success")
        }
        XCTAssertEqual(system.calls, [.moveResize(7, wanted, 4)],
                       "the attempt is still recorded, so a caller can retry")
        XCTAssertEqual(try system.geometry(of: 7).cgRect,
                       WindowRect(x: 0, y: 0, width: 800, height: 600).cgRect,
                       "and the window did not move")
    }

    func testAppliedGeometryReadsBack() throws {
        let system = FakeWindowSystem(windows: [.fake(id: 7)])
        let wanted = WindowRect(x: 10, y: 20, width: 300, height: 400)

        try system.moveResize(7, frame: wanted, tolerance: 0)
        XCTAssertEqual(try system.geometry(of: 7), wanted)
    }

    // MARK: Events come only from dispatch

    func testEventsAreDeliveredOnlyFromDispatch() throws {
        let system = FakeWindowSystem(windows: [.fake(id: 1), .fake(id: 2)])
        var seen: [WindowEvent] = []
        system.setEventCallback { seen.append($0) }

        try system.close(1)
        XCTAssertTrue(seen.isEmpty,
                      "nothing arrives out of the blue; the C contract forbids it")

        XCTAssertEqual(try system.dispatch(), 1)
        XCTAssertEqual(seen, [.removed(1)])
    }

    func testCapabilitiesShrinkOnlyThroughBackendLost() throws {
        let system = FakeWindowSystem(windows: [.fake(id: 1)])
        var seen: [WindowEvent] = []
        system.setEventCallback { seen.append($0) }

        system.loseBackend(keeping: [.canList])
        XCTAssertEqual(try system.dispatch(), 1)
        XCTAssertEqual(seen, [.backendLost],
                       "the caller is told, so it knows to re-read capabilities")
        XCTAssertEqual(system.capabilities, [.canList])
        XCTAssertThrowsError(try system.activate(1))
    }

    // MARK: The helper's shape

    func testRulesGoInAndEventsDoNotComeOut() throws {
        let interceptor = FakeInputInterceptor(inputCapabilities: .helperReady)
        var recorded: [RecordedInput] = []

        try interceptor.setRules(InputRules(tapHold: true, tapThresholdMilliseconds: 200,
                                            tapSource: 58, tapOutput: 1, holdOutput: 29))
        XCTAssertEqual(interceptor.acceptedRules.count, 1)

        interceptor.emit(RecordedInput(kind: .keyDown, keyToken: "a", button: nil,
                                       modifiers: [], timestamp: 0))
        XCTAssertTrue(recorded.isEmpty,
                      "outside tap mode the helper emits nothing at all")

        try interceptor.startRecordingTap { recorded.append($0) }
        interceptor.emit(RecordedInput(kind: .keyDown, keyToken: "a", button: nil,
                                       modifiers: [], timestamp: 0))
        XCTAssertEqual(recorded.count, 1, "tap mode is the one place keys cross")

        interceptor.stopRecordingTap()
        interceptor.emit(RecordedInput(kind: .keyDown, keyToken: "b", button: nil,
                                       modifiers: [], timestamp: 1))
        XCTAssertEqual(recorded.count, 1, "and it ends when recording ends")
    }

    func testARejectedRuleDocumentLeavesThePreviousOneInForce() throws {
        let interceptor = FakeInputInterceptor(inputCapabilities: .helperReady)
        try interceptor.setRules(InputRules(chatter: true, chatterMilliseconds: 40))

        XCTAssertThrowsError(try interceptor.setRules(
            InputRules(tapThresholdMilliseconds: 9_000)))
        XCTAssertTrue(interceptor.rules.chatter,
                      "the helper keeps the last good document when it refuses one")
    }

    func testAMissingHelperExplainsItselfWithTheErrno() {
        let interceptor = FakeInputInterceptor()
        let reported = interceptor.platformCapabilities

        XCTAssertFalse(reported.has(.inputSynthesize))
        XCTAssertEqual(reported.reason(for: .inputSynthesize),
                       "open /dev/uinput: No such file or directory (errno 2)",
                       "ENOENT and ENODEV are different problems, so the errno is kept")
        XCTAssertTrue(reported.has(.inputRequiresHelper))
    }

    // MARK: The seams the core reads through

    func testTheDefaultImageValidatorKeepsStoredIconsRatherThanDroppingThem() {
        // A platform with no decoder must not silently delete a person's
        // custom icons; the macOS build installs AppKitImageDataValidator on
        // its first line and gets the real check.
        let validator = PermissiveImageDataValidator()
        XCTAssertTrue(validator.isValidImageData(Data([0x00])))
        XCTAssertFalse(validator.isValidImageData(Data()))
    }

    func testAnInjectedValidatorIsWhatTheCoreActuallyCalls() {
        let previous = ImageDataValidation.current
        defer { ImageDataValidation.current = previous }

        ImageDataValidation.current = ClosureImageDataValidator { $0.count > 3 }
        XCTAssertFalse(ImageDataValidation.current.isValidImageData(Data([1, 2])))
        XCTAssertTrue(ImageDataValidation.current.isValidImageData(Data([1, 2, 3, 4])))
    }

    func testTheSymbolMeasurementFormatterKeepsTheNumberAndTheUnitSymbol() {
        let formatted = SymbolMeasurementFormatter().string(
            from: Measurement<Dimension>(value: 10.874, unit: UnitLength.inches),
            locale: Locale(identifier: "en_US"),
            maximumFractionDigits: 2)

        XCTAssertEqual(formatted, "10.87 in",
                       "the number is localized; the unit is its own symbol")
    }

    func testTheTransliteratorReportsWhatItCanActuallyDo() {
        let none = NoTransliterator()
        XCTAssertNil(none.mandarinLatin("云笔记"))
        XCTAssertFalse(none.capabilities.canRomanizeMandarin,
                       "a platform that cannot romanize says so rather than "
                       + "indexing the title unchanged")
    }

    // MARK: Cross-protocol aggregation

    func testTheCapabilitiesPageExplainsWhatIsMissing() {
        let capabilities = FakeCapabilities(session: .fakeSandboxed)
        capabilities.set(.windowList, available: true)
        capabilities.set(.windowMoveResize, available: false,
                         detail: "GNOME implements no window-management protocol",
                         remedy: "install the vorssaint-bridge extension")

        XCTAssertTrue(capabilities.supportsAll([.windowList]))
        XCTAssertFalse(capabilities.supportsAll([.windowList, .windowMoveResize]))

        let missing = capabilities.missing(from: [.windowList, .windowMoveResize])
        XCTAssertEqual(missing.map(\.id), [.windowMoveResize])
        XCTAssertEqual(missing.first?.remedy, "install the vorssaint-bridge extension")
    }

    func testEveryFakeAnswersItsOwnCapabilityQuestion() {
        // One assertion that all thirteen conform and can be constructed, which
        // is what makes them usable as a drop-in for a service under test.
        let services: [PlatformService] = [
            FakeWindowSystem(),
            FakeClipboardAccess(),
            FakeScreenCapturer(),
            FakeAudioGraph(),
            FakeSystemSensors(),
            FakePowerControl(),
            FakeInputInterceptor(),
            FakeAppLauncher(),
            FakeNotifier(),
            FakeTrashAndFiles(),
            FakePackageManager(),
            FakeSessionEvents(),
            FakeShortcutRegistrar(),
            FakeCapabilities(),
        ]
        XCTAssertEqual(services.count, 14)
        for service in services {
            _ = service.platformCapabilities
        }
    }

    func testThermalPressureHasTheFourLevelsTheFanPolicyReads() {
        let sensors = FakeSystemSensors()
        var seen: [ThermalPressure] = []
        _ = sensors.observeThermalPressure { seen.append($0) }

        for level in ThermalPressure.allCases { sensors.setThermalPressure(level) }
        XCTAssertEqual(seen, [.nominal, .fair, .serious, .critical])
        XCTAssertEqual(sensors.thermalPressure, .critical)
    }
}
