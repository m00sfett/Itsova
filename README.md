# Itsova

> **Beta:** Itsova is in early development. Features and shortcuts may change, and bugs are expected.

**Itsova** is a lightweight screenshot and annotation utility for KDE Plasma on Wayland (`org.kde.KWin.ScreenShot2`). It combines repeatable, shortcut-bound fixed areas with an interactive capture overlay, screen recording, non-destructive vector annotations, floating screen pins, interval captures, local OCR, a Plasma panel widget, a scriptable CLI, and an optional local MCP server. Because taking a screenshot is sometimes not enough.

**Itsova** stands for **Itsova Takes Screenshots Or Videos, Actually**.

---

## Features

- **Fixed areas, groups, monitors, and windows:** Save named rectangular screen areas (single-monitor or spanning multiple monitors), group multiple targets, or capture an individual monitor, all monitors, or an open window via global KDE shortcuts (`KGlobalAccel`).
- **Interactive capture overlay (`Print`):** Capture in frozen or live mode, adjust selections with a pixel loupe and color picker, trigger saved areas directly, or annotate before saving/copying.
- **Screen recording:** Record a monitor, all monitors, a saved area, a drawn rectangle, or a single window to MP4 (H.264), GIF, or WebM (VP9) from the overlay (`Ctrl+R`), the widget, or a global start/stop shortcut, with pause, a recording indicator in the tray, and short hints for start, pause, stop and errors that stay out of the video. Recordings keep each monitor's native resolution; video-only for now (optional audio recording is planned for a near-term `0.x` release, [#144](https://github.com/m00sfett/Itsova/issues/144)). A window recording contains only the window's content: KWin sends a new frame only when the window changes, so while the window is moved or stays still the video holds the last picture.
- **Non-destructive annotations & Quick Editor:** Draw on captures with *Markup* inside the capture overlay or open images standalone via `itsova -e <file>` (`itsova <files...>`). Available tools: `Select`, `Crop`, `Arrow`, `Rectangle`, `Ellipse`, `Pen`, `Highlighter`, `Text`, numbered `Step` counters, `Redact` (*Solid*, *Pixelate*, *Blur*), and `Spotlight` (*Rectangle*, *Ellipse*).
- **Pinned floating captures:** Keep screenshots floating always-on-top on your desktop. While a capture runs, pins paint themselves fully transparent, so they do not appear in screenshots; KWin's `excludeFromCapture` property did not achieve this in testing. A small KWin window script keeps pins above other windows and places them, and they can be restored across restarts.
- **Countdowns, intervals, history, and hooks:** Pre-capture countdown timers, timed interval captures (`--seconds`, `--max-captures`, `--max-minutes`, `--only-changed`), recent capture history (`itsova history`), desktop notifications, tray controls, and optional post-capture hooks.
- **Plasma widget (`plasmoid`):** Displays recent captures and triggers configured areas, groups, and monitors directly from KDE Plasma (follows your Plasma UI language).
- **Local OCR (`tesseract`):** Extract text from screenshots via *Copy Text* or `itsova capture --ocr`.
- **Export formats:** PNG, JPEG, WebP, AVIF, and JXL (WebP/AVIF/JXL require the corresponding Qt image-format plugins).
- **CLI & local stdio MCP server:** Scriptable JSON/stdout CLI (`capture`, `list`, `area`, `interval`, `overlay`, `history`, `mcp`) and an opt-in local JSON-RPC 2.0 stdio MCP server (`itsova mcp`).
- **19 UI languages:** English (base/fallback), Arabic, Chinese (Simplified & Traditional), Dutch, French, German, Hindi, Indonesian, Italian, Japanese, Korean, Polish, Portuguese, Russian, Spanish, Turkish, Ukrainian, and Vietnamese (*Settings -> General -> Language*).

---

## The Capture Overlay

`Print` captures every monitor into memory and opens the overlay on top, frozen (the moment you pressed the key) or live (`Space` switches) — nothing is written to disk or copied to the clipboard until you save, copy, pin, or edit (an opt-in *Save full screenshot immediately on Print* toggle is available in *Settings → General*). Direct fixed-area, monitor, and window shortcuts continue saving immediately to disk. One panel shows the actions of the current state: at the start *This Monitor*, *Monitors* (each monitor and *All Monitors*), *Windows*, *Areas*, *Record* (the same targets as a video), *Delay*, *Markup* and *More*; with a selection *Save*, *Copy*, *Pin*, *Copy Text*, *Record*, *Markup* and *More*. Buttons carry no labels: hovering shows the title, and after half a second a tooltip adds the shortcut. Windows, saved areas, the delay, the markup tools and less frequent actions open as drawers inside the panel. Drag the panel by its grip to move it, also to another monitor; it remembers its place per monitor. `F1` shows every title and key at once, together with the modifier keys. Without a selection the arrow keys move through the panel and its menus (Enter opens or runs, Escape steps back). Before anything is drawn, the window under the pointer (or the monitor, over the desktop or a full-screen window) is framed, and a click takes it.

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

