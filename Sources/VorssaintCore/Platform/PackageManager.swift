// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// One installed or installable package.
///
/// The field list is `HomebrewSupport`'s, because the Homebrew feature is what
/// this protocol generalizes: a name, the installed version, what is available,
/// and whether it is a leaf nobody depends on (which is what makes "outdated"
/// and "cleanup" lists useful rather than alarming).
public struct Package: Equatable, Identifiable {
    public let id: String
    public let name: String
    public let installedVersion: String?
    public let availableVersion: String?
    /// Bytes on disk, when the backend will say.
    public let installedSize: UInt64?
    /// Nothing else installed depends on it.
    public let isLeaf: Bool
    /// Installed only to satisfy another package's dependency.
    public let isAutomatic: Bool

    public init(id: String, name: String, installedVersion: String?,
                availableVersion: String?, installedSize: UInt64?,
                isLeaf: Bool, isAutomatic: Bool) {
        self.id = id
        self.name = name
        self.installedVersion = installedVersion
        self.availableVersion = availableVersion
        self.installedSize = installedSize
        self.isLeaf = isLeaf
        self.isAutomatic = isAutomatic
    }

    public var isOutdated: Bool {
        guard let installed = installedVersion, let available = availableVersion
        else { return false }
        return installed != available
    }
}

/// Which package system a `PackageManager` speaks.
///
/// Named rather than enumerated closed, because a Linux session can have
/// several at once (a dnf system with Flatpaks and a Snap or two) and the
/// feature lists them side by side rather than picking one.
public struct PackageBackend: RawRepresentable, Hashable, Codable {
    public let rawValue: String
    public init(rawValue: String) { self.rawValue = rawValue }
    public init(_ rawValue: String) { self.rawValue = rawValue }

    public static let homebrew = PackageBackend("homebrew")
    public static let apt = PackageBackend("apt")
    public static let dnf = PackageBackend("dnf")
    public static let pacman = PackageBackend("pacman")
    public static let zypper = PackageBackend("zypper")
    public static let flatpak = PackageBackend("flatpak")
    public static let snap = PackageBackend("snap")
}

/// Lists, updates and removes packages.
///
/// **Read-only by default.** Upgrading system packages needs root, and this
/// app is not a package manager; the honest behaviour — and what
/// `capabilities` reports — is that on most Linux sessions this protocol can
/// *see* what is outdated and hand the user off to their own updater, and can
/// only act for the per-user backends (Flatpak `--user`, Homebrew), never for
/// the system ones. `PLAN.md` § 7's rule applies: a missing tool degrades the
/// feature with an honest message instead of failing to launch.
public protocol PackageManager: PlatformService {
    /// The backends actually present on this machine, probed by running them,
    /// not by looking for a file.
    var availableBackends: [PackageBackend] { get }

    func installed(from backend: PackageBackend) async throws -> [Package]

    /// Refreshes the backend's index. Slow and network-bound; the caller
    /// shows progress.
    func refresh(_ backend: PackageBackend) async throws

    func outdated(from backend: PackageBackend) async throws -> [Package]

    /// Upgrades one package. Throws `PlatformError.denied` where the backend
    /// needs privileges this app does not have and will not ask for.
    func upgrade(_ id: String, from backend: PackageBackend) async throws

    func uninstall(_ id: String, from backend: PackageBackend) async throws

    /// Bytes a cleanup would reclaim: Homebrew's old kegs, apt's `.deb`
    /// archive, Flatpak's unused runtimes.
    func reclaimableBytes(from backend: PackageBackend) async throws -> UInt64

    func cleanUp(_ backend: PackageBackend) async throws
}

public extension PlatformCapability {
    static let packagesList = PlatformCapability("packages.list")
    static let packagesRefresh = PlatformCapability("packages.refresh")
    /// Can upgrade without asking for privileges the app does not hold.
    static let packagesUpgrade = PlatformCapability("packages.upgrade")
    static let packagesUninstall = PlatformCapability("packages.uninstall")
    static let packagesCleanUp = PlatformCapability("packages.cleanUp")
    /// Acting requires root, so the feature hands off to the session's own
    /// updater instead.
    static let packagesReadOnly = PlatformCapability("packages.readOnly")
}
