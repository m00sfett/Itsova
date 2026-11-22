# Changelog

All notable changes to this project will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.9] - Unreleased

### Added

- Selection overlay: hold Ctrl while drawing or resizing to grow the area from its centre (an edge moves its opposite edge by the same amount, a corner scales both dimensions around the centre), and Alt to keep a perfect square. Both combine, react when pressed mid-drag and stay intact at the screen border.

## [0.1.8] - Unreleased

### Fixed

- The Plasma widget now appears in the system tray by itself: it is enabled by default, and `scripts/install.sh` restarts a running glimply and reloads the Plasma shell once when the widget is new or changed. No manual tray configuration is needed.

## [0.1.7] - Unreleased

### Added

- Plasma widget for the system tray: areas, groups and monitors with their shortcuts and buttons to capture, edit the area on screen, rename and delete inline; Add Area, Open Last Capture, Open Capture Folder and Configure. The icon blinks after a capture.
- Session bus interface `io.github.m00sfett.glimply.Control` and D-Bus activation, so the widget can start glimply.
- Selection overlay: round grab handles on corners and edge midpoints, resizing from any edge or corner (grab zone adapts to small selections), and a translucent centre crosshair with dotted guides.

### Changed

- Regions are now called areas everywhere. Existing configurations, group members and global shortcuts are migrated automatically; `{region}` still works in file name templates, the default is `{date}_{time}_{area}.png`.
- While the widget is loaded, the separate tray icon steps back into the hidden part of the tray.

## [0.1.6] - Unreleased

### Added

- `scripts/install.sh`: Release build, tests and installation into `~/.local` (binary, desktop entry with the KWin screenshot permission, icon, notification events); `--uninstall` removes the installed files again.
- Start at login, on by default from the first start and switchable in the settings.
- Application icon and own notification events, so errors appear under glimply's name and can be configured in System Settings.

### Changed

- README describes installation, permissions and usage.

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
