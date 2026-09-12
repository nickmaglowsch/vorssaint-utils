// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

#if canImport(CoreGraphics)
import CoreGraphics
#endif
import Foundation
import VorssaintCore

// The remaining `Fake*` platform implementations. Each one records what it was
// asked to do and answers from a value a test can set, so a service can be
// exercised without a compositor, a sound server, a helper or a network.
//
// They all share one rule with the real backends: a call whose capability is
// absent throws `PlatformError.unsupported` rather than quietly succeeding.
// That is what makes a test able to prove a feature degrades honestly.

// MARK: - ShortcutRegistrar

public final class FakeShortcutRegistrar: ShortcutRegistrar {
    public var platformCapabilities: PlatformCapabilitySet
    public var bindingIsChosenBySession: Bool
    /// Bindings a test declares already taken by another application.
    public var takenBindings: Set<ShortcutBinding> = []

    public private(set) var activeBindings: [ShortcutHandle: ShortcutBinding] = [:]

    private var handlers: [ShortcutHandle: () -> Void] = [:]
    private var identifiers: [ShortcutHandle: String] = [:]
    private var nextHandle: UInt64 = 1

    public init(platformCapabilities: PlatformCapabilitySet = .all(.shortcutDirectBinding,
                                                                  .shortcutLayoutAwareLabels),
                bindingIsChosenBySession: Bool = false) {
        self.platformCapabilities = platformCapabilities
        self.bindingIsChosenBySession = bindingIsChosenBySession
    }

    public func register(_ binding: ShortcutBinding,
                         identifier: String,
                         onActivate: @escaping () -> Void) throws -> ShortcutHandle {
        guard platformCapabilities.has(.shortcutDirectBinding)
                || platformCapabilities.has(.shortcutPortalBinding) else {
            throw ShortcutRegistrationFailure.unsupported(.shortcutDirectBinding)
        }
        if takenBindings.contains(binding) { throw ShortcutRegistrationFailure.alreadyTaken }
        if activeBindings.values.contains(binding) { throw ShortcutRegistrationFailure.alreadyTaken }
        let handle = ShortcutHandle(rawValue: nextHandle)
        nextHandle += 1
        activeBindings[handle] = binding
        handlers[handle] = onActivate
        identifiers[handle] = identifier
        return handle
    }

    public func unregister(_ handle: ShortcutHandle) {
        activeBindings[handle] = nil
        handlers[handle] = nil
        identifiers[handle] = nil
    }

    /// Fires the shortcut registered under `identifier`.
    public func activate(identifier: String) {
        for (handle, name) in identifiers where name == identifier {
            handlers[handle]?()
        }
    }
}

// MARK: - ClipboardAccess

public final class FakeClipboardAccess: ClipboardAccess {
    public var platformCapabilities: PlatformCapabilitySet
    public private(set) var content: ClipboardContent?
    public var primarySelection: ClipboardContent?
    /// Every write, in order, with the concealment flag it was given.
    public private(set) var writes: [(ClipboardContent, concealed: Bool)] = []

    private var watchers: [ClipboardWatchToken: (ClipboardContent) -> Void] = [:]
    private var nextToken: UInt64 = 1

    public init(platformCapabilities: PlatformCapabilitySet = .all(.clipboardRead, .clipboardWrite,
                                                                  .clipboardWatch)) {
        self.platformCapabilities = platformCapabilities
    }

    public func read(flavors: [ClipboardFlavor]) -> ClipboardContent? {
        guard platformCapabilities.has(.clipboardRead), let content else { return nil }
        let wanted = content.flavors.filter { flavors.contains($0.key) }
        guard !wanted.isEmpty else { return nil }
        return ClipboardContent(flavors: wanted,
                                sourceApplicationID: content.sourceApplicationID)
    }

    public func write(_ content: ClipboardContent, concealed: Bool) throws {
        guard platformCapabilities.has(.clipboardWrite) else {
            throw PlatformError.unsupported(.clipboardWrite)
        }
        writes.append((content, concealed: concealed))
        set(content)
    }

