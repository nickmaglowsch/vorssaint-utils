// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

#if canImport(CoreGraphics)
import CoreGraphics
#endif
import Foundation

/// A monitor, as the layout, capture and brightness features need it.
///
/// A value type rather than a protocol: it is what `ScreenCapturer.displays()`
/// returns and what `PowerControl.brightnessTargets()` is keyed by.
///
/// `linux/platform/include/vorssaint_platform.h` has no display section yet —
/// `vs_window_info` carries only an output *name*. So `name` is the join
/// between the two for now (a Wayland `wl_output` name, an XRandR output name,
/// a `CGDisplay`'s localized name), and `id` is assigned by whichever backend
/// enumerates them. WP-C1 adding a `vs_display_system` section is what makes
/// the id authoritative; until then `name` is the identity that crosses the
/// window/display boundary, and `PLATFORM.md` records that as an open edge.
public struct PlatformDisplay: Equatable, Identifiable {
    public let id: PlatformDisplayID
    /// The output name `WindowInfo.output` carries.
    public let name: String
    /// Whole display, in the same top-left-origin layout coordinates as
    /// `WindowInfo.frame`.
    public let bounds: CGRect
    /// Minus panels, docks and reserved struts — what a maximized window gets.
    /// Equal to `bounds` where the platform will not say.
    public let workArea: CGRect
    /// Pixels per point.
    public let scale: CGFloat
    public let isPrimary: Bool

    public init(id: PlatformDisplayID, name: String, bounds: CGRect,
                workArea: CGRect, scale: CGFloat, isPrimary: Bool) {
        self.id = id
        self.name = name
        self.bounds = bounds
        self.workArea = workArea
        self.scale = scale
        self.isPrimary = isPrimary
    }
}
