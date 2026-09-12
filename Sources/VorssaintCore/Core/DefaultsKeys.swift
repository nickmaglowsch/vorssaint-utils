// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// Split out of Sources/Vorssaint/Core/Defaults.swift by WP-11. The key table
// is pure Foundation and is the hub the whole core depends on; the `Defaults`
// enum left behind validates stored values against thirty service `Support`
// types and reads `Bundle.main`, so only this half is portable
// (docs/linux-port/CORE_MOVES.md).

import Foundation

/// Every UserDefaults key used by the app, in one place.
enum DefaultsKey {
    static let language = "appLanguage"                   // AppLanguage.rawValue
    static let appearance = "appAppearance"               // AppAppearance.rawValue
    static let liquidGlassEnabled = "liquidGlassEnabled"  // Liquid Glass visual styling on macOS 26+
    static let clamshellPreferred = "clamshellPreferred"  // apply closed-lid mode to every session
    static let onboardingStep = "onboardingStep"          // resume point if onboarding is interrupted
    static let featuresOnboardingVersion = "featuresOnboardingVersion" // last feature-tour marker handled
    static let lastUpdateIntroVersion = "lastUpdateIntroVersion"
    static let supportUpdateIntroVersion = "supportUpdateIntroVersion"
    static let updateHighlightsSeenVersion = "updateHighlightsSeenVersion"
    static let updateShowcaseIntroVersion = "updateShowcaseIntroVersion"
    static let updateShowcaseMediaOverride = "updateShowcaseMediaOverride"
    static let defaultDuration = "defaultDurationMinutes" // 0 = indefinite
    static let batteryLimit = "batteryLimitPercent"       // 0 = never
    static let keepAwakeAutoStart = "keepAwakeAutoStart"  // start Keep Awake when the app launches
    static let keepAwakeRightClickToggle = "keepAwakeRightClickToggle"
    static let keepAwakeAllowDisplaySleep = "keepAwakeAllowDisplaySleep"
    static let keepAwakeExternalDisplay = "keepAwakeExternalDisplay"
    static let keepAwakeConnectedToPower = "keepAwakeConnectedToPower"
    static let keepAwakeRunningApps = "keepAwakeRunningApps"
    static let keepAwakeRunningAppBundleIDs = "keepAwakeRunningAppBundleIDs"
    static let keepAwakePauseWhenLocked = "keepAwakePauseWhenLocked"
    static let keepAwakeMouseJiggleEnabled = "keepAwakeMouseJiggleEnabled"
    static let keepAwakeMouseJiggleInterval = "keepAwakeMouseJiggleIntervalMinutes"
    static let hotkeyEnabled = "hotkeyEnabled"
    static let launchAtLoginWanted = "launchAtLoginWanted"  // the user's choice; the system record can be lost
    static let keepAwakeShortcut = "keepAwakeShortcut"    // GlobalShortcut storage value
    static let keepAwakeIconTint = "keepAwakeIconTint"    // KeepAwakeIconTint.rawValue
    static let keepAwakeActiveIcon = "keepAwakeActiveIcon" // KeepAwakeActiveIcon.rawValue
    static let showCountdown = "showCountdownInMenuBar"
    static let statusItemPlacementGeneration = "statusItemPlacementGeneration"
    static let hasOnboarded = "hasOnboarded"
    static let sleepDisabledFlag = "vorssDisabledSleep"   // internal guard for pmset disablesleep
    static let scrollInverterEnabled = "scrollInverterEnabled"
    static let scrollInverterHorizontalEnabled = "scrollInverterHorizontalEnabled"
    static let focusFollowsMouseEnabled = "focusFollowsMouseEnabled"
    static let focusFollowsMouseDelay = "focusFollowsMouseDelayMilliseconds"
    static let focusFollowsMouseExceptions = "focusFollowsMouseExceptions"
    static let smoothScrollEnabled = "smoothScrollEnabled"
    static let smoothScrollStep = "smoothScrollStep"      // pixels per wheel tick
    static let mouseAccelerationDisabled = "mouseAccelerationDisabled" // sets HIDMouseAcceleration to -1 for mice
    static let smoothScrollResponse = "smoothScrollResponse" // 0...100, higher follows the wheel sooner
    static let mouseNavigationEnabled = "mouseNavigationEnabled" // side buttons trigger Back and Forward
    static let mouseButtonShortcutsEnabled = "mouseButtonShortcutsEnabled" // extra buttons press a key combination (issue #282)
    static let mouseButtonShortcuts = "mouseButtonShortcuts" // [button number: GlobalShortcut storage value]
    static let mouseSpacesGestureEnabled = "mouseSpacesGestureEnabled" // hold a button and drag to switch Spaces (issue #1012)
    static let mouseSpacesGestureButton = "mouseSpacesGestureButton"   // button number, 0 while none is chosen
    static let mouseSpacesGestureFollowsDrag = "mouseSpacesGestureFollowsDrag" // the Space moves with the hand, the way natural scrolling does
    static let mouseClickDebounceEnabled = "mouseClickDebounceEnabled"
    static let mouseClickDebounceWindowMs = "mouseClickDebounceWindowMs"
    static let superKeyEnabled = "superKeyEnabled"        // chosen key holds the configured modifiers (issue #330)
    static let superKeySource = "superKeySource"           // SuperKeySource raw value
    static let superKeyModifiers = "superKeyModifiers"     // GlobalShortcutModifiers storage tokens
    static let superKeySoloAction = "superKeySoloAction"  // SuperKeySoloAction raw value
    // Machine state, never exported: whether the keyboard mapping is in place
    // and which source to take back after a crash.
    static let superKeyMappingApplied = "superKeyMappingApplied"
    static let superKeyMappedSource = "superKeyMappedSource"
    // One list of bundle ids per mouse feature: apps it leaves alone (issue #358).
    static let smoothScrollExceptions = "smoothScrollExceptions"
    static let scrollInverterExceptions = "scrollInverterExceptions"
    static let mouseNavigationExceptions = "mouseNavigationExceptions"
    static let mouseButtonExceptions = "mouseButtonExceptions"
    static let middleClickExceptions = "middleClickExceptions"
    static let superKeyExceptions = "superKeyExceptions"
    static let switcherEnabled = "switcherEnabled"
    static let switcherTakeOverSystemShortcuts = "switcherTakeOverSystemShortcuts"
    // Machine state, never exported: the system shortcuts this process owns,
    // so a launch after a crash can restore them. The switcher-only key is
    // what builds before the take-over was shared wrote; it is read once,
    // folded into the shared one, and then retired.
    static let switcherNativeHotkeysSuppressed = "switcherNativeHotkeysSuppressed"
    static let systemShortcutsSuppressed = "systemShortcutsSuppressed"
    static let switcherShortcut = "switcherShortcut"      // GlobalShortcut storage value
    static let switcherWindowShortcut = "switcherWindowShortcut" // GlobalShortcut storage value
    static let switcherIconRowMode = "switcherIconRowMode"
    static let switcherSimpleMode = "switcherSimpleMode"  // app-only row without window captures
    static let switcherMergeTabs = "switcherMergeTabs"     // show one switcher entry per app (collapse all of an app's windows)
    static let switcherShowWindowlessFinder = "switcherShowWindowlessFinder" // replaced by switcherWindowlessApps, kept so the migration can read it
    static let switcherWindowlessApps = "switcherWindowlessApps" // SwitcherWindowlessApps raw value
    static let switcherMinimizedPlacement = "switcherMinimizedPlacement"
    static let switcherShowFullscreenWindows = "switcherShowFullscreenWindows"
    static let switcherAppRules = "switcherAppRules" // [bundle id: SwitcherAppRule raw value]
    static let switcherCurrentSpaceOnly = "switcherCurrentSpaceOnly" // list only windows on the desktop the user is in (issue #337)
    static let switcherSearchPinEnabled = "switcherSearchPinEnabled" // S pins the search field open, off by default so existing users typing S as a search letter see no change
    static let switcherShowShortcutHints = "switcherShowShortcutHints" // show the shortcut bar under the large-icon switcher
    static let switcherAppearanceDelay = "switcherAppearanceDelay" // milliseconds the shortcut must be held before the panel appears (SwitcherSupport.appearanceDelayMillisecondsRange)
    static let switcherScreenPlacement = "switcherScreenPlacement" // SwitcherScreenPlacement raw value: which display the panel opens on
    static let switcherCurrentDisplayOnly = "switcherCurrentDisplayOnly" // list only windows on the display under the pointer (issue #1391)
    static let minimalWindowPreviews = "minimalWindowPreviews"
    static let dockPreviewEnabled = "dockPreviewEnabled"
    static let dockPreviewBackgroundOpacity = "dockPreviewBackgroundOpacity" // how solid the preview panel's material is drawn (DockPreviewSupport.backgroundOpacityRange)
    static let dockPreviewOpenDelay = "dockPreviewOpenDelay" // milliseconds the cursor must rest on a Dock icon before its panel opens (DockPreviewSupport.openDelayMillisecondsRange)
    static let dockPreviewQuitAppOnClose = "dockPreviewQuitAppOnClose" // the preview card's close button quits the owning app instead of closing one window
    static let dockClickMinimize = "dockClickMinimize"    // click the active app's Dock icon to minimize its windows
    static let dockClickHide = "dockClickHide"            // click the active app's Dock icon to hide the app
    static let dockClickCycleWindows = "dockClickCycleWindows" // click the active app's Dock icon to cycle through its windows
    static let middleClickEnabled = "middleClickEnabled"  // three-finger PHYSICAL click on the trackpad acts as a middle click
    static let middleClickTapFingers = "middleClickTapFingers"  // 0 = off (default); 3 or 4 = a light tap with that many fingers also middle-clicks (issue #161)
    static let previewSize = "previewSize"                // app switcher + dock preview thumbnail size
    static let autoCheckUpdates = "autoCheckUpdates"
    static let includeBetaUpdates = "includeBetaUpdates"
    static let releaseNotesOnUpdate = "releaseNotesOnUpdate" // show What's New after an update
    static let appVolumes = "appVolumes"                  // [bundle id: 0...2]
    static let appOutputDevices = "appOutputDevices"      // [bundle id: audio device UID]
    static let mixerShowFinder = "mixerShowFinder"
    static let mixerHideInactiveApps = "mixerHideInactiveApps"
    static let mixerHiddenApps = "mixerHiddenApps"        // [persistence id: display name] kept out of the mixer list (issue #300)
    static let mixerLowerVolumeOnHeadphonesDisconnect = "mixerLowerVolumeOnHeadphonesDisconnect"
    static let mixerHeadphonesDisconnectVolumePercent = "mixerHeadphonesDisconnectVolumePercent"
    static let preciseVolumeRollerEnabled = "preciseVolumeRollerEnabled"
    static let soundOutputSwitcherEnabled = "soundOutputSwitcherEnabled"
    static let soundOutputSwitcherShortcut = "soundOutputSwitcherShortcut"
    static let soundOutputSwitcherDeviceUIDs = "soundOutputSwitcherDeviceUIDs"
    static let preferredInputDevice = "preferredInputDevice" // audio input device UID
    static let finderCutPasteEnabled = "finderCutPasteEnabled"
    static let finderCutPasteShowHUD = "finderCutPasteShowHUD"
    static let finderRenameEnabled = "finderRenameEnabled"
    static let finderRenameShortcut = "finderRenameShortcut"
    static let diskImageInstallerTrashesDownload = "diskImageInstallerTrashesDownload"
    static let diskImageInstallerRevealsApp = "diskImageInstallerRevealsApp"
    static let finderPasteImageAsFile = "finderPasteImageAsFile"
    static let autoQuitEnabled = "autoQuitEnabled"
    static let autoQuitExceptions = "autoQuitExceptions"  // [bundle id] kept running
    // Quit/close protection: each shortcut owns its full configuration and app list.
    static let quitProtectionQuitEnabled = "quitProtectionQuitEnabled"
    static let quitProtectionQuitMode = "quitProtectionQuitMode"
    static let quitProtectionQuitHoldDurationMs = "quitProtectionQuitHoldDurationMs"
    static let quitProtectionQuitDoubleIntervalMs = "quitProtectionQuitDoubleIntervalMs"
    static let quitProtectionQuitExtraModifier = "quitProtectionQuitExtraModifier"
    static let quitProtectionQuitScope = "quitProtectionQuitScope"
    static let quitProtectionQuitExceptions = "quitProtectionQuitExceptions"
    static let quitProtectionQuitShowFeedback = "quitProtectionQuitShowFeedback"
    static let quitProtectionCloseEnabled = "quitProtectionCloseEnabled"
    static let quitProtectionCloseMode = "quitProtectionCloseMode"
    static let quitProtectionCloseHoldDurationMs = "quitProtectionCloseHoldDurationMs"
    static let quitProtectionCloseDoubleIntervalMs = "quitProtectionCloseDoubleIntervalMs"
    static let quitProtectionCloseExtraModifier = "quitProtectionCloseExtraModifier"
    static let quitProtectionCloseScope = "quitProtectionCloseScope"
    static let quitProtectionCloseExceptions = "quitProtectionCloseExceptions"
    static let quitProtectionCloseShowFeedback = "quitProtectionCloseShowFeedback"
    static let shelfEnabled = "shelfEnabled"
    static let shelfShortcutEnabled = "shelfShortcutEnabled"
    static let shelfShortcut = "shelfShortcut"            // GlobalShortcut storage value
    static let shelfShakeToOpen = "shelfShakeToOpen"
    static let shelfDropZoneEnabled = "shelfDropZoneEnabled"
    static let shelfEdgeDragEnabled = "shelfEdgeDragEnabled"
    static let shelfCloseAfterDrop = "shelfCloseAfterDrop"
    static let shelfRemoveAfterDrop = "shelfRemoveAfterDrop"
    static let shelfClearOnClose = "shelfClearOnClose"
    static let shelfAutomaticExclusions = "shelfAutomaticExclusions" // [bundle id] blocks automatic opening only
    static let extraBrightnessEnabled = "extraBrightnessEnabled"
    static let extraBrightnessLevel = "extraBrightnessLevel"   // Int percent 0-100
    static let brightnessControlEnabled = "brightnessControlEnabled" // sliders for every display
    static let brightnessKeysEnabled = "brightnessKeysEnabled" // brightness keys act on the display under the pointer
    static let brightnessOSDEnabled = "brightnessOSDEnabled" // brightness adjustment overlay
    static let displayBrightnessShortcutsEnabled = "displayBrightnessShortcutsEnabled"
    static let displayBrightnessDecreaseShortcut = "displayBrightnessDecreaseShortcut"
    static let displayBrightnessIncreaseShortcut = "displayBrightnessIncreaseShortcut"
    static let keyboardBrightnessShortcutsEnabled = "keyboardBrightnessShortcutsEnabled"
    static let keyboardBrightnessDecreaseShortcut = "keyboardBrightnessDecreaseShortcut"
    static let keyboardBrightnessIncreaseShortcut = "keyboardBrightnessIncreaseShortcut"
    // Per-monitor connection paths that accept brightness writes but never
    // answer reads. Kept local so wake handling does not repeatedly probe a
    // sensitive display path.
    static let brightnessDDCWriteOnlyPaths = "brightnessDDCWriteOnlyPaths"
    // Displays this app switched off, so a run that ends without putting them
    // back can be repaired on the next start instead of needing a replug.
    static let displaysSwitchedOff = "displaysSwitchedOff"
    // Set while a start is under way and cleared once the app has run
    // healthily for a while, or when it is quit properly. Found still set at
    // the next start, it means the previous one died on the way up.
    static let startupDidNotFinish = "startupDidNotFinish"
    static let bluetoothSleepEnabled = "bluetoothSleepEnabled"
    static let bluetoothSleepRestoreOnWake = "bluetoothSleepRestoreOnWake"
    // Set only while Vorssaint owes a Bluetooth restore, so a Mac shut down
    // while asleep still gets it back on the next launch.
    static let bluetoothSleepRestorePending = "bluetoothSleepRestorePending"
    static let musicBlockEnabled = "musicBlockEnabled"
    static let musicBlockReplacementPath = "musicBlockReplacementPath"  // app bundle path ("" = none)
    static let cleanerScheduleFrequency = "cleanerScheduleFrequency"    // off | daily | weekly
    static let cleanerScheduleHour = "cleanerScheduleHour"
    static let cleanerScheduleMinute = "cleanerScheduleMinute"
    static let cleanerScheduleWeekday = "cleanerScheduleWeekday"        // 1 Sunday ... 7 Saturday
    static let cleanerScheduleNotify = "cleanerScheduleNotify"
    static let cleanerLastAutoRun = "cleanerLastAutoRun"                // Double, epoch seconds
    static let cleanerLastAutoFreed = "cleanerLastAutoFreed"            // Int bytes
    // Confirmed WhatsApp downloads in the top level of ~/Downloads.
    static let whatsAppDownloadsEnabled = "whatsAppDownloadsEnabled"
    static let whatsAppDownloadsAutomaticEnabled = "whatsAppDownloadsAutomaticEnabled"
    static let whatsAppDownloadsCategories = "whatsAppDownloadsCategories" // comma-joined category ids
    static let whatsAppDownloadsRetentionDays = "whatsAppDownloadsRetentionDays"
    static let whatsAppDownloadsNotify = "whatsAppDownloadsNotify"
    static let whatsAppDownloadsIncludeExisting = "whatsAppDownloadsIncludeExisting"
    static let whatsAppDownloadsAutomaticStartDate = "whatsAppDownloadsAutomaticStartDate"
    static let whatsAppDownloadsLastAutoRun = "whatsAppDownloadsLastAutoRun"
    static let whatsAppDownloadsLastCleanup = "whatsAppDownloadsLastCleanup"
    static let whatsAppDownloadsLastCleanupCount = "whatsAppDownloadsLastCleanupCount"
    static let whatsAppDownloadsLastCleanupBytes = "whatsAppDownloadsLastCleanupBytes"
    static let whatsAppDownloadsLastCleanupFailed = "whatsAppDownloadsLastCleanupFailed"
    static let whatsAppDownloadsLastCleanupAutomatic = "whatsAppDownloadsLastCleanupAutomatic"
    static let whatsAppDownloadsExclusions = "whatsAppDownloadsExclusions" // device:inode ids
    static let whatsAppDownloadsAccessConfirmed = "whatsAppDownloadsAccessConfirmed"
    // Experimental organizer for confirmed WhatsApp downloads.
    static let whatsAppOrganizerEnabled = "whatsAppOrganizerEnabled"
    static let whatsAppOrganizerDestinationPath = "whatsAppOrganizerDestinationPath"
    static let whatsAppOrganizerDelayMinutes = "whatsAppOrganizerDelayMinutes"
    static let whatsAppOrganizerCategories = "whatsAppOrganizerCategories"
    static let whatsAppOrganizerLayout = "whatsAppOrganizerLayout"
    static let whatsAppOrganizerDuplicateAction = "whatsAppOrganizerDuplicateAction"
    static let whatsAppOrganizerRecords = "whatsAppOrganizerRecords"
    static let whatsAppOrganizerUndoTransaction = "whatsAppOrganizerUndoTransaction"
    static let whatsAppOrganizerLastRun = "whatsAppOrganizerLastRun"
    static let whatsAppOrganizerLastMoved = "whatsAppOrganizerLastMoved"
    static let whatsAppOrganizerLastDuplicates = "whatsAppOrganizerLastDuplicates"
    static let whatsAppOrganizerLastFailed = "whatsAppOrganizerLastFailed"
    static let settingsWindowWidth = "settingsWindowWidth"     // last user-chosen content size (0 = unset)
    static let settingsWindowHeight = "settingsWindowHeight"
    static let shelfItems = "shelfItems"                  // Data: [ShelfPersistedItem] JSON
    static let urlCleanerEnabled = "urlCleanerEnabled"
    static let urlCleanerCustomParameters = "urlCleanerCustomParameters"
    static let urlCleanerSiteParameters = "urlCleanerSiteParameters"       // host|name pairs added to one site
    static let urlCleanerDisabledParameters = "urlCleanerDisabledParameters" // built-in host|name pairs switched off
    static let windowMaximizeEnabled = "windowMaximizeEnabled"
    static let keyboardDebounceEnabled = "keyboardDebounceEnabled"
    static let keyboardDebounceWindowMs = "keyboardDebounceWindowMs"
    static let keyboardDebounceKeyWindows = "keyboardDebounceKeyWindows" // comma-separated keyCode:ms
    static let panelUtilityCleaning = "panelUtilityCleaning"
    static let cleaningModeKeepScreenVisible = "cleaningModeKeepScreenVisible"
    static let panelUtilityURLCleaner = "panelUtilityURLCleaner"
    static let panelUtilityUninstaller = "panelUtilityUninstaller"
    static let killProcessCommandBarEnabled = "killProcessCommandBarEnabled"
    static let killProcessGroupRelated = "killProcessGroupRelated"
    static let killProcessSortBy = "killProcessSortBy" // cpu | memory | name | pid
    static let killProcessSortAscending = "killProcessSortAscending"
    static let panelUtilityCleaner = "panelUtilityCleaner"
    static let panelUtilityHomebrew = "panelUtilityHomebrew"
    static let panelUtilityAppUpdates = "panelUtilityAppUpdates"
    static let appUpdatesCheckFrequency = "appUpdatesCheckFrequency"  // off | daily | weekly
    static let appUpdatesIncludeHomebrewApps = "appUpdatesIncludeHomebrewApps"
    static let appUpdatesIncludeAppStore = "appUpdatesIncludeAppStore"
    static let appUpdatesIncludeOnlineCatalog = "appUpdatesIncludeOnlineCatalog"
    static let appUpdatesNotify = "appUpdatesNotify"
    static let appUpdatesLastCheck = "appUpdatesLastCheck"            // Double, epoch seconds
    static let appUpdatesLastCount = "appUpdatesLastCount"
    // Findings already announced once, so a pending update nobody installs
    // does not speak up again after every relaunch.
    static let appUpdatesNotifiedIDs = "appUpdatesNotifiedIDs"
    static let panelUtilityMedia = "panelUtilityMedia"
    static let panelUtilityClipboard = "panelUtilityClipboard"
    static let panelUtilityWindowLayout = "panelUtilityWindowLayout"
    static let panelControlMouseScroll = "panelControlMouseScroll"
    static let panelControlFocusFollowsMouse = "panelControlFocusFollowsMouse"
    static let panelControlMouseNavigation = "panelControlMouseNavigation"
    static let panelControlSwitcher = "panelControlSwitcher"
    static let panelControlDockPreview = "panelControlDockPreview"
    static let panelControlCutPaste = "panelControlCutPaste"
    static let panelControlAutoQuit = "panelControlAutoQuit"
    static let panelControlShelf = "panelControlShelf"
    static let panelControlWindowMaximize = "panelControlWindowMaximize"
    static let panelControlKeyDebounce = "panelControlKeyDebounce"
    static let panelControlDockClick = "panelControlDockClick"
    static let panelControlDockClickHide = "panelControlDockClickHide"
    static let panelControlDockClickCycle = "panelControlDockClickCycle"
    static let panelControlMiddleClick = "panelControlMiddleClick"
    static let panelControlTextSnippets = "panelControlTextSnippets"
    static let panelControlSuperKey = "panelControlSuperKey"
    static let panelControlRadialMenu = "panelControlRadialMenu"
    static let panelControlMouseButtonShortcuts = "panelControlMouseButtonShortcuts"
    static let panelControlMouseAcceleration = "panelControlMouseAcceleration"
    static let panelControlMouseClickDebounce = "panelControlMouseClickDebounce"
    // Quick-control categories start collapsed and remember being opened.
    static let panelControlWindowsExpanded = "panelControlWindowsExpanded"
    static let panelControlInputExpanded = "panelControlInputExpanded"
    static let panelControlFilesExpanded = "panelControlFilesExpanded"
    // Show/hide whole panel sections that have no monitorShow* key of their own.
    static let panelShowKeepAwake = "panelShowKeepAwake"
    static let panelShowBrightness = "panelShowBrightness"
    static let panelShowUtilities = "panelShowUtilities"
    static let panelShowControls = "panelShowControls"
    static let panelShowToggles = "panelShowToggles"
    // Quick toggles tab: per-action visibility (the order lives in panelToggleOrder).
    static let panelToggleDarkMode = "panelToggleDarkMode"
    static let panelToggleKeyboardLight = "panelToggleKeyboardLight"
    // Keep the existing storage key so moving the row preserves its visibility choice.
    static let panelToggleMicMute = "panelUtilityMicMute"
    static let panelToggleEmptyTrash = "panelToggleEmptyTrash"
    static let panelToggleEjectDisks = "panelToggleEjectDisks"
    static let panelToggleHiddenFiles = "panelToggleHiddenFiles"
    static let panelToggleDesktopIcons = "panelToggleDesktopIcons"
    static let panelToggleLockScreen = "panelToggleLockScreen"
    static let panelToggleDisplayOff = "panelToggleDisplayOff"
    static let panelToggleScreenSaver = "panelToggleScreenSaver"