    public func clear() {
        content = nil
    }

    public func watch(_ onChange: @escaping (ClipboardContent) -> Void) -> ClipboardWatchToken? {
        guard platformCapabilities.has(.clipboardWatch) else { return nil }
        let token = ClipboardWatchToken(rawValue: nextToken)
        nextToken += 1
        watchers[token] = onChange
        return token
    }

    public func stopWatching(_ token: ClipboardWatchToken) {
        watchers[token] = nil
    }

    public func readPrimarySelection() -> ClipboardContent? {
        platformCapabilities.has(.clipboardPrimarySelection) ? primarySelection : nil
    }

    /// Puts something on the clipboard as though another application had, and
    /// notifies the watchers.
    public func set(_ content: ClipboardContent) {
        self.content = content
        for watcher in watchers.values { watcher(content) }
    }
}

// MARK: - ScreenCapturer

public final class FakeScreenCapturer: ScreenCapturer {
    public var platformCapabilities: PlatformCapabilitySet
    public var displayList: [PlatformDisplay]
    /// Handed back by every `captureFrame` and every stream tick.
    public var frame: CapturedFrame
    public var restoreToken: String?
    /// What `pickTargetInteractively()` answers; `nil` means the app may draw
    /// its own picker.
    public var interactivePick: CaptureTarget?
    public private(set) var captured: [CaptureTarget] = []
    public private(set) var startedStreams: [(CaptureTarget, CaptureStreamOptions)] = []
    public private(set) var stoppedStreams: [CaptureSession] = []

    private var sinks: [CaptureSession: (CapturedFrame) -> Void] = [:]
    private var nextSession: UInt64 = 1

    public init(platformCapabilities: PlatformCapabilitySet = .all(.captureDisplay, .captureWindow,
                                                                  .captureArea, .captureStream),
                displays: [PlatformDisplay] = [.fake()],
                frame: CapturedFrame = .fake()) {
        self.platformCapabilities = platformCapabilities
        self.displayList = displays
        self.frame = frame
    }

    public func displays() throws -> [PlatformDisplay] { displayList }

    public func captureFrame(_ target: CaptureTarget) async throws -> CapturedFrame {
        try requireTarget(target)
        captured.append(target)
        return frame
    }

    public func startStream(_ target: CaptureTarget,
                            options: CaptureStreamOptions,
                            onFrame: @escaping (CapturedFrame) -> Void) async throws -> CaptureSession {
        guard platformCapabilities.has(.captureStream) else {
            throw PlatformError.unsupported(.captureStream)
        }
        try requireTarget(target)
        startedStreams.append((target, options))
        let session = CaptureSession(rawValue: nextSession)
        nextSession += 1
        sinks[session] = onFrame
        return session
    }

    public func stop(_ session: CaptureSession) {
        stoppedStreams.append(session)
        sinks[session] = nil
    }

    public func pickTargetInteractively() async throws -> CaptureTarget? {
        interactivePick
    }

    public func restore(with token: String) async throws {
        guard platformCapabilities.has(.captureRestoreToken) else {
            throw PlatformError.unsupported(.captureRestoreToken)
        }
        restoreToken = token
    }

    /// Pushes one frame into every live stream.
    public func tick() {
        for sink in sinks.values { sink(frame) }
    }

    private func requireTarget(_ target: CaptureTarget) throws {
        switch target {
        case .display:
            guard platformCapabilities.has(.captureDisplay) else {
                throw PlatformError.unsupported(.captureDisplay)
            }
        case .window:
            guard platformCapabilities.has(.captureWindow) else {
                throw PlatformError.unsupported(.captureWindow)
            }
        case .area:
            guard platformCapabilities.has(.captureArea) else {
                throw PlatformError.unsupported(.captureArea)
            }
        }
    }
}

