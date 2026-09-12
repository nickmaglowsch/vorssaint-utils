// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

// `L10nBridgeService` observes `L10n.shared.$language`, so it needs Combine on
// both sides (`COMBINE.md` § 1).
#if canImport(Darwin)
import Combine
#else
import OpenCombine
#endif
import Foundation

/// The whole UI catalog, for the language in force.
///
/// `strings` is the flat `Strings` struct as a dictionary: 900-odd
/// compiler-checked fields, keyed by their own property names. That is the
/// point of the shape — QML writes `l10n.state.strings.menuQuit` and gets the
/// same string the SwiftUI view gets from `l10n.s.menuQuit`, out of the same
/// catalogs, so there is no second translation file for the Linux shell and
/// the playbook's "every user-facing string goes through `Strings`" survives
/// the port. A missing key in QML is a missing key in both, which is the only
/// way a thirteen-language catalog stays honest without a compiler on the QML
/// side.
///
/// It is also the bridge's largest snapshot by an order of magnitude — about
/// 30 KB — and it changes only when someone picks another language. See
/// `BRIDGE.md` § "Diffing" for why that is an argument for the fast path
/// rather than for a merge patch.
public struct L10nSnapshot: Codable, Equatable {
    public struct Language: Codable, Equatable {
        public let id: String
        /// The language's own name in its own script, the way the picker lists
        /// it.
        public let displayName: String

        public init(id: String, displayName: String) {
            self.id = id
            self.displayName = displayName
        }
    }

    /// The `AppLanguage` raw value in force, e.g. `"pt-BR"`.
    public let language: String
    /// Every language the interface can use, in catalog order.
    public let languages: [Language]
    /// Whether this language puts a distinct form between one and many. Only
    /// Russian, of the thirteen, and QML's plural helpers need to know.
    public let usesFewCountForm: Bool
    /// Every field of `Strings`, keyed by its property name.
    public let strings: [String: String]

    public init(language: String, languages: [Language],
                usesFewCountForm: Bool, strings: [String: String]) {
        self.language = language
        self.languages = languages
        self.usesFewCountForm = usesFewCountForm
        self.strings = strings
    }
}

/// What the language picker calls.
public enum L10nCommand: Equatable {
    case setLanguage(String)
}

public final class L10nBridgeService: BridgeService {
    public static let bridgeID = "l10n"

    private let bridge: CoreBridge
    private var cancellable: AnyCancellable?

    public init(bridge: CoreBridge = .shared) {
        self.bridge = bridge
    }

    /// Start following `L10n.shared`. Separate from `init` because a bridge
    /// service is constructed before it is registered, and publishing to a
    /// bridge that has not been told about it yet would be a silent no-op.
    public func startObserving() {
        cancellable = L10n.shared.$language
            .dropFirst()          // the current value is already in the snapshot
            .sink { [weak self] _ in
                guard let self else { return }
                // `@Published` fires in `willSet`, so the object still holds
                // the old language at this point; publishing now would encode
                // the catalog we are leaving. One hop settles it, and the hop
                // is onto the main queue because the core is main-thread-only.
                DispatchQueue.main.async { self.publishToBridge(self.bridge) }
            }
    }

    public func bridgeSnapshot() -> L10nSnapshot {
        let l10n = L10n.shared
        return L10nSnapshot(
            language: l10n.language.rawValue,
            languages: AppLanguage.allCases.map {
                L10nSnapshot.Language(id: $0.rawValue, displayName: $0.displayName)
            },
            usesFewCountForm: l10n.language.usesFewCountForm,
            strings: Self.catalog(l10n.s))
    }

    public func apply(_ command: L10nCommand) throws {
        switch command {
        case .setLanguage(let id):
            guard let language = AppLanguage(rawValue: id) else {
                throw BridgeError.rejected(service: Self.bridgeID,
                                           reason: "no language \"\(id)\"")
            }
            guard L10n.shared.language != language else { return }
            L10n.shared.language = language
            publishToBridge(bridge)
        }
    }

    /// `Strings` flattened to `[property name: value]`.
    ///
    /// `Mirror` rather than a generated table: `Strings` gains fields in nearly
    /// every feature PR, and a generated table would be one more thing to
    /// forget. The cost is that a field which is not a `String` would be
    /// dropped silently, so `catalogCoverage` states the count both ways and a
    /// test holds them equal.
    // Internal, not public: `Strings` is internal, and a public signature over
    // an internal type does not compile. The shell never needs this — it
    // reads the `strings` dictionary out of the snapshot.
    static func catalog(_ strings: Strings) -> [String: String] {
        var out: [String: String] = [:]
        for child in Mirror(reflecting: strings).children {
            guard let label = child.label, let value = child.value as? String else { continue }
            out[label] = value
        }
        return out
    }

    /// `(fields, strings)` — every stored property of `Strings`, and how many
    /// of them `catalog` could carry. They must be equal.
    static func catalogCoverage(_ strings: Strings) -> (fields: Int, strings: Int) {
        let children = Mirror(reflecting: strings).children
        return (children.count, children.filter { $0.value is String }.count)
    }
}

// MARK: - Wire format

extension L10nCommand: Codable {
    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: BridgeCommandKey.self)
        let key = try BridgeCommandCoding.singleKey(container, in: decoder)
        switch key.stringValue {
        case "setLanguage": self = .setLanguage(try container.decode(String.self, forKey: key))
        default: throw BridgeCommandCoding.unknownCase(key, decoder)
        }
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: BridgeCommandKey.self)
        switch self {
        case .setLanguage(let id):
            try container.encode(id, forKey: BridgeCommandKey("setLanguage"))
        }
    }
}
