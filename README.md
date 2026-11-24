# glimply

> **Note:** `glimply` is currently a working name while in pre-release development (`0.8.0`, targeting `1.0.0`). There are no tagged releases yet; screenshots will be added with `1.0.0`.

**glimply** is a lightweight screenshot and annotation utility for KDE Plasma on Wayland (`org.kde.KWin.ScreenShot2`). It combines repeatable, shortcut-bound fixed areas with an interactive capture overlay, non-destructive vector annotations, floating screen pins, interval captures, local OCR, a Plasma panel widget, a scriptable CLI, and an optional local MCP server.

---

## Features

- **Fixed areas, groups, monitors, and windows:** Save named rectangular screen areas (single-monitor or spanning multiple monitors), group multiple targets, or capture an individual monitor, all monitors, or an open window via global KDE shortcuts (`KGlobalAccel`).
- **Interactive capture overlay (`Print`):** Capture in frozen or live mode, adjust selections with a pixel loupe and color picker, trigger saved areas directly, or annotate before saving/copying.
- **Non-destructive annotations & Quick Editor:** Annotate inside the capture overlay or open images standalone via `glimply -e <file>` (`glimply <files...>`). Available tools: `Select`, `Crop`, `Arrow`, `Rectangle`, `Ellipse`, `Pen`, `Highlighter`, `Text`, numbered `Step` counters, `Redact` (*Solid*, *Pixelate*, *Blur*), and `Spotlight` (*Rectangle*, *Ellipse*).
- **Pinned floating captures:** Keep screenshots floating always-on-top on your desktop. Pinned windows stay hidden from subsequent KWin captures via a KWin window script and can be restored across restarts.
- **Countdowns, intervals, history, and hooks:** Pre-capture countdown timers, timed interval captures (`--seconds`, `--max-captures`, `--max-minutes`, `--only-changed`), recent capture history (`glimply history`), desktop notifications, tray controls, and optional post-capture hooks.
- **Plasma widget (`plasmoid`):** Displays recent captures and triggers configured areas, groups, and monitors directly from KDE Plasma (follows your Plasma UI language).
- **Local OCR (`tesseract`):** Extract text from screenshots via *Copy Text* or `glimply capture --ocr`.
- **Export formats:** PNG, JPEG, WebP, AVIF, and JXL (WebP/AVIF/JXL require the corresponding Qt image-format plugins).
- **CLI & local stdio MCP server:** Scriptable JSON/stdout CLI (`capture`, `list`, `area`, `interval`, `overlay`, `history`, `mcp`) and an opt-in local JSON-RPC 2.0 stdio MCP server (`glimply mcp`).
- **19 UI languages:** English (base/fallback), Arabic, Chinese (Simplified & Traditional), Dutch, French, German, Hindi, Indonesian, Italian, Japanese, Korean, Polish, Portuguese, Russian, Spanish, Turkish, Ukrainian, and Vietnamese (*Settings -> General -> Language*).

---

## Privacy & Local Processing

glimply runs strictly locally on your machine:

- **No network communication:** Qt's network stack is not linked, and glimply makes no network requests (no telemetry, no crash uploads, no update checks).
- **Local storage & processing:** Screenshots, annotations, capture history, and configuration (`glimplyrc`) are stored locally. OCR runs locally using the installed `tesseract` binary.

---

## Permissions & KDE Plasma Integration

- **Screen capture (`org.kde.KWin.ScreenShot2`):** KWin only permits `ScreenShot2` requests from binaries launched via an installed `.desktop` file declaring `X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2` whose `Exec=` matches the running executable. Install glimply with a matching prefix (or use `scripts/install.sh` for `~/.local`) and run the installed binary rather than running straight from a temporary build directory. When Autostart is enabled in settings, glimply keeps the autostart `.desktop` entry pointing at the active binary.
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
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
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
glimply -e <file>
glimply mcp
glimply --release-print-key
```

Run `glimply <subcommand> --help` for full options.

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
