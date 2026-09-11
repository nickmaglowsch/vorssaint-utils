#!/bin/bash
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 1
{
  cat <<'EOF'
Sources/Vorssaint/Core/FeatureCatalog.swift
Sources/Vorssaint/Core/Localization.swift
Sources/Vorssaint/Core/Defaults.swift
Sources/Vorssaint/Core/GlobalShortcut.swift
Sources/Vorssaint/Core/URLCleaning.swift
Sources/Vorssaint/Services/Switcher/SwitcherSupport.swift
Sources/Vorssaint/Services/CommandBar/CommandBarSupport.swift
Sources/Vorssaint/Services/CommandBar/CommandBarMath.swift
Sources/Vorssaint/Services/QuickTools/ScreenshotSupport.swift
Sources/Vorssaint/Services/Recorder/RecorderTimeline.swift
Sources/Vorssaint/Services/Clipboard/ClipboardHistorySupport.swift
Sources/Vorssaint/Core/FeaturePresets.swift
Sources/Vorssaint/Core/SettingsBackupSupport.swift
Sources/Vorssaint/App/FeatureRuntime.swift
EOF
  ls Sources/Vorssaint/Services/*/*Support.swift
  ls Sources/Vorssaint/Core/*Strings.swift
  ls Sources/Vorssaint/Core/Localizations/*.swift
} | sort -u
