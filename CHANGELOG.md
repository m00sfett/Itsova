# Changelog

All notable changes to this project will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.9.0] - Unreleased

Overlay redesign and capture latency (milestone 0.9.0). The repository stays private until 1.0.0.

### Added

- Latency measurement (#67): every Print capture records how long it took from the moment glimply receives the shortcut to the capture request, the pixels in memory, the first overlay frame and the written file. `glimply latency [--last N] [--json]` shows the last 50 captures with median, 95th percentile and maximum (D-Bus `Control.LatencyReport()`). `scripts/measure-latency.sh` measures a build in a nested, virtual KWin with a throw-away home, without touching the desktop session. The time from the physical key press to the shortcut is not measurable: on Wayland, KDE's global shortcut service sends the key timestamp as 0.
- Overlay panel shortcuts (#70): `1` this monitor, `2` all monitors, `W` window list, `G` areas, `D` delay, `F10` or the Menu key for *More*, `Ctrl+C` copy, `Ctrl+Shift+S` save as, `Ctrl+B` save as area and `Ctrl+,` settings. In an open window or area list, `1`–`9` pick the numbered entries. The keys work only where their button exists (for example `Ctrl+C` only with a selection), the existing keys are unchanged, and none of them collides with a default KDE global shortcut. Every panel button names its key in its tooltip. *More* without a selection also offers *Freeze*/*Go Live* (`Space`) and calls the help *Shortcut Overview* (`F1`); drawer contents start at the leading edge of the capsule.

### Changed

- Faster Print overlay (#67): composing, encoding and writing the full screenshot now run in the background, so the overlay opens as soon as the pixels are captured. Measured in a nested KWin (median of 20): overlay after 16 ms instead of 75 ms with one 1920×1080 monitor, 26 ms instead of 143 ms with two. The file is still always written; actions that need it wait for it, and the clipboard and history follow once it is on disk.
- Visible pins no longer delay every capture by a fixed 250 ms (#67): glimply waits until the compositor has shown the transparent pin frames (Wayland frame callback), at most 250 ms. Measured with one pin: pixels in memory after 11 ms instead of 257 ms; pins stay invisible in captures (checked in 20 of 20 captures against a control capture that shows the pin).
- One overlay panel instead of five bars (#69): the top bar, the action bar at the selection, the markup bar, the *Save as Area* panel and the help panel are replaced by one capsule per capture session. It shows on one monitor: where the pointer is while nothing is selected, then where the selection is; dragging it by its grip (⋮⋮) or margin moves it, also to another monitor, and the position is remembered per monitor (double-click on the grip resets it). It starts at the top centre and moves to the bottom when it would cover the selection. Buttons have no labels: hovering fades the icon and shows the title, a tooltip after 500 ms adds the shortcut. Frozen/Live is the round end cap of the capsule. Windows, areas, delay, markup tools, more actions, the area form and the help open as drawers inside the capsule (a second click or Escape closes them) instead of popup menus. The size of the active rectangle is shown at its corner. The menu layout follows the redesign plan; the reworked help follows in #72.

## [0.8.0] - Unreleased

Public release readiness: translations, packaging and documentation. The repository stays private until 1.0.0.

### Added

- Translation infrastructure (#51): the application uses Qt catalogs (`i18n/glimply_<lang>.ts`, compiled to `.qm` and installed to `share/glimply/translations`), the Plasma widget uses gettext catalogs (`plasmoid/po`, installed to `share/locale`). Catalogs exist for Arabic, Chinese (Simplified and Traditional), Dutch, French, German, Hindi, Indonesian, Italian, Japanese, Korean, Polish, Portuguese, Russian, Spanish, Turkish, Ukrainian and Vietnamese; English is the source language.
- glimply picks its language from `[General] Language` in `glimplyrc` when set, else from the system language, else English. KDE and Qt dialogs follow the same language, so a window never mixes two languages. Right-to-left languages mirror the layout.
- `scripts/update-translations.sh` refreshes all catalogs from the code; `scripts/check-translations.py` checks them (placeholders, accelerators, plural forms, untranslated entries) and runs as a CTest test. `i18n/glossary.md` holds the terminology and the words that are never translated.

- Language setting (#53): *Settings → General → Language* offers *System default* and the 19 languages by their own names. After a change, an inline message offers *Restart now*; glimply quits and starts again in the new language. The Plasma widget follows the Plasma language.
- Arch Linux packaging (#56): `packaging/arch/glimply-git/PKGBUILD` (development package) and `packaging/arch/glimply/PKGBUILD` (for tagged releases from `1.0.0`), installing the binary, desktop entry (`ScreenShot2` restricted interface), D-Bus service, notification config, icons, translations, and Plasma widget.
- Documentation (#57): rewrote `README.md` in English covering current features, local-only privacy, `ScreenShot2` / `Print` / MCP / OCR permissions, Qt 6.8 / KF 6.13 requirements, Arch PKGBUILD and source build instructions, CLI usage, and translation notes (no screenshots before `1.0.0`).
- UI translations (#52): completed all 18 additional languages (`ar`, `de`, `es`, `fr`, `hi`, `id`, `it`, `ja`, `ko`, `nl`, `pl`, `pt`, `ru`, `tr`, `uk`, `vi`, `zh_CN`, `zh_TW`) across Qt `.ts` catalogs, Plasma widget `.po` catalogs, desktop entry, notifyrc events, and widget metadata, and enabled the strict `translations` CTest check without `--allow-unfinished`.
- RTL layout support (#54): mirror custom-painted overlay action bar anchor, corner close button, selection badge, pin window zoom/opacity readout, color-picker loupe swatch/value row, toolbar sub-group spacing, and RTL help-panel key/meaning alignment under right-to-left locales (`ar`), wrap the *General* settings page in a `QScrollArea` so hints and the inline restart banner never clip, and keep capture rectangles, monitor geometry, magnified pixel grids, and pinned images unmirrored.

### Changed

- MCP denial messages and `--json` CLI output stay English whatever the UI language; human-readable CLI messages follow it.
- English plural forms are now correct where a count is shown ("1 capture", "3 seconds").
- Lowered minimum supported toolchain (#55) from Qt 6.12 to Qt 6.8 and KDE Frameworks 6.13 (CMake 3.31+), verified in a Debian 13 (`trixie`) container via `scripts/check-debian13.sh`, with a compile-time compatibility fallback for `KStatusNotifierItem::setIsMenu` before KF6 6.14.

## [0.7.1] - 2026-11-24

### Fixed

- glimply no longer crashes on start: the tray menu was built while the application was still being set up and read the interval state before it existed. The crash showed up with 0.7.0; earlier versions had the same flaw but happened not to trip over it.

## [0.7.0] - 2026-11-24

Daily-driver milestone: capture history, Pin-to-Screen and local text recognition.

### Added

- Capture history (#26): glimply remembers the most recent captures (default 20, adjustable from 5 to 100 under *Settings → Storage*) across restarts in `~/.local/state/glimply/history.json`, with cached thumbnails in `~/.cache/glimply/thumbs/`. Manual, CLI, MCP and quick-editor captures are recorded; interval captures are not. Entries whose files were deleted or moved disappear automatically.
- Plasma widget: a collapsible *Recent* section below the targets shows the newest captures as a thumbnail grid (8 at first, *Show all* for the rest). Click opens a capture; the corner button, right click or the Menu key offer *Open*, *Edit in Window*, *Copy*, *Show in Folder* and *Move to Trash*. Captures with several files show a count badge and expand into their files.
- Tray menu: a *Recent Captures* submenu with the newest 10 captures and the same actions, plus *Clear History*.
- *Move to Trash* uses the system trash and never deletes permanently. *Clear History* only empties the list; image files stay where they are.
- CLI: `glimply history [--limit N] [--json]` lists recent captures. D-Bus: `History()`, `HistoryAction(action, entryId, path)`, `ClearHistory()` and the `HistoryChanged` signal; actions only accept files that are recorded in the history.
- Pin-to-Screen (#28): keep a screenshot on screen as a borderless reference window above all other windows. Pin from the capture overlay (`P` or the *Pin* button; one pin per rectangle, annotations included, no file written), from the quick editor (*Pin to Screen*), from a recent capture, or with the global shortcut *Pin last capture to the screen* (unbound by default).
- Pins survive restarts until you remove them (stored in `~/.local/share/glimply/pins/`), come back on their monitor (or on the primary monitor while it is unplugged), and are made invisible during every capture so they never end up in a screenshot.
- Pin controls: drag to move, mouse wheel zooms in steps from 25 % to 400 %, Ctrl+wheel sets the opacity (20–100 %), double click resets to 100 %, Ctrl+C copies; the context menu offers *Copy*, *Edit in Window*, *Save As…*, zoom, opacity and *Remove Pin*. `Esc` or *Remove Pin* removes a pin with an *Undo* notification for 10 seconds.
- Tray menu *Pins (N)*: *Show Pins* toggles all pins (remembered across restarts; also a global shortcut, unbound by default) and *Remove All Pins…* removes them after a confirmation.
- Optional local text recognition (#27): *Copy Text* (`O` or the button in the capture overlay, `Ctrl+Alt+C` in the quick editor) recognizes the text of the selection (several rectangles in reading order, the whole monitor without a selection) and copies it to the clipboard, with a notification showing the start of the text. Annotations are ignored. A global shortcut *Copy text from a screen selection* (unbound by default) opens the overlay in a text mode where Enter copies text and no screenshot is saved.
- `glimply capture <target> --ocr` prints the recognized text (one `== <name> ==` block per image for groups); JSON adds `text` per file plus `texts` and `text`. Without a backend the capture is still saved and the command exits with 1.
- Text recognition uses the `tesseract` program when it is installed (image passed on stdin, no temporary files, 30 s timeout); glimply gains no build or runtime dependency. *Settings → General → Text recognition* shows whether it was found and lets you choose the languages (default: every installed language).

## [0.6.1] - 2026-11-24

### Fixed

- `glimply capture --out` with a relative path now saves next to the caller instead of in the tray daemon's working directory; the CLI resolves the path before sending it over D-Bus.
- Interval captures with *Only save when changed* on a group no longer stop with an error while a member is skipped (e.g. its monitor is unplugged) and the remaining members are unchanged.
- CLI and MCP captures of a group that fail on a later member still record the files already saved: they appear in the JSON `files` list (and as paths on stdout in text mode), become the last capture, and run the post-capture hook.
- Saving captures to filesystems without `RENAME_NOREPLACE` support (many FUSE mounts such as rclone or sshfs, older NFS/CIFS) no longer fails; glimply falls back to an exclusive name reservation before moving the file into place.

## [0.6.0] - 2026-11-24

Automation milestone: interval captures, scriptable CLI & async D-Bus API, post-capture hooks, and an opt-in local MCP server.

### Added

- Scheduled concurrent interval captures per area, monitor, all monitors, or group (`#22`) with per-target interval configuration (`Every N s`, optional `Only save when changed` raw pixel comparison preserving group frame simultaneity, optional `maxCaptures` and `maxMinutes` stop limits), dedicated global toggle shortcuts, active tray/Plasma-widget interval indicator (`io.github.m00sfett.glimply-tray-interval`) with `Start` / `Stop` / `Stop All` controls, and automatic low-disk (`< 100 MB`) / disconnect stop protection.
- Scriptable CLI subcommands (`capture`, `list`, `area`, `interval`, `overlay`) with `--format`, `--quality`, `--png-compression`, `--lossless`, `--delay`, `--cursor`/`--no-cursor`, `--out`, `--stdout` (raw binary stream), `--clipboard`, `--no-save`, `--json`, and structured exit codes (`0`/`1`/`2`/`3`) backed by an expanded async `io.github.m00sfett.glimply.Control` D-Bus API (`CaptureSync`, `ListMonitors`, `ListWindows`, `LastCaptureInfo`, `ResolveTarget`, `AddAreaRect`, `OpenOverlay`) (`#23`).
- Optional post-capture command hooks (`#24`) executed asynchronously without shell interpolation (`QProcess::splitCommand` + `%f` argument expansion + `GLIMPLY_FILE`, `GLIMPLY_FILES`, `GLIMPLY_FILE_COUNT`, `GLIMPLY_TARGET_ID`, `GLIMPLY_TARGET_NAME`, `GLIMPLY_TRIGGER_KIND`, `GLIMPLY_TIMESTAMP`, `GLIMPLY_FORMAT` environment variables), configurable on the *Automation* Settings page with per-target and per-group overrides, interval kill-switch (default off), timeout protection (`5–300 s`), non-blocking `Test Hook` check, and `[Event/hookFailed]` notifications.
- Local stdio-only MCP server (`glimply mcp`, disabled by default, zero network listeners) backed by async `McpDispatch` on `/Control`, configured under *Configure glimply… → Automation → Local MCP Server* with three security modes (`Allowlist`, `Confirm every capture` via native Breeze `McpConfirmDialog` with 30 s auto-deny timeout, and `Full session access`), mandatory tray activity indicator, optional low-urgency `[Event/mcpCapture]` notifications, 10 tools (`list_targets`, `list_monitors`, `list_windows`, `capture_target`, `capture_region`, `capture_window`, `get_last_capture`, `start_interval`, `stop_interval`, `annotate_image`) returning JSON metadata by default with optional Base64 `include_image` + `max_dimension`, and shared `AnnotationDocument`/`renderAnnotatedCrop` programmatic callouts and irreversible pixel redactions (`#25`).

## [0.5.0] - 2026-11-24

Multi-format image export, non-blocking capture delay timer, cross-monitor selections and areas, and a native pixel magnifier loupe + color picker.

### Added

- Multi-format image export (`PNG`, `JPEG`, `WebP`, and runtime-detected `AVIF` / `JPEG XL`) with configurable compression/quality, `#000000` alpha flattening for non-alpha formats, `{ext}` / extension-aligned filename templates, Target > Group > Global format overrides in Settings, and a compact KDE/Breeze `Save As…` export dialog with async live file-size estimation (`#18`).
- Non-blocking capture delay timer (`Off`, `3 s`, `5 s`, `10 s`, `Custom…`) with unobtrusive KDE/Breeze `CountdownBanner` hidden cleanly before capture, One-Shot default reset + checkable `Keep delay active` toggle directly in the overlay, tray, and Plasma widget, overlay `Retake` with countdown, and per-area / per-group `Delay:` selectors inside `Save as Area` and Settings (`#19`).
- Cross-monitor selections and persistent cross-monitor areas across virtual desktop coordinates with `8 px` magnetic monitor-edge snapping, byte-identical single-monitor fast path, mixed-DPI composition across intersected monitors only, and graceful partial-visibility capture with brief passive notification when an involved monitor is disconnected (`#20`).
- Precision native `PixelLoupe` with nearest-neighbor sampling across `1.0`–`2.0` DPI scales (`11 × 11` Frozen selection drag loupe toggleable in *Settings → General*; `15 × 15` eyedropper via `I` / *Pick Color* in `CaptureOverlay` and `QuickEditorWindow`, plus standalone `Pick Color` from the Plasma widget, tray menu, D-Bus `PickColor`, and optional global shortcut), `Tab` cycling of `#RRGGBB` / `rgb()` / `hsl()` / `hsv()`, and `Alt+Click` to apply sampled color to the active annotation tool (`#21`).

## [0.4.0] - Unreleased

Markup, irreversible redaction, and the standalone Quick Editor window.

### Added

- Non-destructive vector annotation engine (`AnnotationDocument`, `AnnotationItem`, `AnnotationRenderer`, `AnnotationController`, `AnnotationCanvas`) with logical coordinate storage, Undo/Redo with edit coalescing, multi-rectangle export clipping, and native-DPI rasterization (`#12`).
- Core markup tools (`Arrow`, `Rectangle`, `Ellipse`, `Pen`, `Multiply Highlighter`, auto-numbering `Step Counter` with optional pointer and renumbering on delete, and unified `Spotlight / Focus` dim mask), `[Annotations]` settings persistence in `TargetStore`, and shared KDE/Breeze `AnnotationToolbar` with progressive disclosure (`#13`).
- Rich text annotation tool (`Text`) with PhotoCraft/Photoshop inline editing (Point Text & Paragraph Text, `Escape` / `Ctrl+Enter` commit, exterior-margin drag move, double-click re-edit), HarfBuzz-safe `QTextLayout` rendering, and four legibility modes (`None`, `Shadow`, `Outline`, `Pill`) (`#14`).
- Irreversible pixel redaction tool (`Redact`, shortcut `X`) with `Solid Block` (opaque `#000000` default), reconstruction-hardened `Pixelate` (12 px logical floor, global cell grid, 16-level RGB quantization + seeded luminance jitter), heavy `Blur` (14 px logical radius floor, 3-pass clamped box blur), outward HiDPI pixel rounding, and image metadata stripping (`#15`).
- Progressive-disclosure markup toolbar (`Annotate`) in `CaptureOverlay` with automatic `Live` → `Frozen` transition, multi-monitor logical-to-native annotation rasterization across crop rectangles and full monitors, and Print-snapshot redaction leak protection on `Copy`, `All Monitors`, and `Escape` (`#16`).
- Standalone native KDE/Breeze `QuickEditorWindow` with all four entry points (`CaptureOverlay` *Edit in Window* / `Ctrl+E` preserving live editable vector annotations, capture notification *Edit*, Plasma widget & SNI tray *Edit Last Capture* + D-Bus `EditLastCapture`, and CLI / file-open forwarding via `glimply <file>` / `glimply --edit <file>`), undoable `Crop` (`C`) command, PhotoCraft cursor-anchored zoom (`Ctrl+Wheel` / `Alt+Wheel`, `10 %–1600 %`, crisp pixels at `≥ 400 %`, pixel grid at `≥ 800 %`) and hand pan (`Space+Drag` / `Middle-Drag`), explicit `Save` (`Ctrl+S`), `Save Copy` (`Ctrl+Alt+S` -> `_edited.png`), `Save As…` (`Ctrl+Shift+S`), and `Copy` (`Ctrl+C`) semantics without unintended clipboard or disk side effects, plus dirty-close confirmation (`#17`).

### Changed

- Unified selection and markup modifiers across `CaptureOverlay`, `AreaSelector`, and all drawing tools to match PhotoCraft / Photoshop: `Shift` constrains to a square/circle or 15° angle snap, `Alt` draws/resizes around the centre (or duplicates annotations under `Select`), `Space` held mid-drag repositions in-progress selections/shapes live, and `Ctrl` temporarily switches markup tools to `Select / Move` (`#16`).

## [0.3.0] - Unreleased

Faster control of the overlay, named monitors and the roteson.de icon.

### Added

- Space switches between Frozen and Live in every overlay (except while typing a name).
- A right click on a rectangle deletes it; a hovered rectangle is outlined in red. A right click elsewhere does nothing.
- Help in the overlay's top bar (also F1): every mouse and key control at a glance. It is open on first use and remembers whether you closed it.
- Monitor names: rename a monitor inline in the widget, in the settings, or from the tray menu without the widget. The name belongs to the physical monitor, is used in the file names of monitor captures and as the start of new automatic area names. Existing areas keep their names; an empty name gives the connector name back.
- Footer in the settings window with version, licence, links to roteson.de and GitHub, and the roteson.de and R²W² brand marks.

### Changed

- The overlay starts in the mode you used last. The setting *Overlay starts* offers Last used, Frozen or Live; a switched-off *Start frozen* becomes Live.
- Every monitor gets its own top bar, so *This Monitor* always means the monitor you click on.
- Modifiers mean the same with mouse and arrow keys: Shift steps by 10, Alt works around the centre, Ctrl keeps a square. With the mouse, Alt now grows from the centre and Ctrl makes a square (swapped). Arrow keys with Alt resize around the centre, with Ctrl as a square; Shift+arrow no longer resizes one edge.
- New icons in the roteson.de colours: app icon with the dark-red ring, tray and widget icon as a red tile with yellow corners that flashes yellow after a capture.

## [0.2.0] - Unreleased

glimply becomes the Print key tool: one key press saves the screen, an optional overlay narrows it down.

### Added

- Print key: glimply takes Print over from its previous owner (usually Spectacle) on first start. Pressing it saves all monitors as one PNG and copies the picture to the clipboard, then opens the capture overlay. Settings: use the Print key (turning it off gives Print back exactly as it was), open the overlay after saving, start frozen, save as soon as the mouse is released.
- Capture overlay with a top bar of labelled buttons (Frozen/Live, This Monitor, All Monitors, Window, Areas, Settings, Close) and a hint line that explains the next step.
- Several rectangles at once, on any monitor and overlapping. Each has a number and a delete button; only the active one has handles. A click on a rectangle selects it, a drag outside the active one draws another, *Add Rectangle* forces the next drag to draw a new one.
- Action bar at the active rectangle: Save (one PNG per rectangle, same timestamp, numbered), Copy, Save As…, Save as Area, Add Rectangle, Cancel. Saving replaces the full screenshot from the key press.
- Save as Area right in the overlay, with name and optional shortcut. Several rectangles can be combined into a group with one shortcut that captures all of them at the same moment.
- Window capture from the top bar: a list of open windows, captured on their own by KWin, also when covered or minimized. Hovering a window or a saved area outlines it on screen.
- Live mode: the overlay stays see-through and lighter, so a video keeps playing; captures happen when you save.
- Notification after a capture with Open and Show in Folder.
- `glimply --release-print-key`, used by `scripts/install.sh --uninstall` to hand Print back.

### Changed

- *Add Area* in the widget and the tray opens the capture overlay instead of the settings window.

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
