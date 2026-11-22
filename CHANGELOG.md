# Changelog

All notable changes to this project will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
