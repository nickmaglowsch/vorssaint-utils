// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// WP-00 spike: the one-line Combine abstraction WP-13 will productise.
// Apple platforms get the system Combine; Linux gets OpenCombine, whose
// `ObservableObject`, `@Published`, `AnyCancellable`, `PassthroughSubject`
// and `CurrentValueSubject` are source-compatible.
//
// A file that only needs those symbols keeps its `import Combine` on macOS
// and gets the same names from here on Linux.

#if canImport(Combine)
@_exported import Combine
#else
@_exported import OpenCombine
@_exported import OpenCombineFoundation
@_exported import OpenCombineDispatch
#endif