    // System monitor — live metrics shown next to the menu bar icon (opt-in).
    static let menuBarCPU = "menuBarCPU"
    static let menuBarGPU = "menuBarGPU"
    static let menuBarMemory = "menuBarMemory"
    static let menuBarCPUTemperature = "menuBarCPUTemperature"
    static let menuBarGPUTemperature = "menuBarGPUTemperature"
    static let menuBarBatteryTemperature = "menuBarBatteryTemperature"
    static let menuBarTemperature = "menuBarTemperature" // legacy Developer key for the old generic temperature metric
    static let menuBarNetwork = "menuBarNetwork"
    static let menuBarDiskUsage = "menuBarDiskUsage"
    static let menuBarDiskActivity = "menuBarDiskActivity"
    static let menuBarBattery = "menuBarBattery"
    static let menuBarBatteryTime = "menuBarBatteryTime"
    static let menuBarPeripheralBattery = "menuBarPeripheralBattery"
    static let menuBarPower = "menuBarPower"
    static let menuBarFanSpeed = "menuBarFanSpeed"
    static let menuBarPreset = "menuBarPreset"           // dense
    static let menuBarMetricSpacing = "menuBarMetricSpacing" // standard | compact
    static let menuBarMetricAppearance = "menuBarMetricAppearance" // values | bars
    static let menuBarUsageBarNormalColor = "menuBarUsageBarNormalColor" // #RRGGBB
    static let menuBarUsageBarElevatedColor = "menuBarUsageBarElevatedColor" // #RRGGBB
    static let menuBarUsageBarCriticalColor = "menuBarUsageBarCriticalColor" // #RRGGBB
    static let menuBarUsageBarMediumThreshold = "menuBarUsageBarMediumThreshold" // percent
    static let menuBarUsageBarHighThreshold = "menuBarUsageBarHighThreshold" // percent
    static let menuBarHideIconWithMetrics = "menuBarHideIconWithMetrics" // glyph hides while metrics render in the main item
    static let menuBarMetricOrder = "menuBarMetricOrder" // comma-separated MenuBarMetric raw values
    static let menuBarCombineTemperatures = "menuBarCombineTemperatures" // usage/charge + temperature in one block when possible
    static let menuBarSeparateMetrics = "menuBarSeparateMetrics" // one status item per active metric
    static let menuBarNetworkUploadFirst = "menuBarNetworkUploadFirst" // network menu bar block shows upload above download
    static let menuBarLabelStyle = "menuBarLabelStyle"     // compact | classic
    static let menuBarMemoryStyle = "menuBarMemoryStyle"   // dot | percent | both
    static let monitorMemoryMetric = "monitorMemoryMetric" // used | app
    static let monitorInterval = "monitorIntervalSeconds"  // sampling cadence: 1/2/5
    static let temperatureUnit = "temperatureUnit"          // celsius | fahrenheit
    // System monitor — which blocks appear in the panel.
    static let monitorShowSystem = "monitorShowSystem"
    static let monitorShowNetwork = "monitorShowNetwork"
    static let monitorShowDisk = "monitorShowDisk"
    static let monitorShowPower = "monitorShowPower"
    static let monitorShowMixer = "monitorShowMixer"
    static let panelShowFanControl = "panelShowFanControl"
    static let fanControlMode = "fanControlMode"
    static let fanControlCoolingLevel = "fanControlCoolingLevel"
    static let fanControlCurves = "fanControlCurves"
    // Previous panel visibility key, read once by the migration below.
    static let monitorShowFanControlBeta = "monitorShowFanControlBeta"
    // Machine-only recovery state. A true value means the helper must confirm
    // automatic fan control before this marker can be cleared.
    static let fanControlRecoveryNeeded = "fanControlRecoveryNeeded"
    static let fanControlHelperVersion = "fanControlHelperVersion"
    // System monitor — per-metric history graphs (each independently toggleable).
    static let monitorGraphCPU = "monitorGraphCPU"
    static let monitorGraphGPU = "monitorGraphGPU"
    static let monitorGraphMemory = "monitorGraphMemory"
    static let monitorGraphNetwork = "monitorGraphNetwork"
    static let monitorGraphDisk = "monitorGraphDisk"
    static let monitorGraphPower = "monitorGraphPower"
    static let monitorGraphBattery = "monitorGraphBattery"
    // System monitor — per-item visibility inside each panel section.
    static let monitorSysTemps = "monitorSysTemps"
    static let monitorSysCPU = "monitorSysCPU"
    static let monitorSysGPU = "monitorSysGPU"
    static let monitorSysBattery = "monitorSysBattery"
    static let monitorSysMemory = "monitorSysMemory"
    static let monitorSysAlerts = "monitorSysAlerts"
    static let monitorSysUptime = "monitorSysUptime"
    static let monitorNetSpeed = "monitorNetSpeed"
    static let monitorNetApps = "monitorNetApps"
    static let monitorNetTotals = "monitorNetTotals"
    static let monitorNetTest = "monitorNetTest"
    static let monitorDiskUsage = "monitorDiskUsage"
    static let monitorDiskActivity = "monitorDiskActivity"
    static let monitorDiskSMART = "monitorDiskSMART"
    static let monitorDiskProtection = "monitorDiskProtection"
    static let monitorDiskTools = "monitorDiskTools"
    static let monitorPwrTemperature = "monitorPwrTemperature"
    static let monitorPwrSystem = "monitorPwrSystem"
    static let monitorPwrAdapter = "monitorPwrAdapter"
    static let monitorPwrBattery = "monitorPwrBattery"
    static let monitorPwrTimeRemaining = "monitorPwrTimeRemaining"
    static let monitorPwrHealth = "monitorPwrHealth"
    // System monitor — optional notifications for sustained or actionable conditions.
    static let monitorAlertCPU = "monitorAlertCPU"
    static let monitorAlertCPUTemperature = "monitorAlertCPUTemperature"
    static let monitorAlertBatteryTemperature = "monitorAlertBatteryTemperature"
    static let monitorAlertMemory = "monitorAlertMemory"
    static let monitorAlertDisk = "monitorAlertDisk"
    static let monitorAlertBattery = "monitorAlertBattery"
    static let monitorAlertCPUThreshold = "monitorAlertCPUThreshold"
    static let monitorAlertCPUTemperatureThreshold = "monitorAlertCPUTemperatureThreshold"
    static let monitorAlertBatteryTemperatureThreshold = "monitorAlertBatteryTemperatureThreshold"
    static let monitorAlertDiskFreePercent = "monitorAlertDiskFreePercent"
    static let monitorAlertBatteryPercent = "monitorAlertBatteryPercent"
    static let monitorAlertCooldownMinutes = "monitorAlertCooldownMinutes"
    // Menu panel layout — the order the major sections appear in and which are
    // collapsed, both comma-joined section ids (see PanelSectionID). Absent keys
    // mean the canonical order and nothing collapsed, so no defaults registration.
    static let panelSectionOrder = "panelSectionOrder"
    static let panelUtilityOrder = "panelUtilityOrder"
    static let panelControlOrder = "panelControlOrder"
    static let panelToggleOrder = "panelToggleOrder"
    static let panelSystemOrder = "panelSystemOrder"
    static let panelNetworkOrder = "panelNetworkOrder"
    static let panelDiskOrder = "panelDiskOrder"
    static let panelPowerOrder = "panelPowerOrder"
    static let panelNavigationEnabled = "panelNavigationEnabled" // legacy: the panel always navigates by sections since 3.1.8
    static let updateLastInstallFailure = "updateLastInstallFailure" // last installer step that failed (fail-copy etc.)
    static let windowLayoutHiddenActions = "windowLayoutHiddenActions" // comma-separated action ids hidden from the grid
    static let windowLayoutWindowGap = "windowLayoutWindowGap" // px between adjacent snapped windows
    static let windowLayoutScreenGap = "windowLayoutScreenGap" // px between a snapped window and the visible frame edge
    static let panelCollapsedSections = "panelCollapsedSections"
    static let panelCollapsedResetVersion = "panelCollapsedResetVersion"

