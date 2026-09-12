// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

#if canImport(CoreGraphics)
import CoreGraphics
#endif
import Foundation

/// What to capture. Mirrors `RecorderSupport.Region`, which already resolves a
/// pick into a display plus an optional window plus a pixel rectangle.
public enum CaptureTarget: Equatable {
    case display(PlatformDisplayID)
    case window(PlatformWindowID)
    case area(display: PlatformDisplayID, pixelRect: CGRect)
}

/// One still frame, as bytes plus the shape to read them with.
///
/// Not an image object: the core must not name `CGImage` or a GdkPixbuf. The
/// screenshot and recorder features hand this straight to the platform's
/// encoder, and the OCR/barcode features hand it to theirs.
public struct CapturedFrame: Equatable {
    public let width: Int
    public let height: Int
    public let bytesPerRow: Int
    /// Always premultiplied BGRA on both platforms — what ScreenCaptureKit
    /// hands back and what PipeWire's `SPA_VIDEO_FORMAT_BGRA` buffers carry,
    /// so no consumer has to branch.
    public let pixels: Data
    /// Pixels per point of the source display, for a Retina/HiDPI-correct save.
    public let scale: CGFloat
    /// Seconds since the reference date, for the recorder's timeline.
    public let capturedAt: TimeInterval

    public init(width: Int, height: Int, bytesPerRow: Int, pixels: Data,
                scale: CGFloat, capturedAt: TimeInterval) {
        self.width = width
        self.height = height
        self.bytesPerRow = bytesPerRow
        self.pixels = pixels
        self.scale = scale
        self.capturedAt = capturedAt
    }
}

public struct CaptureStreamOptions: Equatable {
    public let framesPerSecond: Int
    public let includesCursor: Bool
    /// Windows the stream must leave out — the recorder excludes its own
    /// overlay and indicator. On macOS this is ScreenCaptureKit's exclusion
    /// list, which `RecorderSupport.exceptedOwnWindowIDs` already computes.
    public let excludedWindows: Set<PlatformWindowID>
    public let capturesAudio: Bool

    public init(framesPerSecond: Int, includesCursor: Bool,
                excludedWindows: Set<PlatformWindowID>, capturesAudio: Bool) {
        self.framesPerSecond = framesPerSecond
        self.includesCursor = includesCursor
        self.excludedWindows = excludedWindows
        self.capturesAudio = capturesAudio
    }
}

/// Takes screenshots and drives capture streams.
///
/// macOS is ScreenCaptureKit plus the screen-recording permission. Linux is
/// the ScreenCast portal (PipeWire) where a portal exists, `grim`-style
/// `ext_image_copy_capture_manager_v1` on wlroots, and neither under X11
/// without XSHM — so `capabilities` is what tells the screenshot feature
/// whether it may offer a window pick at all.
///
/// Portals hand back a session the user consents to once and which can be
/// restored later; `restoreToken` is that, and it is the difference between
/// "one permission prompt ever" and "one per screenshot".
public protocol ScreenCapturer: PlatformService {
    /// Every monitor, with its bounds, work area and scale.
    ///
    /// Lives here because capture is what must enumerate displays to offer a
    /// target. `WindowLayoutService` reads work areas through this too, until
    /// `linux/platform/include/vorssaint_platform.h` grows a display section
    /// of its own (see `PlatformDisplay`).
    func displays() throws -> [PlatformDisplay]

    /// One frame, now.
    func captureFrame(_ target: CaptureTarget) async throws -> CapturedFrame

    /// Starts a stream. Frames arrive on the callback until the returned
    /// session is stopped.
    func startStream(_ target: CaptureTarget,
                     options: CaptureStreamOptions,
                     onFrame: @escaping (CapturedFrame) -> Void) async throws -> CaptureSession

    func stop(_ session: CaptureSession)

    /// Lets the person pick what to capture using the session's own UI, when
    /// the session insists on picking for us (every Wayland portal does).
    /// Returns `nil` when the app may draw its own picker instead.
    func pickTargetInteractively() async throws -> CaptureTarget?

    /// Opaque consent the portal gives back, to be stored and replayed so the
    /// next capture does not prompt. `nil` on a platform with no such concept.
    var restoreToken: String? { get }

    func restore(with token: String) async throws
}

public struct CaptureSession: Hashable {
    public let rawValue: UInt64
    public init(rawValue: UInt64) { self.rawValue = rawValue }
}

public extension PlatformCapability {
    static let captureDisplay = PlatformCapability("capture.display")
    static let captureWindow = PlatformCapability("capture.window")
    static let captureArea = PlatformCapability("capture.area")
    static let captureStream = PlatformCapability("capture.stream")
    static let captureCursor = PlatformCapability("capture.cursor")
    /// Can leave named windows out of the stream. Without it the recorder's
    /// own overlay would appear in the recording.
    static let captureWindowExclusion = PlatformCapability("capture.windowExclusion")
    static let captureSystemAudio = PlatformCapability("capture.systemAudio")
    /// The app may draw its own region picker; otherwise the session's picker
    /// is mandatory.
    static let captureOwnPicker = PlatformCapability("capture.ownPicker")
    /// Consent survives between captures.
    static let captureRestoreToken = PlatformCapability("capture.restoreToken")
}