public extension PlatformDisplay {
    static func fake(id: PlatformDisplayID = 1,
                     name: String = "fake-0",
                     bounds: CGRect = CGRect(x: 0, y: 0, width: 1920, height: 1080),
                     workArea: CGRect? = nil,
                     scale: CGFloat = 2,
                     isPrimary: Bool = true) -> PlatformDisplay {
        PlatformDisplay(id: id, name: name, bounds: bounds,
                        workArea: workArea ?? bounds, scale: scale, isPrimary: isPrimary)
    }
}

public extension CapturedFrame {
    static func fake(width: Int = 8, height: Int = 8, scale: CGFloat = 1) -> CapturedFrame {
        CapturedFrame(width: width, height: height, bytesPerRow: width * 4,
                      pixels: Data(count: width * height * 4), scale: scale,
                      capturedAt: 0)
    }
}

// MARK: - AudioGraph

public final class FakeAudioGraph: AudioGraph {
    public var platformCapabilities: PlatformCapabilitySet
    public var sinkList: [AudioSink]
    public var sourceList: [AudioSource]
    public var streamList: [AudioStream]
    public private(set) var calls: [Call] = []

    public enum Call: Equatable {
        case sinkVolume(String, Double)
        case sinkMuted(String, Bool)
        case sourceVolume(String, Double)
        case sourceMuted(String, Bool)
        case streamVolume(String, Double)
        case streamMuted(String, Bool)
        case defaultSink(String)
        case defaultSource(String)
        case route(String, String)
    }

    private var observers: [AudioObservationToken: (AudioGraphEvent) -> Void] = [:]
    private var nextToken: UInt64 = 1

    public init(platformCapabilities: PlatformCapabilitySet = .all(.audioSinkVolume,
                                                                   .audioSourceVolume,
                                                                   .audioStreamVolume,
                                                                   .audioEvents),
                sinks: [AudioSink] = [],
                sources: [AudioSource] = [],
                streams: [AudioStream] = []) {
        self.platformCapabilities = platformCapabilities
        self.sinkList = sinks
        self.sourceList = sources
        self.streamList = streams
    }

    public func sinks() -> [AudioSink] { sinkList }
    public func sources() -> [AudioSource] { sourceList }
    public func streams() -> [AudioStream] { streamList }

    public func setVolume(_ volume: Double, ofSink id: String) -> Bool {
        record(.sinkVolume(id, volume), needing: .audioSinkVolume)
    }

    public func setMuted(_ muted: Bool, ofSink id: String) -> Bool {
        record(.sinkMuted(id, muted), needing: .audioSinkVolume)
    }

    public func setVolume(_ volume: Double, ofSource id: String) -> Bool {
        record(.sourceVolume(id, volume), needing: .audioSourceVolume)
    }

    public func setMuted(_ muted: Bool, ofSource id: String) -> Bool {
        record(.sourceMuted(id, muted), needing: .audioSourceVolume)
    }

    public func setVolume(_ volume: Double, ofStream id: String) -> Bool {
        record(.streamVolume(id, volume), needing: .audioStreamVolume)
    }

    public func setMuted(_ muted: Bool, ofStream id: String) -> Bool {
        record(.streamMuted(id, muted), needing: .audioStreamVolume)
    }

    public func setDefaultSink(_ id: String) -> Bool {
        record(.defaultSink(id), needing: .audioDefaultDeviceSwitch)
    }

    public func setDefaultSource(_ id: String) -> Bool {
        record(.defaultSource(id), needing: .audioDefaultDeviceSwitch)
    }

    public func route(stream id: String, toSink sinkID: String) -> Bool {
        record(.route(id, sinkID), needing: .audioStreamRouting)
    }

    public func observe(_ onEvent: @escaping (AudioGraphEvent) -> Void) -> AudioObservationToken? {
        guard platformCapabilities.has(.audioEvents) else { return nil }
        let token = AudioObservationToken(rawValue: nextToken)
        nextToken += 1
        observers[token] = onEvent
        return token
    }