    // Media utility — local video, GIF, image and OCR tools.
    static let mediaLastTool = "mediaLastTool"
    static let mediaVideoStart = "mediaVideoStart"
    static let mediaVideoEnd = "mediaVideoEnd"
    static let mediaVideoQuality = "mediaVideoQuality"
    static let mediaVideoMaxDimension = "mediaVideoMaxDimension"
    static let mediaVideoFPS = "mediaVideoFPS"
    static let mediaVideoKeepAudio = "mediaVideoKeepAudio"
    static let mediaVideoCodec = "mediaVideoCodec"
    static let mediaVideoSizing = "mediaVideoSizing"
    static let mediaVideoTargetMegabytes = "mediaVideoTargetMegabytes"
    static let mediaGIFStart = "mediaGIFStart"
    static let mediaGIFEnd = "mediaGIFEnd"
    static let mediaGIFQuality = "mediaGIFQuality"
    static let mediaGIFWidth = "mediaGIFWidth"
    static let mediaGIFFPS = "mediaGIFFPS"
    static let mediaGIFLoops = "mediaGIFLoops"
    static let mediaGIFSizing = "mediaGIFSizing"
    static let mediaGIFTargetMegabytes = "mediaGIFTargetMegabytes"
    static let mediaImageQuality = "mediaImageQuality"
    static let mediaImageMaxDimension = "mediaImageMaxDimension"
    static let mediaImageFormat = "mediaImageFormat"
    static let mediaImageStripMetadata = "mediaImageStripMetadata"
    static let mediaImageResizeKind = "mediaImageResizeKind"
    static let mediaImageResizeWidth = "mediaImageResizeWidth"
    static let mediaImageResizeHeight = "mediaImageResizeHeight"
    static let mediaImageExactResizeMode = "mediaImageExactResizeMode"
    static let mediaImageWatermarkKind = "mediaImageWatermarkKind"
    static let mediaImageWatermarkText = "mediaImageWatermarkText"
    static let mediaImageWatermarkLogoPath = "mediaImageWatermarkLogoPath"
    static let mediaImageWatermarkPosition = "mediaImageWatermarkPosition"
    static let mediaImageWatermarkOpacity = "mediaImageWatermarkOpacity"
    static let mediaImageWatermarkMargin = "mediaImageWatermarkMargin"
    static let mediaImageWatermarkScale = "mediaImageWatermarkScale"
    static let mediaImageRenamePattern = "mediaImageRenamePattern"
    static let mediaImageBackground = "mediaImageBackground"
    static let mediaImagePreserveModificationDate = "mediaImagePreserveModificationDate"
    static let mediaImageProfiles = "mediaImageProfiles"
    static let mediaImageSelectedProfileID = "mediaImageSelectedProfileID"
    static let mediaTextAccurate = "mediaTextAccurate"
    static let mediaTextLanguageCorrection = "mediaTextLanguageCorrection"

