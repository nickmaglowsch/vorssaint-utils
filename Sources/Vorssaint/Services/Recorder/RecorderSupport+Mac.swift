// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import CoreGraphics
import Foundation

// The one corner of RecorderSupport.swift that CoreGraphics owns rather
// than Foundation: `CGAffineTransform` and `CGRect.applying(_:)`, neither of
// which swift-corelibs-foundation has. Split out by WP-12 so the other 740
// lines could move into Sources/VorssaintCore. The text is verbatim; only
// the `extension` wrapper around it is new.
//
// Every caller is a macOS AV pipeline file (RecorderComposer, RecorderExporter,
// RecorderEditorController) reading an AVAssetTrack's preferredTransform, so
// nothing in the core needs it. The Linux recorder gets its rotation from the
// capture backend instead (docs/linux-port/PLATFORM.md, ScreenCapturer).
extension RecorderSupport {
    struct VideoGeometry: Equatable {
        let size: CGSize
        let transform: CGAffineTransform
    }

    /// Normalizes a movie track's orientation into a display-sized rectangle
    /// whose origin is zero. Screen recordings are already identity; imported
    /// portrait and rotated movies commonly are not.
    static func videoGeometry(naturalSize: CGSize,
                              preferredTransform: CGAffineTransform) -> VideoGeometry {
        guard naturalSize.width.isFinite, naturalSize.height.isFinite,
              naturalSize.width > 0, naturalSize.height > 0
        else { return VideoGeometry(size: .zero, transform: .identity) }
        let naturalRect = CGRect(origin: .zero, size: naturalSize)
        let displayed = naturalRect.applying(preferredTransform)
        guard displayed.width.isFinite, displayed.height.isFinite,
              displayed.width != 0, displayed.height != 0
        else { return VideoGeometry(size: .zero, transform: .identity) }
        var normalized = preferredTransform
        normalized.tx -= displayed.minX
        normalized.ty -= displayed.minY
        return VideoGeometry(
            size: evenSize(CGSize(width: abs(displayed.width), height: abs(displayed.height))),
            transform: normalized)
    }
}