    public func stopObserving(_ token: AudioObservationToken) { observers[token] = nil }

    public func emit(_ event: AudioGraphEvent) {
        for observer in observers.values { observer(event) }
    }

    private func record(_ call: Call, needing capability: PlatformCapability) -> Bool {
        guard platformCapabilities.has(capability) else { return false }
        calls.append(call)
        return true
    }
}

// MARK: - SystemSensors

public final class FakeSystemSensors: SystemSensors {
    public var platformCapabilities: PlatformCapabilitySet
    public var cpuSample: CPUSample?
    public var memorySample: MemorySample?
    public var temperatureSamples: [TemperatureSample] = []
    public var fanSamples: [FanSample] = []
    public var batterySample: BatterySample?
    public var peripheralBatterySamples: [BatterySample] = []
    public var networkSamples: [NetworkSample] = []
    public var diskSamples: [DiskSample] = []
    public private(set) var thermalPressure: ThermalPressure = .nominal

    private var observers: [SensorObservationToken: (ThermalPressure) -> Void] = [:]
    private var nextToken: UInt64 = 1

    public init(platformCapabilities: PlatformCapabilitySet = .all(.sensorsCPU, .sensorsMemory,
                                                                   .sensorsThermalPressure)) {
        self.platformCapabilities = platformCapabilities
    }

    public func cpu() -> CPUSample? { platformCapabilities.has(.sensorsCPU) ? cpuSample : nil }
    public func memory() -> MemorySample? { platformCapabilities.has(.sensorsMemory) ? memorySample : nil }
    public func temperatures() -> [TemperatureSample] {
        platformCapabilities.has(.sensorsTemperature) ? temperatureSamples : []
    }
    public func fans() -> [FanSample] {
        platformCapabilities.has(.sensorsFanSpeed) ? fanSamples : []
    }
    public func battery() -> BatterySample? {
        platformCapabilities.has(.sensorsBattery) ? batterySample : nil
    }
    public func peripheralBatteries() -> [BatterySample] {
        platformCapabilities.has(.sensorsPeripheralBatteries) ? peripheralBatterySamples : []
    }
    public func network() -> [NetworkSample] {
        platformCapabilities.has(.sensorsNetwork) ? networkSamples : []
    }
    public func disks() -> [DiskSample] {
        platformCapabilities.has(.sensorsDiskActivity) ? diskSamples : []
    }

    public func observeThermalPressure(
        _ onChange: @escaping (ThermalPressure) -> Void) -> SensorObservationToken? {
        guard platformCapabilities.has(.sensorsThermalPressure) else { return nil }
        let token = SensorObservationToken(rawValue: nextToken)
        nextToken += 1
        observers[token] = onChange
        return token
    }

    public func stopObserving(_ token: SensorObservationToken) { observers[token] = nil }

    /// Drives the fan-control policy through a heat-up without hardware.
    public func setThermalPressure(_ pressure: ThermalPressure) {
        thermalPressure = pressure
        for observer in observers.values { observer(pressure) }
    }
}

// MARK: - PowerControl

public final class FakePowerControl: PowerControl {
    public var platformCapabilities: PlatformCapabilitySet
    public var brightness: [BrightnessTarget]
    /// When false, `setBrightness` records the attempt and reports failure —
    /// a DDC write that is acknowledged and does nothing.
    public var brightnessWritesLand = true
    public private(set) var activeAssertions: [PowerAssertion: SleepInhibition] = [:]
    public private(set) var releasedAssertions: [PowerAssertion] = []
    public private(set) var fanCommands: [(fanID: String, rpm: Int?)] = []
    public private(set) var brightnessWrites: [(PlatformDisplayID, Double)] = []
    public private(set) var didSleep = false
    public private(set) var didLock = false

    private var nextAssertion: UInt64 = 1