    // Clipboard history — text only, opt-in and local.
    static let clipboardHistoryEnabled = "clipboardHistoryEnabled"
    static let clipboardHistoryEntries = "clipboardHistoryEntries"
    static let clipboardHistoryLimit = "clipboardHistoryLimit"
    static let clipboardHistorySkipSensitive = "clipboardHistorySkipSensitive"
    static let clipboardHistoryIncludeImagesFiles = "clipboardHistoryIncludeImagesFiles" // capture copied images and files too
    static let clipboardHistoryIgnoredApps = "clipboardHistoryIgnoredApps" // apps whose copies are never saved
    static let clipboardHistoryQuickPreview = "clipboardHistoryQuickPreview"

    // Auto clear: wipes the system pasteboard on a delay or on sleep and lock.
    // Deliberately outside the clipboardHistory family, since it clears the
    // pasteboard without touching saved entries, and runs with capture off.
    static let clipboardAutoClearOnDelay = "clipboardAutoClearOnDelay"
    static let clipboardAutoClearDelay = "clipboardAutoClearDelaySeconds" // seconds since the last copy
    static let clipboardAutoClearOnSleep = "clipboardAutoClearOnSleep"
    static let clipboardAutoClearOnDisplaySleep = "clipboardAutoClearOnDisplaySleep"
    static let clipboardAutoClearOnScreenLock = "clipboardAutoClearOnScreenLock"

