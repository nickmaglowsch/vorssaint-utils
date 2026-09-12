// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// A key combination, as the core stores it.
///
/// Not `GlobalShortcut`: that type is Carbon all the way down — 46 `kVK_`
/// constants and a `keyLabel` that goes through `TISCopyCurrentKeyboardInputSource`
/// and `UCKeyTranslate` (`docs/linux-port/CORE_MOVES.md` § 2) — and WP-11
/// proved 27 of its 30 non-UI users need that Carbon half. This is the
/// platform-free value the registrar takes: a physical key plus modifiers,
/// where the key is named rather than numbered so it survives the translation
/// from `kVK_ANSI_K` to an xkb keysym.
public struct ShortcutBinding: Hashable, Codable {
    /// Physical key, spelled the way `GlobalShortcut.storageValue` already
    /// spells it in preferences, so a settings backup crosses platforms
    /// (`WORK_PACKAGES.md` WP-14).
    public let keyToken: String
    public let modifiers: ShortcutModifiers

    public init(keyToken: String, modifiers: ShortcutModifiers) {
        self.keyToken = keyToken
        self.modifiers = modifiers
    }
}

/// The four modifiers the app has ever bound, in a platform-free spelling.
public struct ShortcutModifiers: OptionSet, Hashable, Codable {
    public let rawValue: Int
    public init(rawValue: Int) { self.rawValue = rawValue }

    public static let command = ShortcutModifiers(rawValue: 1 << 0)
    public static let option = ShortcutModifiers(rawValue: 1 << 1)
    public static let control = ShortcutModifiers(rawValue: 1 << 2)
    public static let shift = ShortcutModifiers(rawValue: 1 << 3)
}

/// Why a registration did not take.
public enum ShortcutRegistrationFailure: Error, Equatable {
    /// Another application already owns it. On macOS this is Carbon's
    /// `eventHotKeyExistsErr`; under the GlobalShortcuts portal it is the
    /// portal refusing the binding.
    case alreadyTaken
    /// The session grants shortcuts only through a system UI the user must
    /// confirm (the XDG GlobalShortcuts portal), and they have not yet.
    case awaitingUserBinding
    case unsupported(PlatformCapability)
    case backendFailure(String)
}

/// Registers system-wide key combinations and reports when they fire.
///
/// Consumed by `HotkeyManager` and every feature with a shortcut. On macOS
/// this is `RegisterEventHotKey`; on Linux it is the XDG `GlobalShortcuts`
/// portal where the session has one, otherwise a compositor-specific binding
/// (KWin/Hyprland/Sway IPC), otherwise the privileged evdev grab — three
/// backends whose difference is exactly what `capabilities` reports.
public protocol ShortcutRegistrar: PlatformService {
    /// Takes the binding. The handle is what `unregister` takes back; a
    /// registrar that binds lazily (the portal) still returns one immediately
    /// and calls `onActivate` only once the session honours it.
    func register(_ binding: ShortcutBinding,
                  identifier: String,
                  onActivate: @escaping () -> Void) -> Result<ShortcutHandle, ShortcutRegistrationFailure>

    func unregister(_ handle: ShortcutHandle)

    /// Every binding this process currently holds, so the settings screen can
    /// show what actually took rather than what was asked for.
    var activeBindings: [ShortcutHandle: ShortcutBinding] { get }

    /// True when the session decides bindings itself and the app may only
    /// propose them — the XDG portal's model. The shortcut recorder shows the
    /// system sheet instead of capturing keys when this is true.
    var bindingIsChosenBySession: Bool { get }
}

/// Opaque receipt for one registration.
public struct ShortcutHandle: Hashable {
    public let rawValue: UInt64
    public init(rawValue: UInt64) { self.rawValue = rawValue }
}

public extension PlatformCapability {
    /// Can bind a combination the app chooses, without user confirmation.
    static let shortcutDirectBinding = PlatformCapability("shortcut.directBinding")
    /// Can bind only through a session UI the user confirms (XDG portal).
    static let shortcutPortalBinding = PlatformCapability("shortcut.portalBinding")
    /// Can bind a combination that another application already holds.
    static let shortcutOverrideSystem = PlatformCapability("shortcut.overrideSystem")
    /// Reports the keys as they are labelled on the user's layout.
    static let shortcutLayoutAwareLabels = PlatformCapability("shortcut.layoutAwareLabels")
}