Itsova runs strictly locally on your machine:

- **No network communication:** Qt's network stack is not linked, and Itsova makes no network requests (no telemetry, no crash uploads, no update checks).
- **Local storage & processing:** Screenshots, annotations, capture history, and configuration (`itsovarc`) are stored locally. OCR runs locally using the installed `tesseract` binary.
- **Redaction behavior:** *Solid* redaction overwrites the covered pixels with an opaque flat color so the original pixels are not present in the saved image — always use **Solid** for passwords, tokens, or sensitive text. *Pixelate* and *Blur* are visual obfuscation filters; neither has undergone an independent cryptographic or forensic reconstruction audit.
- **Security review status:** Itsova has not yet undergone an independent external security audit. A pre-`1.0.0` security review covering capture boundaries, redaction, file permissions, hooks, and the MCP server is tracked in [`docs/security-review-checklist.md`](docs/security-review-checklist.md) ([#143](https://github.com/m00sfett/Itsova/issues/143)).

---

## Permissions & KDE Plasma Integration

- **Screen capture (`org.kde.KWin.ScreenShot2`):** Itsova installs a desktop file that declares `X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2` and points `Exec=` at the installed binary. This is the access model KWin documents: KWin should only accept `ScreenShot2` requests from programs started through such a file. On the development system, KWin did not enforce it. Test programs without a matching file, including a plain Python script, received captures without a prompt, and the cause is not yet known. Do not treat this declaration as a security boundary against other programs in your session. Install Itsova with a matching prefix (or use `scripts/install.sh` for `~/.local`) and run the installed binary rather than running straight from a temporary build directory. When Autostart is enabled in settings, Itsova keeps the autostart `.desktop` entry pointing at the active binary.
- **Screen recording (`zkde_screencast_unstable_v1`):** KWin grants its screencast protocol through the same desktop file (`X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1`); KPipeWire encodes the stream. Because this is an unstable KDE Wayland protocol, upstream protocol changes and successor protocols are tracked in [#140](https://github.com/m00sfett/Itsova/issues/140). After a recording, Itsova runs `ffprobe`/`ffmpeg` once on the file to hold the last frame until *Stop* (container timing only). Video-only in the current beta; optional audio recording is planned for a near-term `0.x` release ([#144](https://github.com/m00sfett/Itsova/issues/144)).
- **Global `Print` key shortcut:** Itsova can register `Print` through KDE GlobalAccel to launch its capture overlay. To release the `Print` binding cleanly (for example before switching back to Spectacle or uninstalling), run:
  ```bash
  itsova --release-print-key
  ```
- **Local MCP server (disabled by default):** `itsova mcp` exposes 10 local stdio tools (`list_targets`, `list_monitors`, `list_windows`, `capture_target`, `capture_region`, `capture_window`, `get_last_capture`, `start_interval`, `stop_interval`, `annotate_image`) and denies all calls until explicitly enabled under **Settings -> Automation -> Local MCP Server**. Access scopes:
  - `Allowlist` — only pre-approved saved targets (`list_windows`, `capture_region`, and `capture_window` are blocked).
  - `ConfirmPerCapture` — asks the user via dialog before captures, interval starts, and annotations (showing input, resolved output path, and overwrite warnings).
  - `FullSession` — allows all MCP tools without per-call confirmation for the configured session scope.
  - Across all MCP modes, `annotate_image` writes are confined to Itsova's configured capture directory, refuse symlinks, and refuse overwriting any existing file unless `overwrite=true` is passed explicitly.
  - **Session D-Bus boundary:** MCP access modes govern the MCP interface (`itsova mcp`) only. Local session D-Bus methods on `io.github.m00sfett.itsova.Control` (such as `CaptureSync`, used by the CLI and Plasma widget) are intentionally separate and follow the user's local D-Bus session boundary.
- **Optional OCR (`tesseract`):** Only executed locally when *Copy Text* or `itsova capture --ocr` is invoked; requires `tesseract` and the matching language data (such as `tesseract-data-eng`).

---

## Beta limitations

- Resizing a window during recording can distort the picture ([#127](https://github.com/m00sfett/Itsova/issues/127)).
- Screen capture and recording require KDE Plasma on Wayland and KWin; other desktops are not supported.
- Some desktop configurations, including multiple monitors and fractional scaling, need further hands-on testing.
- Recording pause is unavailable with KPipeWire 6.3, including Debian 13.
- Translations are machine-generated and may contain errors.

## Requirements

Minimum required toolchain:

- **CMake** >= `3.25`, **Ninja**, C++ compiler
- **Qt 6** >= `6.8` (`qt6-base`, `qt6-tools` / `linguist` tools)
- **KDE Frameworks 6** >= `6.13` (`KConfig`, `KDBusAddons`, `KGlobalAccel`, `KI18n`, `KNotifications`, `KStatusNotifierItem`, `KWidgetsAddons`, `KXmlGui`)
- **Screen recording:** Qt Wayland (`qt6-wayland`, Debian `qt6-wayland-dev`), `plasma-wayland-protocols` (build), KPipeWire >= `6.3` (`kpipewire`, Debian `libkpipewire-dev`), FFmpeg (`ffmpeg`), and LayerShellQt for the recording hints (`layer-shell-qt`, Debian `liblayershellqtinterface-dev`)
- **Python 3** (build/test translation validation) & **gettext** (`msgfmt` for Plasma widget catalogs)
- **Runtime:** KDE Plasma Wayland (`kwin` with `ScreenShot2`), `hicolor-icon-theme`
- **Optional runtime:**
  - `qt6-imageformats` (Arch) / `qt6-image-formats-plugins` (Debian) for WebP and extra export formats
  - `tesseract` + traineddata packs (e.g. `tesseract-data-eng`) for OCR
  - `plasma-workspace` for the Plasma widget

---

## Installation

Itsova requires a **KDE Plasma Wayland session with KWin**. Select “Plasma (Wayland)”
at login if your display manager offers more than one session. The first beta ships
an Arch Linux package recipe and source code; it does not yet ship `.deb` or `.rpm`
downloads. Install only packages built for your distribution and version.

### Arch Linux (`PKGBUILD`)

Packaging files are provided in [`packaging/arch/`](packaging/arch/):

- **Development package (current `main`):** [`packaging/arch/itsova-git/PKGBUILD`](packaging/arch/itsova-git/PKGBUILD)
  ```bash
  cd packaging/arch/itsova-git
  makepkg -si
  ```
- **Beta release package:** Download the checksum-verified [`PKGBUILD` from the v0.12.0-beta.1 release](https://github.com/m00sfett/Itsova/releases/tag/v0.12.0-beta.1), then run `makepkg -si` in its directory. The template in this repository requires a checksum update for each release.

For example, after downloading the release's `PKGBUILD` into an empty directory:

```bash
cd /path/to/download-directory
makepkg -si
```

After installation, start **Itsova** from the application menu. To remove the
package, first run `itsova --release-print-key`, then `sudo pacman -R itsova`
(or `sudo pacman -R itsova-git` for the development package).

### Debian and Ubuntu

Debian 13 provides the required toolchain. Ubuntu 24.04's standard Qt 6.4 is too
old; Ubuntu 26.04 has newer Qt and KDE Frameworks packages but has not been
tested with Itsova. A native packaging recipe is provided in
[`packaging/debian/`](packaging/debian/); build it on the target distribution
and version rather than assuming a package built elsewhere is compatible.

On Debian 13, install the build tools and dependencies and build from a checkout:

```bash
sudo apt update
sudo apt install git build-essential debhelper cmake ninja-build python3 gettext ffmpeg \
  qt6-base-dev qt6-tools-dev qt6-tools-dev-tools qt6-l10n-tools \
  qt6-wayland-dev qt6-wayland-dev-tools plasma-wayland-protocols \
  libkf6config-dev libkf6globalaccel-dev libkf6statusnotifieritem-dev \
  libkf6notifications-dev libkf6dbusaddons-dev libkf6widgetsaddons-dev \
  libkf6xmlgui-dev libkf6i18n-dev libkpipewire-dev liblayershellqtinterface-dev \
  qt6-image-formats-plugins
git clone https://github.com/m00sfett/Itsova.git
cd Itsova
cp -a packaging/debian ./debian
dpkg-buildpackage -us -uc -b
sudo apt install ../itsova_*.deb
```

Then launch Itsova from the application menu. To uninstall, run
`itsova --release-print-key` and `sudo apt remove itsova`. Settings and captures
are kept. Package building on Ubuntu and installation in a Plasma session still
need testing.

### openSUSE

No RPM package is available yet. A build from source needs the versions listed
under Requirements; availability differs between openSUSE releases. RPM packaging
and installation have not yet been tested.

### Build from source

Install the dependencies listed under Requirements, obtain the source from the
[beta release](https://github.com/m00sfett/Itsova/releases/tag/v0.12.0-beta.1)
or clone the repository, and run the following in its root directory:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
ctest --test-dir build --output-on-failure -LE nested
sudo cmake --install build
```

Alternatively, for a user installation under `~/.local`, run the script below
instead of the system-wide commands above. It builds and tests the app, refreshes
desktop caches, and may restart the Plasma shell to reload the widget:

```bash
./scripts/install.sh
```

`./scripts/install.sh` also accepts `--prefix DIR` and `--uninstall` (stops `itsova`, releases the `Print` shortcut via `--release-print-key`, and removes installed files while keeping `~/.config/itsovarc` and captures).

To verify compilation and tests against the minimum Qt 6.8 / KF 6.13 toolchain in a `debian:trixie` container (requires Docker):

```bash
./scripts/check-debian13.sh
```

### Tests

- **Offscreen tests** (`ctest -LE nested`): QTest on Qt's `offscreen` platform; CTest sets it for every test, so no window ever appears on the desktop. These run on every build and in `install.sh`.
- **Nested-KWin tests** (`ctest -L nested`): each test starts a virtual `kwin_wayland` on a private D-Bus session with a throw-away home, runs the freshly built `itsova` there and drives it through D-Bus and KWin's fake input (`itsova-fakekey`); screenshots and files land in `<build>/nested/<test>/`. The desktop session is never touched. They need `kwin_wayland`, `wayland-scanner`, `python-dbus`, ImageMagick and FFmpeg, and are skipped otherwise. Run one by hand with `tests/nested/session.sh --help`.
- **Coverage**: `scripts/coverage.sh [build-dir]` builds with `--coverage`, runs the offscreen tests and writes a `gcovr` report (reported, no gate).
- **Mutation testing**: `scripts/mutate.sh <src/file.cpp>...` runs [Mull](https://github.com/mull-project/mull) on the named files only and reports surviving mutants. It needs Mull built for the installed clang's LLVM version (`MULL_DIR`).
- Every change starts with a failing test (red), then the fix (green).

---

## CLI Quick Reference

```bash
itsova overlay [--mode frozen|live] [--delay N]
itsova capture <target|all|monitor:...|window:...> [--format png|jpeg|webp|avif|jxl] [--ocr] [--clipboard] [--stdout] [--no-save] [--json]
itsova list [--kind targets|monitors|windows] [--json]
itsova area add|edit|rename|remove ...
itsova interval start|stop|status ...
itsova history [--limit N] [--json]
itsova latency [--last N] [--json]
itsova logs [journalctl options]
itsova -e <file>
itsova mcp
itsova --verbose
itsova --release-print-key
```

Run `itsova <subcommand> --help` for full options.

### Logs

Itsova writes every capture, overlay session, recording start and stop (with who asked: tray, shortcut, D-Bus, quit, KWin) and every error to the user journal. `itsova logs` shows the last hour; pass `journalctl` options such as `-f`, `-n 200` or `--since today`. Detailed (debug) lines are off by default; turn them on in *Settings → General → Logging* or start the daemon with `itsova --verbose`. Logs never contain secrets, hook commands or file contents, only paths.

---

## Translations

English is the source language (`i18n/itsova_en.ts` provides English plural rules).

> **Note on translations:** The 18 non-English UI translations were generated via machine/AI translation and may contain awkward phrasing or terminology issues. Corrections and native-speaker pull requests are very welcome!

Developer helpers for catalogs:

- Terminology and non-translatable technical terms: [`i18n/glossary.md`](i18n/glossary.md)
- Refresh `.ts`/`.po` catalogs from source: [`scripts/update-translations.sh`](scripts/update-translations.sh) (`<build-dir>`)
- Validate placeholders, accelerators, plural forms, and completeness: [`scripts/check-translations.py`](scripts/check-translations.py)

---

## License

`itsova` is licensed under the **GNU Affero General Public License v3.0 or later** (`AGPL-3.0-or-later`) — see [`LICENSE`](LICENSE). Clean-room implementation: no source code is copied from Spectacle or other screenshot tools.