    static let windowPreviewExcludedApps = "windowPreviewExcludedApps" // pause thumbnail capture while these apps are in front
    static let diskEjectExcludedVolumes = "diskEjectExcludedVolumes" // volume names/UUIDs excluded from Eject all disks
    // Quick tools: paste as plain text, color picker, screen OCR, mic mute.
    static let pastePlainEnabled = "pastePlainEnabled"
    static let pastePlainShortcut = "pastePlainShortcut"
    static let colorPickerShortcutEnabled = "colorPickerShortcutEnabled"
    static let colorPickerShortcut = "colorPickerShortcut"
    static let colorPickerFormat = "colorPickerFormat"       // hex | rgb | hsl | swiftui
    static let colorPickerBareHex = "colorPickerBareHex"     // copy HEX without the leading #
    static let screenOCRShortcutEnabled = "screenOCRShortcutEnabled"
    static let screenOCRShortcut = "screenOCRShortcut"
    static let screenOCRRemoveLineBreaks = "screenOCRRemoveLineBreaks"
    static let screenOCRDetectQRCodes = "screenOCRDetectQRCodes" // QR content wins over OCR text
    static let micMuteShortcutEnabled = "micMuteShortcutEnabled"
    static let micMuteShortcut = "micMuteShortcut"
    static let cameraPreviewShortcutEnabled = "cameraPreviewShortcutEnabled"
    static let cameraPreviewShortcut = "cameraPreviewShortcut"
    static let scratchpadShortcutEnabled = "scratchpadShortcutEnabled"
    static let scratchpadShortcut = "scratchpadShortcut"
    static let commandBarShortcutEnabled = "commandBarShortcutEnabled"
    static let commandBarShortcut = "commandBarShortcut"
    /// Compact mode: an empty field shows nothing but itself. Off by default
    static let commandBarCompactMode = "commandBarCompactMode"
    static let commandBarUsage = "commandBarUsage"           // per-command run counts, never queries
    static let commandBarQueryHabits = "commandBarQueryHabits" // keyed query digests → app row ids
    static let commandBarDisabledSources = "commandBarDisabledSources" // kinds of result switched off
    static let commandBarAliases = "commandBarAliases"       // {row id: the name the person gave it}
    static let commandBarPins = "commandBarPins"             // row keys kept at the top, in order
    static let commandBarHidden = "commandBarHidden"         // row keys the person never wants offered
    static let commandBarLinks = "commandBarLinks"           // Data: [CommandBarLink] JSON
    static let commandBarRowShortcuts = "commandBarRowShortcuts" // {row key: shortcut}
    static let commandBarPositionOffset = "commandBarPositionOffset" // "dx,dy" from the default spot
    // The folders a file search looks in, one per line, written with a tilde
    // so an exported list still points somewhere on another Mac. Empty means
    // the bar looks for no files at all, which is the setting out of the box.
    static let commandBarFileScopes = "commandBarFileScopes"
    static let commandBarFileIgnores = "commandBarFileIgnores" // names a file search never shows
    static let panelUtilityCommandBar = "panelUtilityCommandBar"
    static let scratchpadRetention = "scratchpadRetention"   // never | day | week | month
    static let scratchpadCloseOnClickOutside = "scratchpadCloseOnClickOutside"
    static let scratchpadBackgroundOpacity = "scratchpadBackgroundOpacity" // opaque fill over the pad material (ScratchpadSupport.backgroundOpacityRange)
    static let scratchpadDocument = "scratchpadDocument"     // Data: ScratchpadDocument JSON, including named tabs
    static let micMuteActive = "micMuteActive"               // mic muted by the app (survives relaunch)
    static let micMuteSavedVolume = "micMuteSavedVolume"     // input volume to restore on unmute (pre 3.2.0 state)
    static let micMuteSavedVolumes = "micMuteSavedVolumes"   // [device uid: input volume] to restore on unmute
    static let micMuteMutedDevices = "micMuteMutedDevices"   // uids of the devices this app muted
    static let micMuteMenuBarIndicator = "micMuteMenuBarIndicator" // badge the status icon while muted
    static let quickLauncherShortcutEnabled = "quickLauncherShortcutEnabled"
    static let quickLauncherShortcut = "quickLauncherShortcut"
    static let quickLauncherItemOrder = "quickLauncherItemOrder"
    static let quickLauncherHiddenItems = "quickLauncherHiddenItems"
    static let panelUtilityQuickLauncher = "panelUtilityQuickLauncher"
    static let panelUtilityColorPicker = "panelUtilityColorPicker"
    static let panelUtilityScreenOCR = "panelUtilityScreenOCR"
    static let panelUtilityCameraPreview = "panelUtilityCameraPreview"
    static let panelUtilityScratchpad = "panelUtilityScratchpad"
    static let clipboardHistoryShortcutEnabled = "clipboardHistoryShortcutEnabled"
    static let clipboardHistoryShortcut = "clipboardHistoryShortcut"
    // Mode chooser visibility for dedicated capture shortcuts.
    static let screenshotShowCaptureMenuOnShortcut = "screenshotShowCaptureMenuOnShortcut"
    static let recorderShowCaptureMenuOnShortcut = "recorderShowCaptureMenuOnShortcut"
    static let screenOCRShowCaptureMenuOnShortcut = "screenOCRShowCaptureMenuOnShortcut"
    static let colorPickerShowCaptureMenuOnShortcut = "colorPickerShowCaptureMenuOnShortcut"
    // Screenshot capture and editor.
    static let screenshotShortcutEnabled = "screenshotShortcutEnabled"
    static let screenshotShortcut = "screenshotShortcut"
    static let unifiedScreenCaptureShortcutMigrated = "unifiedScreenCaptureShortcutMigrated"
    static let restoredScreenCaptureShortcutsMigrated = "restoredScreenCaptureShortcutsMigrated"
    static let orphanedCaptureShortcutMigrated = "orphanedCaptureShortcutMigrated"
    static let screenshotFullScreenShortcutEnabled = "screenshotFullScreenShortcutEnabled"
    static let screenshotFullScreenShortcut = "screenshotFullScreenShortcut"
    static let screenshotLastCaptureShortcutEnabled = "screenshotLastCaptureShortcutEnabled"
    static let screenshotLastCaptureShortcut = "screenshotLastCaptureShortcut"
    static let recentCapturesShortcutEnabled = "recentCapturesShortcutEnabled"
    static let recentCapturesShortcut = "recentCapturesShortcut"
    static let screenshotClipboardShortcutEnabled = "screenshotClipboardShortcutEnabled"
    static let screenshotClipboardShortcut = "screenshotClipboardShortcut"
    static let screenshotFreeze = "screenshotFreeze"
    static let screenshotHideVorssaintWindows = "screenshotHideVorssaintWindows"
    static let screenshotSaveFolder = "screenshotSaveFolder"
    static let screenshotSaveSubfolder = "screenshotSaveSubfolder"
    static let screenshotFileNamePattern = "screenshotFileNamePattern"
    static let screenshotFileNumberStart = "screenshotFileNumberStart"
    static let screenshotFileNumberNext = "screenshotFileNumberNext"
    static let screenshotDefaultAction = "screenshotDefaultAction"
    static let screenshotIncludePointer = "screenshotIncludePointer"
    static let screenshotShowLastRegion = "screenshotShowLastRegion"
    static let screenshotLoupeStartsOn = "screenshotLoupeStartsOn"
    static let screenshotLoupeRememberZoom = "screenshotLoupeRememberZoom"
    static let screenshotLoupeDefaultZoom = "screenshotLoupeDefaultZoom"
    static let screenshotLoupeLastZoom = "screenshotLoupeLastZoom"
    static let screenshotLoupeSteppedZoomByDefault = "screenshotLoupeSteppedZoomByDefault"
    static let screenshotDownscale = "screenshotDownscale"
    static let screenshotDelay = "screenshotDelay"
    static let screenshotLastTool = "screenshotLastTool"
    static let screenshotLastColor = "screenshotLastColor"
    static let screenshotLastStroke = "screenshotLastStroke"
    static let screenshotLastSticker = "screenshotLastSticker"
    static let screenshotAnnotationShadows = "screenshotAnnotationShadows"
    static let screenshotToolOrder = "screenshotToolOrder"
    static let screenshotToolShortcuts = "screenshotToolShortcuts"
    static let screenshotToolShortcutsEnabled = "screenshotToolShortcutsEnabled"
    static let screenshotBackdropStyle = "screenshotBackdropStyle"
    static let screenshotBackdropPresets = "screenshotBackdropPresets"
    static let screenshotOpenEditorDirectly = "screenshotOpenEditorDirectly"
    static let screenshotCopyToClipboard = "screenshotCopyToClipboard"
    static let screenshotPreviewPosition = "screenshotPreviewPosition"
    static let screenshotPreviewTakesFocus = "screenshotPreviewTakesFocus"
    static let screenshotSharingEnabled = "screenshotSharingEnabled"
    // Developer-only endpoint for an isolated test tunnel. The official app
    // ignores it, and settings backups must never carry it to another Mac.
    static let screenshotSharingDeveloperEndpoint = "screenshotSharingDeveloperEndpoint"
    static let panelUtilityScreenshot = "panelUtilityScreenshot"

