# Changelog

All notable changes to this project will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.5] - Unreleased

### Added

- Settings window (Targets, Groups, Storage, General) with immediate apply: rename regions, adjust position and size numerically, pointer setting, global shortcut fields with conflict checks, group members, capture folder, file name template with live example, start at login.
- Region selection overlay on every monitor with a frozen, dimmed picture; drag, move, nudge with arrow keys, confirm with Enter or double click.
- Accessible names for all editable fields.

### Changed

- The whole interface stays in English until glimply ships translations, instead of mixing in system-language texts from KDE widgets.

## [0.1.4] - Unreleased

### Added

- Tray application: a click opens a menu with all groups, regions and monitors, their global shortcuts, Add Region, Open Last Capture, Open Capture Folder, Configure and Quit.
- Global shortcuts for every target and group through KGlobalAccel, without defaults; shortcuts edited in System Settings are picked up.
- The tray icon blinks once after a successful capture; failures, skipped targets and monitor changes raise notifications.
- Open Last Capture opens a single file in the image viewer and shows all files of a group in the file manager.
- Single-instance start through D-Bus.

### Changed

- Capture files are created with mode 0644 instead of 0600.

## [0.1.3] - Unreleased

### Added

- Persistent regions with automatic `<connector>-<n>` names, groups, built-in per-monitor and all-monitors targets, and capture settings stored with KConfig.
- Trigger planning that captures every involved monitor once, in parallel, and writes all outputs of a trigger with one shared timestamp.
- All-monitors composition at the highest monitor scale with transparent gaps.
- Skipped targets and monitor identity changes are reported instead of being cropped silently.

## [0.1.2] - Unreleased

### Fixed

- Report crop allocation failures and clear the previous output path when a write fails.

## [0.1.1] - Unreleased

### Added

- Native per-monitor capture through KWin ScreenShot2, monitor-relative region cropping, and collision-safe PNG output.
- Automated image output tests and an opt-in live capture check.

## [0.1.0] - Unreleased

### Added

- Initial CMake and Qt application skeleton.
- Project README and GNU AGPL-3.0-or-later license.
