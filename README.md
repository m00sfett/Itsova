# glimply

> **Note:** `glimply` is currently a working name while in pre-release development (`0.12.0`, targeting `1.0.0`). There are no tagged releases yet; screenshots will be added with `1.0.0`.

**glimply** is a lightweight screenshot and annotation utility for KDE Plasma on Wayland (`org.kde.KWin.ScreenShot2`). It combines repeatable, shortcut-bound fixed areas with an interactive capture overlay, screen recording, non-destructive vector annotations, floating screen pins, interval captures, local OCR, a Plasma panel widget, a scriptable CLI, and an optional local MCP server.

---

## Features

- **Fixed areas, groups, monitors, and windows:** Save named rectangular screen areas (single-monitor or spanning multiple monitors), group multiple targets, or capture an individual monitor, all monitors, or an open window via global KDE shortcuts (`KGlobalAccel`).
- **Interactive capture overlay (`Print`):** Capture in frozen or live mode, adjust selections with a pixel loupe and color picker, trigger saved areas directly, or annotate before saving/copying.
- **Screen recording:** Record a monitor, all monitors, a saved area, a drawn rectangle, or a single window to MP4 (H.264), GIF, or WebM (VP9) from the overlay (`Ctrl+R`), the widget, or a global start/stop shortcut, with pause and a recording indicator in the tray. Recordings keep each monitor's native resolution and have no sound. A window recording contains only the window's content: KWin sends a new frame only when the window changes, so while the window is moved or stays still the video holds the last picture.
- **Non-destructive annotations & Quick Editor:** Draw on captures with *Markup* inside the capture overlay or open images standalone via `glimply -e <file>` (`glimply <files...>`). Available tools: `Select`, `Crop`, `Arrow`, `Rectangle`, `Ellipse`, `Pen`, `Highlighter`, `Text`, numbered `Step` counters, `Redact` (*Solid*, *Pixelate*, *Blur*), and `Spotlight` (*Rectangle*, *Ellipse*).
- **Pinned floating captures:** Keep screenshots floating always-on-top on your desktop. While a capture runs, pins paint themselves fully transparent, so they do not appear in screenshots; KWin's `excludeFromCapture` property did not achieve this in testing. A small KWin window script keeps pins above other windows and places them, and they can be restored across restarts.
- **Countdowns, intervals, history, and hooks:** Pre-capture countdown timers, timed interval captures (`--seconds`, `--max-captures`, `--max-minutes`, `--only-changed`), recent capture history (`glimply history`), desktop notifications, tray controls, and optional post-capture hooks.
- **Plasma widget (`plasmoid`):** Displays recent captures and triggers configured areas, groups, and monitors directly from KDE Plasma (follows your Plasma UI language).
- **Local OCR (`tesseract`):** Extract text from screenshots via *Copy Text* or `glimply capture --ocr`.
- **Export formats:** PNG, JPEG, WebP, AVIF, and JXL (WebP/AVIF/JXL require the corresponding Qt image-format plugins).
- **CLI & local stdio MCP server:** Scriptable JSON/stdout CLI (`capture`, `list`, `area`, `interval`, `overlay`, `history`, `mcp`) and an opt-in local JSON-RPC 2.0 stdio MCP server (`glimply mcp`).
- **19 UI languages:** English (base/fallback), Arabic, Chinese (Simplified & Traditional), Dutch, French, German, Hindi, Indonesian, Italian, Japanese, Korean, Polish, Portuguese, Russian, Spanish, Turkish, Ukrainian, and Vietnamese (*Settings -> General -> Language*).

---

## The Capture Overlay

`Print` saves every monitor right away; then the overlay opens on top, frozen (the moment you pressed the key) or live (`Space` switches). One panel shows the actions of the current state: at the start *This Monitor*, *Monitors* (each monitor and *All Monitors*), *Windows*, *Areas*, *Record* (the same targets as a video), *Delay*, *Markup* and *More*; with a selection *Save*, *Copy*, *Pin*, *Copy Text*, *Record*, *Markup* and *More*. Buttons carry no labels: hovering shows the title, and after half a second a tooltip adds the shortcut. Windows, saved areas, the delay, the markup tools and less frequent actions open as drawers inside the panel. Drag the panel by its grip to move it, also to another monitor; it remembers its place per monitor. `F1` shows every title and key at once, together with the modifier keys. Without a selection the arrow keys move through the panel and its menus (Enter opens or runs, Escape steps back). Before anything is drawn, the window under the pointer (or the monitor, over the desktop or a full-screen window) is framed, and a click takes it.