    // Screen recorder - records the picked area, keeps the untouched master
    // in Application Support until retention sweeps it.
    static let recorderShortcutEnabled = "recorderShortcutEnabled"
    static let recorderShortcut = "recorderShortcut"
    static let recorderCountdown = "recorderCountdown"
    static let recorderQuality = "recorderQuality"
    static let recorderFrameRate = "recorderFrameRate"
    static let recorderSystemAudio = "recorderSystemAudio"
    static let recorderMicrophone = "recorderMicrophone"
    // Machine state, never exported: whether this Mac's audio system has let
    // a recording hear the Mac's sound through a process tap.
    static let recorderSystemAudioTapVerified = "recorderSystemAudioTapVerified"
    static let recorderSaveFolder = "recorderSaveFolder"
    static let recorderOpenEditor = "recorderOpenEditor"
    static let recorderAutomaticZoom = "recorderAutomaticZoom"
    static let recorderGIFSize = "recorderGIFSize"
    static let recorderGIFFrameRate = "recorderGIFFrameRate"
    static let recorderEditorPresets = "recorderEditorPresets"
    static let recorderSharingEnabled = "recorderSharingEnabled"
    static let panelUtilityScreenRecorder = "panelUtilityScreenRecorder"

    // Window Layout — snapping, global shortcuts and optional pointer gestures.
    static let windowLayoutShortcutsEnabled = "windowLayoutShortcutsEnabled"
    static let windowDirectionalEnabled = "windowDirectionalEnabled"
    static let windowDirectionalShortcut = "windowDirectionalShortcut"
    static let windowEdgeSnapEnabled = "windowEdgeSnapEnabled"
    static let windowEdgeSnapDisabledZones = "windowEdgeSnapDisabledZones" // comma-separated visual zone ids
    static let windowGestureEnabled = "windowGestureEnabled"
    static let windowGestureModifiers = "windowGestureModifiers"
    static let windowGestureRaiseWindow = "windowGestureRaiseWindow"
    static let windowLayoutShortcutLeft = "windowLayoutShortcutLeft"
    static let windowLayoutShortcutRight = "windowLayoutShortcutRight"
    static let windowLayoutShortcutTop = "windowLayoutShortcutTop"
    static let windowLayoutShortcutBottom = "windowLayoutShortcutBottom"
    static let windowLayoutShortcutCenterHalf = "windowLayoutShortcutCenterHalf"
    static let windowLayoutShortcutTopLeft = "windowLayoutShortcutTopLeft"
    static let windowLayoutShortcutTopRight = "windowLayoutShortcutTopRight"
    static let windowLayoutShortcutBottomLeft = "windowLayoutShortcutBottomLeft"
    static let windowLayoutShortcutBottomRight = "windowLayoutShortcutBottomRight"
    static let windowLayoutShortcutMaximize = "windowLayoutShortcutMaximize"
    static let windowLayoutShortcutMarginMaximize = "windowLayoutShortcutMarginMaximize"
    static let windowLayoutShortcutCenter = "windowLayoutShortcutCenter"
    static let windowLayoutShortcutRestore = "windowLayoutShortcutRestore"
    static let windowLayoutShortcutLeftThird = "windowLayoutShortcutLeftThird"
    static let windowLayoutShortcutCenterThird = "windowLayoutShortcutCenterThird"
    static let windowLayoutShortcutRightThird = "windowLayoutShortcutRightThird"
    static let windowLayoutShortcutLeftTwoThirds = "windowLayoutShortcutLeftTwoThirds"
    static let windowLayoutShortcutRightTwoThirds = "windowLayoutShortcutRightTwoThirds"
    static let windowLayoutShortcutPreviousDisplay = "windowLayoutShortcutPreviousDisplay"
    static let windowLayoutShortcutNextDisplay = "windowLayoutShortcutNextDisplay"
    static let windowLayoutShortcutFullScreen = "windowLayoutShortcutFullScreen"
    static let windowLayoutShortcutTopLeftSixth = "windowLayoutShortcutTopLeftSixth"
    static let windowLayoutShortcutTopCenterSixth = "windowLayoutShortcutTopCenterSixth"
    static let windowLayoutShortcutTopRightSixth = "windowLayoutShortcutTopRightSixth"
    static let windowLayoutShortcutBottomLeftSixth = "windowLayoutShortcutBottomLeftSixth"
    static let windowLayoutShortcutBottomCenterSixth = "windowLayoutShortcutBottomCenterSixth"
    static let windowLayoutShortcutBottomRightSixth = "windowLayoutShortcutBottomRightSixth"

