// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// An application the session knows how to start.
///
/// The macOS side builds these from `NSWorkspace` and the bundle; the Linux
/// side from `.desktop` entries under `XDG_DATA_DIRS/applications`. The fields
/// are what `InstalledApps`, `CommandBarCatalog` and the switcher's app
/// grouping already read.
public struct LaunchableApplication: Equatable, Identifiable {
    /// Bundle identifier on macOS, `.desktop` file id on Linux (`firefox.desktop`).
    /// The same string `WindowInfo.appID` carries wherever the platform agrees
    /// with itself — and where it does not (a Wayland `app_id` that does not
    /// match any desktop file), `PLATFORM.md` records the join as best-effort.
    public let id: String
    public let name: String
    /// Where it lives, for "reveal in file manager" and for the uninstaller.
    public let location: URL?
    /// Icon name to look up in the session's theme on Linux; `nil` on macOS,
    /// where the icon comes from the bundle.
    public let iconName: String?
    /// `NoDisplay=true` desktop entries and background-only macOS apps: real,
    /// launchable, but not offered in a list of applications.
    public let isHidden: Bool

    public init(id: String, name: String, location: URL?, iconName: String?, isHidden: Bool) {
        self.id = id
        self.name = name
        self.location = location
        self.iconName = iconName
        self.isHidden = isHidden
    }
}

/// Starts applications, opens files and URLs, and runs commands.
///
/// macOS is `NSWorkspace` plus `Process`; Linux is the `OpenURI` portal where
/// one exists (so it works inside a Flatpak sandbox), `gio open`/`xdg-open`
/// otherwise, and a plain `Process` for commands. The portal path is why
/// opening is `async throws` rather than a synchronous `Bool`.
public protocol AppLauncher: PlatformService {
    /// Everything installed, for the command bar's catalogue and the
    /// uninstaller's list.
    func installedApplications() -> [LaunchableApplication]

    /// Launches, or activates it if it is already running — the behaviour
    /// `CommandBarCatalog` relies on.
    func launch(_ id: String) async throws

    /// Opens a file with whatever the session considers its default handler.
    func open(_ url: URL) async throws

    /// Opens a file with one named application.
    func open(_ url: URL, with applicationID: String) async throws

    /// Shows the file in the session's file manager, selected.
    func reveal(_ url: URL) async throws

    /// Runs a command and waits for it, with a deadline. The command-bar
    /// script runner and the Homebrew and package features use this; it is
    /// never a shell string, always an argv, so nothing is word-split.
    func run(_ executable: String,
             arguments: [String],
             timeout: TimeInterval) async throws -> ProcessOutcome

    /// Asks the session to terminate an application politely, then forcibly.
    /// `autoQuit` and `KillProcess` consume this.
    func quit(_ id: String, force: Bool) throws
}

public struct ProcessOutcome: Equatable {
    public let exitCode: Int32
    public let standardOutput: String
    public let standardError: String
    /// `true` when the deadline fired and the process was killed, so a caller
    /// does not read an empty stdout as an empty answer.
    public let timedOut: Bool

    public init(exitCode: Int32, standardOutput: String, standardError: String, timedOut: Bool) {
        self.exitCode = exitCode
        self.standardOutput = standardOutput
        self.standardError = standardError
        self.timedOut = timedOut
    }
}

public extension PlatformCapability {
    static let launchApplications = PlatformCapability("launch.applications")
    static let launchEnumerateInstalled = PlatformCapability("launch.enumerateInstalled")
    static let launchOpenURL = PlatformCapability("launch.openURL")
    /// Can select a file in the session's file manager. Not every Linux file
    /// manager implements `org.freedesktop.FileManager1.ShowItems`.
    static let launchRevealInFileManager = PlatformCapability("launch.revealInFileManager")
    /// Can run an arbitrary command. `false` inside a Flatpak sandbox without
    /// `--talk-name=org.freedesktop.Flatpak`.
    static let launchRunCommand = PlatformCapability("launch.runCommand")
    static let launchQuitApplication = PlatformCapability("launch.quitApplication")
}
