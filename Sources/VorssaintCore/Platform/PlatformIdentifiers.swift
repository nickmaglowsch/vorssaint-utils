// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// A window, as a number the platform layer hands out and takes back.
///
/// `UInt32` because that is what every backend this port targets already uses:
/// `CGWindowID` on macOS is `public typealias CGWindowID = UInt32`, an X11
/// `Window` is an XID (32 bits on the wire), and the Wayland
/// `ext_foreign_toplevel_list_v1` handles the Linux backends index are also
/// mapped into a 32-bit table. So on macOS `PlatformWindowID` and `CGWindowID`
/// are the *same type*, and code that passes one where the other is expected —
/// every existing caller of `RecorderSupport` — compiles unchanged.
///
/// The identifier is opaque: it is meaningful only to the `WindowSystem` that
/// issued it, and only until that window closes.
public typealias PlatformWindowID = UInt32

/// A display, as a number the platform layer hands out.
///
/// `UInt32` for the same reason: `CGDirectDisplayID` is `UInt32`, and the
/// Linux backends assign their own small ids over `wl_output` /
/// XRandR outputs.
public typealias PlatformDisplayID = UInt32