    // Text snippets: type a trigger, get the expansion.
    static let textSnippetsEnabled = "textSnippetsEnabled"
    static let textSnippets = "textSnippets"              // Data: [TextSnippet] JSON
    static let snippetLibraryEnabled = "snippetLibraryEnabled"
    static let snippetLibraryShortcut = "snippetLibraryShortcut"

    // Radial menu: a wheel of actions on a shortcut.
    static let radialMenuEnabled = "radialMenuEnabled"
    static let radialMenuShortcut = "radialMenuShortcut"
    static let radialMenuAtPointer = "radialMenuAtPointer" // false: screen center
    static let radialMenuMouseButton = "radialMenuMouseButton" // RadialMenuMouseTrigger.rawValue
    static let radialMenuActivationMode = "radialMenuActivationMode" // RadialMenuActivationMode.rawValue
    static let radialMenuItems = "radialMenuItems"        // Data: [RadialMenuItem] JSON
    static let radialMenuProfiles = "radialMenuProfiles"  // Data: [RadialMenuProfile] JSON

    // Dev-build only: force the "update available" UI for local testing.
    static let simulateUpdate = "simulateUpdate"
    static let simulateBetaUI = "simulateBetaUI"

    /// Features hub availability layer, one key per AppFeature raw value.
    /// Registered true: unavailable features vanish from every surface and
    /// hold no resources, without ever touching their own enable keys.
    static func featureAvailable(_ id: String) -> String { "featureAvailable.\(id)" }
}
