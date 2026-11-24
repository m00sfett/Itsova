# Product

<!-- impeccable:product-schema 1 -->

## Platform

desktop (KDE Plasma on Wayland, native Qt 6 Widgets with the Breeze style; not one of web/ios/android)

## Users

First the author, a KDE Plasma power user with two monitors at different scales. Later other KDE Plasma users who find the project on GitHub. Their job: press Print in the middle of other work and get the screen saved instantly, then optionally cut it down to exactly what they need; and capture the same fixed areas, monitors or windows again and again at the moment a shortcut is pressed.

## Product Purpose

glimply replaces the Print key tool (since 0.2.0): Print always yields a file immediately; an overlay with labelled buttons then narrows it down to rectangles, a monitor, a window or a saved area, without shortcuts to learn. Predefined targets still capture instantly from their own shortcuts, without any overlay. Success: Print never loses a moment, choosing what to keep takes one gesture, and an area set up once yields an identical, native-resolution PNG on every shortcut press.

## Positioning

Fixed areas bound to a specific monitor, groups that capture several targets from one frame per monitor with one shared timestamp, and true native resolution on mixed-scale setups.

## Operating Context

Runs as a tray icon or Plasma widget. Triggered by the Print key, global shortcuts (registered in KDE's global shortcut system) or the widget. The capture overlay covers every monitor, each with its own top bar, frozen or live (lighter, so a video stays watchable); Space switches, and it starts in the last used mode unless the settings fix one. Areas and groups are created inline in the overlay; the settings window is for rare configuration.

## Capabilities and Constraints

- Targets: areas (single-monitor logical coordinates auto-named `<monitor>-<n>`, or global cross-monitor areas with `8 px` magnetic monitor-edge snapping, transparent cutouts outside monitor bounds, byte-identical single-monitor fast path, and graceful partial-visibility capture + passive notification when an involved monitor is disconnected), built-in per-monitor targets, "All monitors". Groups of targets with their own shortcut.
- Capture Delay Timer: non-blocking, non-focus-stealing countdown banner (`Off`, `3 s`, `5 s`, `10 s`, `Custom…` 1–60 s) triggered pre-capture from the widget, tray, D-Bus, or per-area/group shortcuts, or in-overlay via `Delay` retake; One-Shot default reset unless `Keep delay active` is toggled on.
- Precision Pixel Loupe & Color Picker: nearest-neighbor `11 × 11` Frozen selection drag magnifier (toggleable in Settings) and `15 × 15` eyedropper (`I` / *Pick Color* inside `CaptureOverlay` and `QuickEditorWindow`, or standalone from the Plasma widget, tray menu, D-Bus `PickColor`, and optional global shortcut) with persistent `Tab` cycling (`#RRGGBB`, `rgb()`, `hsl()`, `hsv()`) and `Alt+Click` annotation color application.
- Monitors can be named; the name belongs to the physical monitor (manufacturer, model, serial), falls back to the connector, and is used for display, monitor capture file names and new area names.
- Unified PhotoCraft / Photoshop modifiers across overlays and markup: with the mouse, `Shift` keeps a square/circle (or 15° angle snap), `Alt` grows from the centre (or duplicates an annotation), `Space` while dragging repositions in-progress shapes/selections live, and `Ctrl` in markup temporarily switches to `Select / Move`. With arrow keys, `Shift` steps by 10, `Alt` resizes around the centre, and `Ctrl` resizes as a square.
- Optional progressive-disclosure markup in the capture overlay (`Annotate` or single-letter tool keys `A R E B M T N X S`): non-destructive vector callouts, rich text with legibility modes, step counter, spotlight dim mask, and irreversible pixel redactions (`Solid`, `Pixelate`, `Blur`) rendered directly at native monitor DPI, with automatic `Print`-snapshot leak protection when redacting.
- Standalone native KDE/Breeze `QuickEditorWindow`: opened from the capture overlay (`Edit in Window` / `Ctrl+E`, transferring raw image + still-editable vector annotations), capture notification (`Edit`), Plasma widget / SNI tray (`Edit Last Capture`, D-Bus `EditLastCapture`), or CLI (`glimply <file>` / `glimply --edit <file>`). Includes undoable `Crop` (`C`), `Pick Color` (`I`), PhotoCraft zoom/pan (`Ctrl+Wheel` / `Alt+Wheel`, `Space`+drag / Middle-drag, `Ctrl+0` / `Ctrl+1`, crisp pixels `≥ 400 %`, pixel grid `≥ 800 %`), explicit `Save` (`Ctrl+S`), `Save Copy` (`Ctrl+Alt+S`), `Save As…` (`Ctrl+Shift+S`), and `Copy` (`Ctrl+C`) without unintended clipboard or disk writes.
- No default shortcuts. Cursor off by default, per target and per group.
- Storage & Export Formats: `~/Pictures/glimply/`, flat, `{date}_{time}_{area}.{ext}` with PNG default plus JPEG (`#000000` black alpha flattening), WebP (lossless/lossy), and runtime-detected AVIF / JPEG XL, Target > Group > Global overrides, metadata stripping, and a compact Breeze `Save As…` dialog with async file-size estimation.
- Feedback: one short tray icon blink after success; a notification with Open, Edit, and Show in Folder after Print and overlay captures; low-urgency passive notifications for partial cross-monitor captures, copied colors, stopped intervals, and MCP captures; errors (including hook failures) notify cleanly.
- Interval captures: concurrent per-target/per-group timed captures (`Every N s`, `Only save when changed` exact pixel filter preserving group simultaneity, optional capture/time limits), dedicated toggle shortcuts, tray/widget interval state icon + `Stop` / `Stop All`, and low-disk (`< 100 MB`) / disconnect protection.
- Scriptable CLI (`capture`, `list`, `area`, `interval`, `overlay` with `--stdout`, `--no-save`, `--json`) and expanded async session D-Bus API (`CaptureSync`, `ListMonitors`, `ListWindows`, `LastCaptureInfo`, `ResolveTarget`, `AddAreaRect`, `OpenOverlay`, `McpDispatch`), plus shell-free async post-capture command hooks (`Automation` Settings page + per-target/group overrides).
- Local stdio MCP server (`glimply mcp`): strictly local, disabled by default, three security scopes (`Allowlist`, `Confirm every capture` 30 s auto-deny Breeze dialog, `Full session access`), metadata-first responses with optional `include_image` + `max_dimension`, and programmatic vector callout / irreversible redaction tools (`annotate_image`).
- All UI text translatable; English by default, 18 more languages later.
- Capture history: the newest captures (default 20, persisted, interval captures excluded) as a thumbnail grid in the widget's collapsible *Recent* section and a *Recent Captures* tray submenu, with Open, Edit in Window, Copy, Show in Folder and Move to Trash (system trash only); `glimply history` and D-Bus `History()` for scripts.
- Not yet: shaped selections, targets that follow a window, text recognition, upload.

## Brand Commitments

"glimply" is a working name. The icons use the roteson.de colours and frame (dark tile #080604, red #B11218, yellow #FFD21F), so glimply reads as a sibling of the other roteson.de tools such as Cinegain: app icon with the dark-red outer ring, tray icon as a red tile with yellow viewfinder corners that flashes yellow with red corners after a capture. The settings window carries a footer with version, licence, links to roteson.de and GitHub, and the roteson.de and R²W² brand marks; nowhere else. Apart from that the UI follows the native KDE look and does not introduce its own visual style.

## Product Principles

1. Invisible while capturing: nothing of glimply may appear in a capture (the live overlay hides before it captures).
2. Set up once, then forget: configuration is rare, so it can be explicit; triggering must be effortless.
3. Never swallow errors: skipped targets and failures are always reported, never cropped or dropped silently.
4. Native first: behave like a well-made KDE application (shortcuts, tray, notifications, file manager).
