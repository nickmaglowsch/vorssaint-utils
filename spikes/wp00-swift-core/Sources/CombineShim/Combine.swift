// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// WP-00 spike shim so that vendored sources keep their unmodified
// `import Combine`. On Linux this module resolves to OpenCombine through
// `VorssaintCombine`; on macOS the system framework wins and this target is
// never built.

@_exported import VorssaintCombine