| Key | Action |
|---|---|
| Drag | Draw a rectangle (`Shift` square, `Alt` from the centre, `Space` repositions) |
| `1` / `2` | Capture this monitor / all monitors |
| `W` / `G` / `D` | Window list / saved areas / delay (in a list, `1`–`9` pick an entry) |
| `V A R E B M T N X S` | Markup tools: Select, Arrow, Rectangle, Ellipse, Pen, Marker, Text, Step, Redact, Spotlight |
| `Enter` / `Ctrl+C` / `P` / `O` | Save / copy / pin / copy text of the selection |
| `Ctrl+Shift+S` / `Ctrl+B` | Save as… / save as area |
| `I` | Pick a colour (`Tab` cycles the format) |
| `Ctrl+E` | Open in the editor window |
| `Ctrl+R` | Record the selection as a video; without one, choose what to record |
| `F10` / `Ctrl+,` | More actions / settings |
| `Tab`, arrows | Move through the panel; `Enter` or `Space` presses the focused button |
| `F1` | Overview of all buttons, keys and modifiers |
| `Escape` | Step back, or close (the full screenshot stays saved) |

---

## Privacy & Local Processing

glimply runs strictly locally on your machine:

- **No network communication:** Qt's network stack is not linked, and glimply makes no network requests (no telemetry, no crash uploads, no update checks).
- **Local storage & processing:** Screenshots, annotations, capture history, and configuration (`glimplyrc`) are stored locally. OCR runs locally using the installed `tesseract` binary.

---

## Permissions & KDE Plasma Integration

- **Screen capture (`org.kde.KWin.ScreenShot2`):** glimply installs a desktop file that declares `X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2` and points `Exec=` at the installed binary. This is the access model KWin documents: KWin should only accept `ScreenShot2` requests from programs started through such a file. On the development system, KWin did not enforce it. Test programs without a matching file, including a plain Python script, received captures without a prompt, and the cause is not yet known. Do not treat this declaration as a security boundary against other programs in your session. Install glimply with a matching prefix (or use `scripts/install.sh` for `~/.local`) and run the installed binary rather than running straight from a temporary build directory. When Autostart is enabled in settings, glimply keeps the autostart `.desktop` entry pointing at the active binary.
- **Screen recording (`zkde_screencast_unstable_v1`):** KWin grants its screencast protocol through the same desktop file (`X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1`); KPipeWire encodes the stream. After a recording, glimply runs `ffprobe`/`ffmpeg` once on the file to hold the last frame until *Stop* (container timing only).
- **Global `Print` key shortcut:** glimply can register `Print` through KDE GlobalAccel to launch its capture overlay. To release the `Print` binding cleanly (for example before switching back to Spectacle or uninstalling), run:
  ```bash
  glimply --release-print-key
  ```
- **Local MCP server (disabled by default):** `glimply mcp` exposes 10 local stdio tools (`list_targets`, `list_monitors`, `list_windows`, `capture_target`, `capture_region`, `capture_window`, `get_last_capture`, `start_interval`, `stop_interval`, `annotate_image`) and denies all calls until explicitly enabled under **Settings -> Automation -> Local MCP Server**. Access scopes:
  - `Allowlist` — only pre-approved saved targets (`list_windows`, `capture_region`, and `capture_window` are blocked).
  - `ConfirmPerCapture` — asks the user via dialog before captures, interval starts, and annotations.
  - `FullSession` — allows all MCP tools without per-call confirmation for the configured session scope.
- **Optional OCR (`tesseract`):** Only executed locally when *Copy Text* or `glimply capture --ocr` is invoked; requires `tesseract` and the matching language data (such as `tesseract-data-eng`).

---

## Requirements

Minimum supported toolchain (verified in Debian 13 `trixie` via [`scripts/check-debian13.sh`](scripts/check-debian13.sh)):

- **CMake** >= `3.25` (verified on Debian 13 `trixie` with CMake `3.31`, Qt `6.8`, KF6 `6.13`), **Ninja**, C++ compiler
- **Qt 6** >= `6.8` (`qt6-base`, `qt6-tools` / `linguist` tools)
- **KDE Frameworks 6** >= `6.13` (`KConfig`, `KDBusAddons`, `KGlobalAccel`, `KI18n`, `KNotifications`, `KStatusNotifierItem`, `KWidgetsAddons`, `KXmlGui`)
- **Screen recording:** Qt Wayland (`qt6-wayland`, Debian `qt6-wayland-dev`), `plasma-wayland-protocols` (build), KPipeWire >= `6.3` (`kpipewire`, Debian `libkpipewire-dev`), and FFmpeg (`ffmpeg`)
- **Python 3** (build/test translation validation) & **gettext** (`msgfmt` for Plasma widget catalogs)
- **Runtime:** KDE Plasma Wayland (`kwin` with `ScreenShot2`), `hicolor-icon-theme`
- **Optional runtime:**
  - `qt6-imageformats` (Arch) / `qt6-image-formats-plugins` (Debian) for WebP and extra export formats
  - `tesseract` + traineddata packs (e.g. `tesseract-data-eng`) for OCR
  - `plasma-workspace` for the Plasma widget

