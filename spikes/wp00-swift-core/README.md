# WP-00 spike: Vorssaint core on Linux Swift

Throwaway SwiftPM package that puts the candidate Vorssaint sources in front
of a real Linux Swift compiler. It is not part of the product build and the
top-level `Package.swift` does not reference it.

```
./sync.sh                                   # vendor the sources listed in files-*.txt
swift build --target VorssaintCoreSpike     # the 103-file core candidate set
swift build --target VorssaintCoreWide      # all 120 census files, errors expected
swift build -c release --static-swift-stdlib --product hello-spike
swift build --target VorssaintServices     # the whole non-UI layer
swift test                                  # against VorssaintCoreMinimal
```

- `files-core.txt` — the core candidate set: the 101 census-portable files plus `Defaults.swift` and `GlobalShortcut.swift`, which the rest need to resolve.
- `files-all.txt` — all 120 candidate files; this target *is* the error census.
- `files-services.txt` — every file under `Sources/Vorssaint` outside `UI/` that does not import SwiftUI/AppKit/Cocoa (208). Unlike the census sets this one is close to dependency-closed, so its log shows Linux gaps rather than missing in-repo declarations.
- `files-minimal.txt` — `URLCleaning.swift` and `CommandBarMath.swift`: two files that declare everything they use, so this target is truly closed and the unit tests run against it.
- `Sources/*Shim` — stand-ins for Apple frameworks, named so that vendored
  sources keep their unmodified `import`. Every symbol in them is a
  `Platform` protocol candidate for WP-12.
- `Sources/VorssaintCombine` — the `Combine`/OpenCombine switch WP-13 makes real.
- `sync.sh` performs exactly one source rewrite, `import Carbon.HIToolbox` →
  `import Carbon`, because a SwiftPM target name cannot contain a dot.

Findings live in `docs/linux-port/spikes/00-swift-core.md`.
