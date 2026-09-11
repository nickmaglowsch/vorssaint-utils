# WP-00 spike: Vorssaint core on Linux Swift

Throwaway SwiftPM package that puts the candidate Vorssaint sources in front
of a real Linux Swift compiler. It is not part of the product build and the
top-level `Package.swift` does not reference it.

```
./sync.sh                                   # vendor the sources listed in files-*.txt
swift build --target VorssaintCoreSpike     # the 93 files the census calls Foundation-only
swift build --target VorssaintCoreWide      # all 120 census files, errors expected
swift build -c release --static-swift-stdlib --product hello-spike
swift test
```

- `files-clean.txt` — the 93 census-clean files; this target is the go/no-go.
- `files-all.txt` — all 120 candidate files; this target *is* the error census.
- `Sources/*Shim` — stand-ins for Apple frameworks, named so that vendored
  sources keep their unmodified `import`. Every symbol in them is a
  `Platform` protocol candidate for WP-12.
- `Sources/VorssaintCombine` — the `Combine`/OpenCombine switch WP-13 makes real.
- `sync.sh` performs exactly one source rewrite, `import Carbon.HIToolbox` →
  `import Carbon`, because a SwiftPM target name cannot contain a dot.

Findings live in `docs/linux-port/spikes/00-swift-core.md`.