    public init(platformCapabilities: PlatformCapabilitySet = .all(.powerInhibitSystemSleep,
                                                                   .powerInhibitDisplaySleep),
                brightnessTargets: [BrightnessTarget] = []) {
        self.platformCapabilities = platformCapabilities
        self.brightness = brightnessTargets
    }

    public func inhibit(_ what: SleepInhibition, reason: String) throws -> PowerAssertion {
        if what.contains(.systemSleep), !platformCapabilities.has(.powerInhibitSystemSleep) {
            throw PlatformError.unsupported(.powerInhibitSystemSleep)
        }
        if what.contains(.lidClose), !platformCapabilities.has(.powerInhibitLidClose) {
            throw PlatformError.unsupported(.powerInhibitLidClose)
        }
        let assertion = PowerAssertion(rawValue: nextAssertion)
        nextAssertion += 1
        activeAssertions[assertion] = what
        return assertion
    }

    public func release(_ assertion: PowerAssertion) {
        activeAssertions[assertion] = nil
        releasedAssertions.append(assertion)
    }

    public func brightnessTargets() -> [BrightnessTarget] { brightness }

    public func setBrightness(_ level: Double, of display: PlatformDisplayID) -> Bool {
        brightnessWrites.append((display, level))
        guard brightnessWritesLand,
              let index = brightness.firstIndex(where: { $0.id == display }),
              brightness[index].isWritable
        else { return false }
        let old = brightness[index]
        brightness[index] = BrightnessTarget(id: old.id, name: old.name, level: level,
                                             isExternal: old.isExternal, isWritable: true)
        return true
    }

    public func setFanSpeed(rpm: Int, fanID: String) throws {
        guard platformCapabilities.has(.powerFanControl) else {
            throw PlatformError.denied("fan control needs the privileged helper")
        }
        fanCommands.append((fanID: fanID, rpm: rpm))
    }

    public func restoreAutomaticFanControl() throws {
        guard platformCapabilities.has(.powerFanControl) else {
            throw PlatformError.denied("fan control needs the privileged helper")
        }
        fanCommands.append((fanID: "*", rpm: nil))
    }

    public func sleepNow() throws {
        guard platformCapabilities.has(.powerSleepNow) else {
            throw PlatformError.unsupported(.powerSleepNow)
        }
        didSleep = true
    }

    public func lockSession() throws {
        guard platformCapabilities.has(.powerLockSession) else {
            throw PlatformError.unsupported(.powerLockSession)
        }
        didLock = true
    }
}

// MARK: - AppLauncher

public final class FakeAppLauncher: AppLauncher {
    public var platformCapabilities: PlatformCapabilitySet
    public var applications: [LaunchableApplication] = []
    /// Answers for `run`, keyed by executable path.
    public var outcomes: [String: ProcessOutcome] = [:]
    public private(set) var launched: [String] = []
    public private(set) var opened: [(URL, applicationID: String?)] = []
    public private(set) var revealed: [URL] = []
    public private(set) var ran: [(String, [String])] = []
    public private(set) var quitCalls: [(String, force: Bool)] = []

    public init(platformCapabilities: PlatformCapabilitySet = .all(.launchApplications,
                                                                   .launchEnumerateInstalled,
                                                                   .launchOpenURL,
                                                                   .launchRunCommand)) {
        self.platformCapabilities = platformCapabilities
    }

    public func installedApplications() -> [LaunchableApplication] {
        platformCapabilities.has(.launchEnumerateInstalled) ? applications : []
    }

    public func launch(_ id: String) async throws {
        guard platformCapabilities.has(.launchApplications) else {
            throw PlatformError.unsupported(.launchApplications)
        }
        launched.append(id)
    }

    public func open(_ url: URL) async throws {
        guard platformCapabilities.has(.launchOpenURL) else {
            throw PlatformError.unsupported(.launchOpenURL)
        }
        opened.append((url, applicationID: nil))
    }

