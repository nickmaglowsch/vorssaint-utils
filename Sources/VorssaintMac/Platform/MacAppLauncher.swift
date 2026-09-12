// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

#if os(macOS)
import AppKit
import Foundation

/// The macOS `AppLauncher`, over `NSWorkspace` and `Process`.
///
/// `installedApplications()` delegates to `InstalledApps.installedApplications`,
/// which is the scan the command bar and the uninstaller already use, so the
/// two never disagree about what is installed.
struct MacAppLauncher: AppLauncher {
    var platformCapabilities: PlatformCapabilitySet {
        .all(.launchApplications, .launchEnumerateInstalled, .launchOpenURL,
             .launchRevealInFileManager, .launchRunCommand, .launchQuitApplication)
    }

    func installedApplications() -> [LaunchableApplication] {
        InstalledApps.installedApplications().map { app in
            LaunchableApplication(id: app.identity ?? app.url.path,
                                  name: app.name,
                                  location: app.url,
                                  iconName: nil,
                                  isHidden: app.isSystem)
        }
    }

    func launch(_ id: String) async throws {
        guard let url = InstalledApps.url(for: id) else { throw PlatformError.notFound }
        let configuration = NSWorkspace.OpenConfiguration()
        configuration.activates = true
        _ = try await NSWorkspace.shared.openApplication(at: url, configuration: configuration)
    }

    func open(_ url: URL) async throws {
        guard NSWorkspace.shared.open(url) else {
            throw PlatformError.backendFailure("nothing opened \(url.lastPathComponent)")
        }
    }

    func open(_ url: URL, with applicationID: String) async throws {
        guard let application = InstalledApps.url(for: applicationID) else {
            throw PlatformError.notFound
        }
        let configuration = NSWorkspace.OpenConfiguration()
        configuration.activates = true
        _ = try await NSWorkspace.shared.open([url], withApplicationAt: application,
                                              configuration: configuration)
    }

    func reveal(_ url: URL) async throws {
        NSWorkspace.shared.activateFileViewerSelecting([url])
    }

    func run(_ executable: String,
             arguments: [String],
             timeout: TimeInterval) async throws -> ProcessOutcome {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        let out = Pipe()
        let err = Pipe()
        process.standardOutput = out
        process.standardError = err
        do {
            try process.run()
        } catch {
            throw PlatformError.backendFailure("\(executable): \(error.localizedDescription)")
        }

        // Read before waiting: a child that fills a 64 KiB pipe buffer blocks
        // forever if the parent waits first, which is the classic way a
        // "timeout" turns into a hang that the timeout never sees.
        let outData = out.fileHandleForReading.readDataToEndOfFile()
        let errData = err.fileHandleForReading.readDataToEndOfFile()

        var timedOut = false
        let deadline = Date().addingTimeInterval(timeout)
        while process.isRunning && Date() < deadline {
            usleep(10_000)
        }
        if process.isRunning {
            timedOut = true
            process.terminate()
        }
        process.waitUntilExit()

        return ProcessOutcome(exitCode: process.terminationStatus,
                              standardOutput: String(decoding: outData, as: UTF8.self),
                              standardError: String(decoding: errData, as: UTF8.self),
                              timedOut: timedOut)
    }

    func quit(_ id: String, force: Bool) throws {
        let running = NSRunningApplication.runningApplications(withBundleIdentifier: id)
        guard !running.isEmpty else { throw PlatformError.notFound }
        for application in running {
            _ = force ? application.forceTerminate() : application.terminate()
        }
    }
}
#endif
