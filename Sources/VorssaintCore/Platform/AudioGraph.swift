// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// An output device: speakers, headphones, an HDMI sink.
public struct AudioSink: Equatable, Identifiable {
    public let id: String
    public let name: String
    /// The bus, for the icon the mixer draws: "bluetooth", "usb", "hdmi",
    /// "builtin". Free-form because PipeWire's `device.bus` and CoreAudio's
    /// transport type do not enumerate the same set.
    public let transport: String
    public let isDefault: Bool
    /// 0...1, and above 1 when the platform allows boost. `BoostLimiter`
    /// already assumes a scalar in this shape.
    public let volume: Double
    public let isMuted: Bool

    public init(id: String, name: String, transport: String, isDefault: Bool,
                volume: Double, isMuted: Bool) {
        self.id = id
        self.name = name
        self.transport = transport
        self.isDefault = isDefault
        self.volume = volume
        self.isMuted = isMuted
    }
}

/// An input device: a microphone.
public struct AudioSource: Equatable, Identifiable {
    public let id: String
    public let name: String
    public let transport: String
    public let isDefault: Bool
    public let volume: Double
    public let isMuted: Bool

    public init(id: String, name: String, transport: String, isDefault: Bool,
                volume: Double, isMuted: Bool) {
        self.id = id
        self.name = name
        self.transport = transport
        self.isDefault = isDefault
        self.volume = volume
        self.isMuted = isMuted
    }
}

/// One application's audio, which is what the per-app mixer exists for.
public struct AudioStream: Equatable, Identifiable {
    public let id: String
    public let applicationID: String
    public let applicationName: String
    public let sinkID: String
    public let volume: Double
    public let isMuted: Bool
    /// Whether anything is actually coming out right now, so the mixer can
    /// sort silent apps down as it does today.
    public let isActive: Bool

    public init(id: String, applicationID: String, applicationName: String,
                sinkID: String, volume: Double, isMuted: Bool, isActive: Bool) {
        self.id = id
        self.applicationID = applicationID
        self.applicationName = applicationName
        self.sinkID = sinkID
        self.volume = volume
        self.isMuted = isMuted
        self.isActive = isActive
    }
}

public enum AudioGraphEvent: Equatable {
    case sinksChanged
    case sourcesChanged
    case streamsChanged
    case defaultSinkChanged(String)
    case defaultSourceChanged(String)
    case volumeChanged(streamID: String, volume: Double, muted: Bool)
}

/// Devices, per-application streams and their volumes.
///
/// macOS is CoreAudio plus the app's existing tap (`AppVolumeMixer`,
/// `MixerRouting`); Linux is PipeWire, where per-application volume is a
/// first-class node property and needs no tap at all — the one place the
/// Linux backend is *simpler* than the Mac one.
public protocol AudioGraph: PlatformService {
    func sinks() -> [AudioSink]
    func sources() -> [AudioSource]
    func streams() -> [AudioStream]

    @discardableResult func setVolume(_ volume: Double, ofSink id: String) -> Bool
    @discardableResult func setMuted(_ muted: Bool, ofSink id: String) -> Bool
    @discardableResult func setVolume(_ volume: Double, ofSource id: String) -> Bool
    @discardableResult func setMuted(_ muted: Bool, ofSource id: String) -> Bool

    /// Per-application volume, the mixer's whole reason to exist.
    @discardableResult func setVolume(_ volume: Double, ofStream id: String) -> Bool
    @discardableResult func setMuted(_ muted: Bool, ofStream id: String) -> Bool

    @discardableResult func setDefaultSink(_ id: String) -> Bool
    @discardableResult func setDefaultSource(_ id: String) -> Bool

    /// Sends one application's audio to a different device.
    @discardableResult func route(stream id: String, toSink sinkID: String) -> Bool

    func observe(_ onEvent: @escaping (AudioGraphEvent) -> Void) -> AudioObservationToken?
    func stopObserving(_ token: AudioObservationToken)
}

public struct AudioObservationToken: Hashable {
    public let rawValue: UInt64
    public init(rawValue: UInt64) { self.rawValue = rawValue }
}

public extension PlatformCapability {
    static let audioSinkVolume = PlatformCapability("audio.sinkVolume")
    static let audioSourceVolume = PlatformCapability("audio.sourceVolume")
    /// Per-application volume. Present on PipeWire and on macOS only through
    /// the app's own tap.
    static let audioStreamVolume = PlatformCapability("audio.streamVolume")
    /// Can move one application's audio to another device.
    static let audioStreamRouting = PlatformCapability("audio.streamRouting")
    /// Volume above 100 %.
    static let audioBoost = PlatformCapability("audio.boost")
    static let audioDefaultDeviceSwitch = PlatformCapability("audio.defaultDeviceSwitch")
    static let audioEvents = PlatformCapability("audio.events")
}