---

## Installation

### Arch Linux (`PKGBUILD`)

Packaging files are provided in [`packaging/arch/`](packaging/arch/):

- **Development package (current `main`):** [`packaging/arch/glimply-git/PKGBUILD`](packaging/arch/glimply-git/PKGBUILD)
  ```bash
  cd packaging/arch/glimply-git
  makepkg -si
  ```
- **Release template (`1.0.0`+ once tagged):** [`packaging/arch/glimply/PKGBUILD`](packaging/arch/glimply/PKGBUILD) (no release tags exist prior to `1.0.0`).

### Build from Source

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
ctest --test-dir build --output-on-failure -LE nested
sudo cmake --install build
```

For a rootless user installation under `~/.local` (also refreshes KDE desktop/icon caches and reloads the Plasma shell if needed), run:

```bash
./scripts/install.sh
```

`./scripts/install.sh` also accepts `--prefix DIR` and `--uninstall` (stops `glimply`, releases the `Print` shortcut via `--release-print-key`, and removes installed files while keeping `~/.config/glimplyrc` and captures).

To verify compilation and tests against the minimum Qt 6.8 / KF 6.13 toolchain in a `debian:trixie` container (requires Docker):

```bash
./scripts/check-debian13.sh
```

### Tests

- **Offscreen tests** (`ctest -LE nested`): QTest on Qt's `offscreen` platform; CTest sets it for every test, so no window ever appears on the desktop. These run on every build and in `install.sh`.
- **Nested-KWin tests** (`ctest -L nested`): each test starts a virtual `kwin_wayland` on a private D-Bus session with a throw-away home, runs the freshly built `glimply` there and drives it through D-Bus and KWin's fake input (`glimply-fakekey`); screenshots and files land in `<build>/nested/<test>/`. The desktop session is never touched. They need `kwin_wayland`, `wayland-scanner`, `python-dbus`, ImageMagick and FFmpeg, and are skipped otherwise. Run one by hand with `tests/nested/session.sh --help`.
- **Coverage**: `scripts/coverage.sh [build-dir]` builds with `--coverage`, runs the offscreen tests and writes a `gcovr` report (reported, no gate).
- **Mutation testing**: `scripts/mutate.sh <src/file.cpp>...` runs [Mull](https://github.com/mull-project/mull) on the named files only and reports surviving mutants. It needs Mull built for the installed clang's LLVM version (`MULL_DIR`).
- Every change starts with a failing test (red), then the fix (green).

---

## CLI Quick Reference

```bash
glimply overlay [--mode frozen|live] [--delay N]
glimply capture <target|all|monitor:...|window:...> [--format png|jpeg|webp|avif|jxl] [--ocr] [--clipboard] [--stdout] [--no-save] [--json]
glimply list [--kind targets|monitors|windows] [--json]
glimply area add|edit|rename|remove ...
glimply interval start|stop|status ...
glimply history [--limit N] [--json]
glimply latency [--last N] [--json]
glimply logs [journalctl options]
glimply -e <file>
glimply mcp
glimply --verbose
glimply --release-print-key
```

Run `glimply <subcommand> --help` for full options.

### Logs

glimply writes every capture, overlay session, recording start and stop (with who asked: tray, shortcut, D-Bus, quit, KWin) and every error to the user journal. `glimply logs` shows the last hour; pass `journalctl` options such as `-f`, `-n 200` or `--since today`. Detailed (debug) lines are off by default; turn them on in *Settings → General → Logging* or start the daemon with `glimply --verbose`. Logs never contain secrets, hook commands or file contents, only paths.

---

## Translations

English is the source language (`i18n/glimply_en.ts` provides English plural rules).

> **Note on translations:** The 18 non-English UI translations were generated via machine/AI translation and may contain awkward phrasing or terminology issues. Corrections and native-speaker pull requests are very welcome!

Developer helpers for catalogs:

- Terminology and non-translatable technical terms: [`i18n/glossary.md`](i18n/glossary.md)
- Refresh `.ts`/`.po` catalogs from source: [`scripts/update-translations.sh`](scripts/update-translations.sh) (`<build-dir>`)
- Validate placeholders, accelerators, plural forms, and completeness: [`scripts/check-translations.py`](scripts/check-translations.py)

---

## License

`glimply` is licensed under the **GNU Affero General Public License v3.0 or later** (`AGPL-3.0-or-later`) — see [`LICENSE`](LICENSE). Clean-room implementation: no source code is copied from Spectacle or other screenshot tools.
