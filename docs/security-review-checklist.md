# Pre-1.0.0 Security Review Checklist

Tracking issue: [#143](https://github.com/m00sfett/Itsova/issues/143) (originated from [#139](https://github.com/m00sfett/Itsova/issues/139)).

Itsova has not yet undergone an independent external security audit. Before tagging `1.0.0`, complete and record verification for each area below.

## 1. Screen Capture & Wayland / D-Bus Boundaries

- [ ] Verify desktop-file interface declarations (`X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2`, `X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1`) and document observed KWin enforcement behavior accurately.
- [ ] Review the local session D-Bus interface (`io.github.m00sfett.itsova.Control`, including `CaptureSync`) and verify documentation clearly states it is governed by session D-Bus access, separate from MCP access modes.
- [ ] Verify temporary files and PipeWire/Wayland handles are cleaned up on cancellation, crash recovery, or interrupted recordings.

## 2. Redaction

- [ ] Verify **Solid** redaction replaces every covered device pixel (including fractional-DPI edge rounding in `redactionPhysicalRect`) with an opaque flat color so no original pixel data remains in the exported image.
- [ ] Verify UI tooltips, MCP descriptions, and documentation consistently recommend **Solid** for sensitive text and frame **Pixelate** and **Blur** as visual obfuscation filters without claiming independent forensic/cryptographic certification.
- [ ] Verify unredacted `Print` captures are not leaked to disk or clipboard when redaction is used (RAM-first default and opt-in immediate-save cleanup paths).

## 3. File Permissions & Path Handling

- [ ] Audit file creation modes for captures, `itsovarc`, capture history (`history.json`), thumbnails, and saved pins.
- [ ] Verify symlink rejection and atomic write paths (`QSaveFile` / overwrite checks) across capture export, Quick Editor, and MCP `annotate_image`.
- [ ] Verify all path-confinement checks resolve canonical paths and prevent directory traversal outside configured output directories.

## 4. Hooks & External Processes

- [ ] Audit post-capture hook execution (command/argument passing, environment variables, error logging without leaking secrets or hook commands).
- [ ] Audit external process invocations (`tesseract`, `ffmpeg`, `ffprobe`, `journalctl`) for argument injection (`--` separators / explicit `QProcess` argument lists, no shell interpolation), stdin/stdout handling, and timeouts.

## 5. Local MCP Server (`itsova mcp`)

- [ ] Verify MCP stays disabled by default until explicitly enabled in Settings.
- [ ] Verify all three access scopes (`Allowlist`, `ConfirmPerCapture`, `FullSession`) enforce their documented boundaries across all 10 tools.
- [ ] Verify `ConfirmPerCapture` dialogs display full target/output/overwrite context and default to deny on close or timeout.
- [ ] Verify `annotate_image` enforces capture-directory confinement, symlink rejection, and explicit `overwrite=true` for any existing target file across all access modes.