    public func open(_ url: URL, with applicationID: String) async throws {
        guard platformCapabilities.has(.launchOpenURL) else {
            throw PlatformError.unsupported(.launchOpenURL)
        }
        opened.append((url, applicationID: applicationID))
    }

    public func reveal(_ url: URL) async throws {
        guard platformCapabilities.has(.launchRevealInFileManager) else {
            throw PlatformError.unsupported(.launchRevealInFileManager)
        }
        revealed.append(url)
    }

    public func run(_ executable: String,
                    arguments: [String],
                    timeout: TimeInterval) async throws -> ProcessOutcome {
        guard platformCapabilities.has(.launchRunCommand) else {
            throw PlatformError.unsupported(.launchRunCommand)
        }
        ran.append((executable, arguments))
        return outcomes[executable]
            ?? ProcessOutcome(exitCode: 0, standardOutput: "", standardError: "", timedOut: false)
    }

    public func quit(_ id: String, force: Bool) throws {
        guard platformCapabilities.has(.launchQuitApplication) else {
            throw PlatformError.unsupported(.launchQuitApplication)
        }
        quitCalls.append((id, force: force))
    }
}

// MARK: - PlatformNotifier

public final class FakeNotifier: PlatformNotifier {
    public var platformCapabilities: PlatformCapabilitySet
    public private(set) var authorizationStatus: NotificationAuthorization
    /// What `requestAuthorization()` will answer.
    public var authorizationAnswer: NotificationAuthorization = .authorized
    public private(set) var posted: [PlatformNotification] = []
    public private(set) var withdrawn: [String] = []
    public var onResponse: ((NotificationResponse) -> Void)?

    private var nextIdentifier = 1

    public init(platformCapabilities: PlatformCapabilitySet = .all(.notifyPost, .notifyActions,
                                                                   .notifyWithdraw),
                authorizationStatus: NotificationAuthorization = .authorized) {
        self.platformCapabilities = platformCapabilities
        self.authorizationStatus = authorizationStatus
    }

    public func requestAuthorization() async -> NotificationAuthorization {
        authorizationStatus = authorizationAnswer
        return authorizationStatus
    }

    @discardableResult public func post(_ notification: PlatformNotification) throws -> String {
        guard platformCapabilities.has(.notifyPost) else {
            throw PlatformError.unsupported(.notifyPost)
        }
        guard authorizationStatus == .authorized else {
            throw PlatformError.denied("notifications are not authorized")
        }
        posted.append(notification)
        let identifier = notification.identifier ?? "fake-\(nextIdentifier)"
        nextIdentifier += 1
        return identifier
    }

    public func withdraw(_ identifier: String) { withdrawn.append(identifier) }

    /// Answers the last notification as though the user had.
    public func respond(actionID: String?, identifier: String? = nil) {
        let last = posted.last
        onResponse?(NotificationResponse(identifier: identifier ?? last?.identifier,
                                         actionID: actionID,
                                         userInfo: last?.userInfo ?? [:]))
    }
}

// MARK: - TrashAndFiles

public final class FakeTrashAndFiles: TrashAndFiles {
    public var platformCapabilities: PlatformCapabilitySet
    public private(set) var trashed: [TrashedItem] = []
    public private(set) var didEmpty = false
    /// Directories `directory(for:)` answers with.
    public var directories: [StandardDirectory: URL] = [:]
    /// Paths a test declares to be on a different volume from everything else.
    public var otherVolumePrefixes: [String] = []

    public init(platformCapabilities: PlatformCapabilitySet = .all(.filesTrash,
                                                                   .filesRestoreFromTrash,
                                                                   .filesEnumerateTrash,
                                                                   .filesEmptyTrash)) {
        self.platformCapabilities = platformCapabilities
    }

    @discardableResult public func trash(_ url: URL) throws -> TrashedItem {
        guard platformCapabilities.has(.filesTrash) else {
            throw PlatformError.unsupported(.filesTrash)
        }
        let item = TrashedItem(
            trashedURL: URL(fileURLWithPath: "/fake/Trash/files/").appendingPathComponent(url.lastPathComponent),
            originalURL: url,
            deletedAt: Date(timeIntervalSinceReferenceDate: 0))
        trashed.append(item)
        return item
    }

