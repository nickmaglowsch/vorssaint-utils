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

/// How to read the four bytes of a captured pixel.
///
/// Open — a `String` raw value rather than a closed enum — for the reason
/// `PlatformCapability` is (`PLATFORM.md` § 0): the name is read back from a C
/// capture backend that negotiates its format with the compositor at run time,
/// and a format nobody anticipated must arrive as data rather than as a trap.
///
/// The distinction that makes this type exist is `bgrx` versus
/// `bgraPremultiplied`. WP-B1 found it and wrote it down as open item 1 of
/// `linux/platform/capture/README.md`: the xdg-desktop-portal ScreenCast path
/// commonly negotiates `SPA_VIDEO_FORMAT_BGRx`, whose fourth byte is
/// **undefined padding, not opacity**. A wrapper that relabels those bytes as
/// BGRA hands a consumer an alpha channel of whatever the compositor's scratch
/// memory held — frequently zero, which is a screenshot that saves and
/// previews as fully transparent. The engine reports the format it actually
/// negotiated in `vs_capture_frame.format`, so the wrapper carries it through
/// and every consumer reads `hasAlpha` before it trusts byte 3.
///
/// The raw values are `vs_capture_pixel_format_name()`'s strings, exactly:
/// the C side owns the names (`linux/platform/README.md`, rule 1 of "How
/// WP-12 mirrors it"), so a name renamed there is renamed here in the same PR
/// and a `rawValue` round-trips through the C boundary unchanged.
public struct CapturedPixelFormat: RawRepresentable, Hashable, Codable, Sendable {
    public let rawValue: String
    public init(rawValue: String) { self.rawValue = rawValue }

    /// `VS_CAPTURE_PIXEL_UNKNOWN`. A backend that could not name its format.
    /// Treated as opaque and 4 bytes per pixel by everything below, because
    /// that is what every format the engine can actually produce is.
    public static let unknown = CapturedPixelFormat(rawValue: "unknown")
    /// `VS_CAPTURE_PIXEL_BGRX`: four bytes B, G, R, **padding**. What
    /// `xdg-desktop-portal-wlr` negotiates on the SHM path. The image is
    /// opaque; byte 3 means nothing and must be overwritten or ignored, never
    /// read as alpha.
    public static let bgrx = CapturedPixelFormat(rawValue: "BGRx")
    /// `VS_CAPTURE_PIXEL_BGRA`: four bytes B, G, R, A with the colour already
    /// multiplied by the alpha. ScreenCaptureKit's output.
    public static let bgraPremultiplied = CapturedPixelFormat(rawValue: "BGRA")
    /// `VS_CAPTURE_PIXEL_RGBX`: R, G, B, padding.
    public static let rgbx = CapturedPixelFormat(rawValue: "RGBx")
    /// `VS_CAPTURE_PIXEL_RGBA`: R, G, B, A premultiplied.
    public static let rgbaPremultiplied = CapturedPixelFormat(rawValue: "RGBA")

    /// Whether the fourth byte carries opacity. `false` for the padded
    /// formats, and `false` for a format this build does not know — an unknown
    /// format is read as opaque, because an image that is wrong in its alpha
    /// is recoverable and a fully transparent one is not.
    public var hasAlpha: Bool {
        self == .bgraPremultiplied || self == .rgbaPremultiplied
    }

    /// Whether byte order is B, G, R rather than R, G, B.
    public var isBGROrdered: Bool {
        self == .bgraPremultiplied || self == .bgrx
    }

    /// `vs_capture_pixel_bytes()`. Four for every format the engine produces;
    /// four for an unknown one too, since `bytesPerRow` is what actually walks
    /// the buffer and a zero here would divide by nothing.
    public var bytesPerPixel: Int { 4 }
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
    public let pixels: Data
    /// What `pixels` actually is. There is no default: the whole point of the
    /// field is that a backend must say, and a default would be the
    /// assumption it exists to remove.
    public let pixelFormat: CapturedPixelFormat
    /// Pixels per point of the source display, for a Retina/HiDPI-correct save.
    public let scale: CGFloat
    /// Seconds since the reference date, for the recorder's timeline.
    public let capturedAt: TimeInterval

    public init(width: Int, height: Int, bytesPerRow: Int, pixels: Data,
                pixelFormat: CapturedPixelFormat,
                scale: CGFloat, capturedAt: TimeInterval) {
        self.width = width
        self.height = height
        self.bytesPerRow = bytesPerRow
        self.pixels = pixels
        self.pixelFormat = pixelFormat
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