    @discardableResult public func restore(_ item: TrashedItem) throws -> Bool {
        guard platformCapabilities.has(.filesRestoreFromTrash) else {
            throw PlatformError.unsupported(.filesRestoreFromTrash)
        }
        guard let index = trashed.firstIndex(of: item) else { return false }
        trashed.remove(at: index)
        return true
    }

    public func trashContents() throws -> [TrashedItem] {
        guard platformCapabilities.has(.filesEnumerateTrash) else {
            throw PlatformError.unsupported(.filesEnumerateTrash)
        }
        return trashed
    }

    public func emptyTrash() throws {
        guard platformCapabilities.has(.filesEmptyTrash) else {
            throw PlatformError.unsupported(.filesEmptyTrash)
        }
        trashed.removeAll()
        didEmpty = true
    }

    public func trashSize() throws -> UInt64 { UInt64(trashed.count) }

    public func directory(for purpose: StandardDirectory) -> URL {
        directories[purpose] ?? URL(fileURLWithPath: "/fake/\(purpose.rawValue)")
    }

    public func isSameVolume(_ a: URL, _ b: URL) -> Bool {
        func volume(_ url: URL) -> String {
            for prefix in otherVolumePrefixes where url.path.hasPrefix(prefix) { return prefix }
            return "/"
        }
        return volume(a) == volume(b)
    }
}

// MARK: - PackageManager

public final class FakePackageManager: PackageManager {
    public var platformCapabilities: PlatformCapabilitySet
    public var availableBackends: [PackageBackend]
    public var packages: [PackageBackend: [Package]] = [:]
    public var reclaimable: [PackageBackend: UInt64] = [:]
    public private(set) var refreshed: [PackageBackend] = []
    public private(set) var upgraded: [(String, PackageBackend)] = []
    public private(set) var uninstalled: [(String, PackageBackend)] = []
    public private(set) var cleanedUp: [PackageBackend] = []

    public init(platformCapabilities: PlatformCapabilitySet = .all(.packagesList, .packagesRefresh),
                availableBackends: [PackageBackend] = [.flatpak]) {
        self.platformCapabilities = platformCapabilities
        self.availableBackends = availableBackends
    }

    public func installed(from backend: PackageBackend) async throws -> [Package] {
        guard platformCapabilities.has(.packagesList) else {
            throw PlatformError.unsupported(.packagesList)
        }
        return packages[backend] ?? []
    }

    public func refresh(_ backend: PackageBackend) async throws {
        guard platformCapabilities.has(.packagesRefresh) else {
            throw PlatformError.unsupported(.packagesRefresh)
        }
        refreshed.append(backend)
    }

    public func outdated(from backend: PackageBackend) async throws -> [Package] {
        try await installed(from: backend).filter(\.isOutdated)
    }

    public func upgrade(_ id: String, from backend: PackageBackend) async throws {
        guard platformCapabilities.has(.packagesUpgrade) else {
            throw PlatformError.denied("upgrading \(backend.rawValue) packages needs root")
        }
        upgraded.append((id, backend))
    }

    public func uninstall(_ id: String, from backend: PackageBackend) async throws {
        guard platformCapabilities.has(.packagesUninstall) else {
            throw PlatformError.denied("uninstalling \(backend.rawValue) packages needs root")
        }
        uninstalled.append((id, backend))
    }

    public func reclaimableBytes(from backend: PackageBackend) async throws -> UInt64 {
        reclaimable[backend] ?? 0
    }

    public func cleanUp(_ backend: PackageBackend) async throws {
        guard platformCapabilities.has(.packagesCleanUp) else {
            throw PlatformError.unsupported(.packagesCleanUp)
        }
        cleanedUp.append(backend)
    }
}

// MARK: - SessionEvents

public final class FakeSessionEvents: SessionEvents {
    public var platformCapabilities: PlatformCapabilitySet
    public var frontmostApplicationID: String?
    public var isScreenLocked = false
    public var isSessionActive = true
    public var prefersDarkAppearance = false
    public var idleTime: TimeInterval?

    private var observers: [SessionObservationToken: (SessionEvent) -> Void] = [:]
    private var nextToken: UInt64 = 1

    public init(platformCapabilities: PlatformCapabilitySet = .all(.sessionSleepWakeEvents,
                                                                   .sessionLockEvents,
                                                                   .sessionFrontmostApplication,
                                                                   .sessionAppearanceEvents)) {
        self.platformCapabilities = platformCapabilities
    }

    public func observe(_ onEvent: @escaping (SessionEvent) -> Void) -> SessionObservationToken {
        let token = SessionObservationToken(rawValue: nextToken)
        nextToken += 1
        observers[token] = onEvent
        return token
    }

    public func stopObserving(_ token: SessionObservationToken) { observers[token] = nil }

    /// Delivers an event, and keeps the fake's own state consistent with it so
    /// a feature that reads back after a wake sees what it should.
    public func emit(_ event: SessionEvent) {
        switch event {
        case .screenLocked: isScreenLocked = true
        case .screenUnlocked: isScreenLocked = false
        case .sessionBecameActive: isSessionActive = true
        case .sessionBecameInactive: isSessionActive = false
        case .frontmostApplicationChanged(let appID): frontmostApplicationID = appID
        case .appearanceChanged(let isDark): prefersDarkAppearance = isDark
        default: break
        }
        for observer in observers.values { observer(event) }
    }
}

// MARK: - Capabilities

public final class FakeCapabilities: Capabilities {
    public var session: SessionDescription
    public private(set) var probes: [CapabilityProbe]
    public var onChange: (() -> Void)?
    public private(set) var refreshCount = 0

    public var platformCapabilities: PlatformCapabilitySet {
        PlatformCapabilitySet(
            supported: Set(probes.filter(\.isAvailable).map(\.id)),
            unavailableReasons: probes.filter { !$0.isAvailable }
                .reduce(into: [:]) { $0[$1.id] = $1.detail })
    }

    public init(session: SessionDescription = .fakeWayland, probes: [CapabilityProbe] = []) {
        self.session = session
        self.probes = probes
    }

    public func has(_ capability: PlatformCapability) -> Bool {
        probes.first { $0.id == capability }?.isAvailable ?? false
    }

    public func explanation(for capability: PlatformCapability) -> CapabilityProbe? {
        guard let probe = probes.first(where: { $0.id == capability }) else {
            return CapabilityProbe(id: capability, isAvailable: false,
                                   detail: "not probed", remedy: nil)
        }
        return probe.isAvailable ? nil : probe
    }

    public func refresh() {
        refreshCount += 1
        onChange?()
    }

    /// Declares a capability present or absent, and notifies, the way a real
    /// probe does after the user installs the GNOME extension or the helper.
    public func set(_ capability: PlatformCapability,
                    available: Bool,
                    detail: String? = nil,
                    remedy: String? = nil) {
        probes.removeAll { $0.id == capability }
        probes.append(CapabilityProbe(id: capability, isAvailable: available,
                                      detail: detail, remedy: remedy))
        onChange?()
    }
}

public extension SessionDescription {
    static let fakeWayland = SessionDescription(desktop: "fake", displayProtocol: "wayland",
                                                desktopVersion: nil, isSandboxed: false)
    static let fakeX11 = SessionDescription(desktop: "fake", displayProtocol: "x11",
                                            desktopVersion: nil, isSandboxed: false)
    static let fakeSandboxed = SessionDescription(desktop: "gnome", displayProtocol: "wayland",
                                                  desktopVersion: "48", isSandboxed: true)
}
