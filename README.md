# glimply

glimply is a KDE Plasma tray application for screenshots. The Print key saves
all monitors instantly; an overlay then lets you cut the picture down to
rectangles, one monitor or one window. Fixed areas, monitors and groups can be
captured the moment a global shortcut is pressed, without any overlay.

The project is in active development (0.5.x) and targets KDE Plasma 6 on Wayland.

## Features

- **Print key** replacement: Print saves all monitors immediately and copies the
  picture to the clipboard, then opens the capture overlay. glimply takes the
  key over from Spectacle on first start and gives it back exactly when you turn
  the option off or uninstall.
- **Capture overlay** with labelled buttons on every monitor: Frozen or Live,
  This Monitor, All Monitors, Window, Areas, Delay, Annotate, Pick Color, Help.
  Draw several rectangles across one or multiple monitors (`8 px` magnetic edge
  snapping), save them all at once as numbered files, copy one, or save them as
  areas or as a group with one shortcut. Escape keeps the full screenshot.
  **Space** switches between Frozen and Live, **F1** shows all controls, a right
  click deletes a rectangle.
- **Multi-format export & live size estimate**: PNG by default plus JPEG (transparent
  regions flatten cleanly to `#000000`), WebP (lossless or lossy), and
  runtime-detected AVIF / JPEG XL. *Configure glimply…* supports Target > Group >
  Global overrides; **Save As…** opens a compact Breeze dialog with live async
  file-size preview.
- **Capture delay timer**: non-blocking countdown banner (`Off`, `3 s`, `5 s`,
  `10 s`, or custom `1–60 s`) usable pre-capture from the widget, tray, or
  per-area/group delays, or inside the overlay via **Delay** retake. One-Shot by
  default unless **Keep delay active** is toggled on.
- **Windows** captured on their own, also when covered or minimized.
- **Areas** on one monitor (`<monitor>-<n>`) or spanning multiple monitors
  (mixed-DPI composition at highest involved scale, byte-identical fast path
  for single-monitor areas, and graceful partial-visibility capture + passive
  notice if an involved monitor is disconnected).
- **Pixel loupe & color picker**: an `11 × 11` nearest-neighbor magnifier while
  dragging or resizing frozen selections (toggleable under *General*), plus a
  `15 × 15` eyedropper (`I` / **Pick Color** in overlay & Quick Editor, or
  standalone from the widget, tray, global shortcut, and D-Bus `PickColor`).
  **Tab** cycles `#RRGGBB` / `rgb()` / `hsl()` / `hsv()`; **Alt+Click** sets the
  active annotation tool color.
- **Monitor names**: give a monitor a name such as `Left` in the widget, the
  settings or the tray menu. The name sticks to the physical monitor, appears
  in file names of monitor captures and starts the names of new areas.
- **Monitors and All monitors** are always available. *All monitors* is one
  image in the current arrangement at the highest monitor scale; gaps between
  monitors are transparent.
- **Groups** capture several targets with one shortcut. Each monitor is
  captured once per trigger, so all files show the same moment and share one
  timestamp.
- **Global shortcuts** for every target and group, registered with KDE's global
  shortcut service (also editable in System Settings). There are no default
  shortcuts.
- **Plasma widget** in the system tray: every area, group and monitor in one
  list with its shortcut and buttons to capture, edit the area on screen,
  rename and delete (inline, without dialogs), plus *Delay*, *Pick Color*,
  *Add Area*, *Open Last Capture*, *Edit Last Capture*, and *Open Capture Folder*.
  Without the widget, glimply shows a classic tray icon with a menu instead.
- **Feedback** without interruptions: the tray icon blinks once after a
  capture. Errors, such as a missing monitor, raise a notification.
- Captures are saved in `~/Pictures/glimply/` (your XDG pictures folder) as
  `{date}_{time}_{area}.{ext}` (PNG by default). Existing files are never
  overwritten.

## Build requirements

- CMake 3.25 or later and a C++20 compiler
- Qt 6.12 or later: Widgets, DBus, Concurrent (and Test for the tests)
- KDE Frameworks 6: Config, DBusAddons, GlobalAccel, I18n, Notifications,
  StatusNotifierItem, WidgetsAddons, XmlGui

## Install

```sh
scripts/install.sh
```

The script builds a Release version, runs the tests and installs into
`~/.local` (binary, desktop entry, icon, notification events). Use
`--prefix DIR` for another prefix and `--uninstall` to remove the installed
files again; settings and captures are kept.

The script also installs the Plasma widget and a D-Bus activation file, starts
glimply, and reloads the Plasma shell once when the widget is new or changed,
so the widget shows up in the system tray right away. Nothing needs to be
configured by hand.

On first start glimply adds itself to autostart. You can turn that off under
*Configure glimply… → General*.

### Screen capture permission

glimply uses KWin's `org.kde.KWin.ScreenShot2` interface. KWin grants access to
applications whose desktop entry declares
`X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2` and whose `Exec`
points to the running executable. The installed desktop entry does this. If
captures fail with a permission error, reinstall with `scripts/install.sh`.

## Usage

Press **Print**. The full screenshot is saved and in the clipboard. In the
overlay, drag a rectangle and press **Enter** (or *Save*) to keep only that
part, or click a monitor to keep only that monitor. Draw more rectangles to
save several parts at once. *Save as Area* remembers the rectangles for later.
In **Live** mode the screen keeps moving and the capture happens when you
save; Space switches between both, and the next overlay starts in the mode you
used last (configurable). Escape keeps the full screenshot. Help (F1) lists all
controls.

Controls in the overlay (aligned with PhotoCraft and Photoshop):

| | Mouse (draw, resize) | Arrow keys |
|---|---|---|
| — | free | move by 1 px |
| Shift | perfect square / circle (or 15° angle snap) | steps of 10 |
| Alt | grow from the centre (or duplicate annotation) | resize around the centre (both sides) |
| Shift+Alt | square / circle from the centre | centered resize ×10 |
| Space *(mid-drag)* | reposition selection or shape live | — |
| Ctrl | temporary Select / Move while in a markup tool | resize as a square |

Click **Annotate** (or press `A`, `R`, `E`, `B`, `M`, `T`, `N`, `X`, `S`) in the
capture overlay to unfold markup and irreversible redaction tools directly on
the frozen screen (`V` returns to Select / Move, `Ctrl+Z` / `Ctrl+Shift+Z` /
`Ctrl+Y` undo and redo, `[` and `]` adjust tool size).

### Quick Editor window

Open any screenshot or image in the standalone Quick Editor window from:

- Capture overlay: **Edit in Window** (`Ctrl+E`), keeping drawn annotations
  editable as vector items
- Capture notification: **Edit**
- Plasma widget or tray menu: **Edit Last Capture**
- Command line or file manager: `glimply <file>` or `glimply --edit <file>`
  (forwarded to the running glimply instance)

Inside the Quick Editor: **Crop** (`C` + `Enter` to apply, undoable with
`Ctrl+Z`), **Ctrl+Wheel** / **Alt+Wheel** for cursor-anchored zoom
(`Ctrl+0` fit, `Ctrl+1` 100%, crisp pixels `≥ 400 %`, pixel grid `≥ 800 %`),
**Space+Drag** or middle-mouse drag to pan, **Save** (`Ctrl+S`), **Save Copy**
(`Ctrl+Alt+S` as `_edited.png` without altering the original or clipboard),
**Save As…** (`Ctrl+Shift+S`), and **Copy** (`Ctrl+C`).

A right click deletes the topmost annotation or rectangle under the pointer;
Delete removes the active one.

To set up areas without capturing:

1. Open the glimply widget in the system tray and choose **Add Area**. Every
   monitor shows a frozen, dimmed picture. Drag a rectangle on the monitor you
   want and adjust it: drag the round handles on corners and edges to resize,
   drag inside to move, use the centre crosshair to aim. Hold **Shift** while
   drawing or resizing for a perfect square, **Alt** to grow from the centre,
   and **Space** mid-drag to reposition. Choose **Save as Area**.
2. Enter a name and, optionally, record a shortcut right there.
3. Press the shortcut. The tray icon blinks once and the PNG is in the capture
   folder.

Groups are created under *Configure glimply… → Groups*.

### D-Bus interface

The widget uses `io.github.m00sfett.glimply` at `/Control`
(`io.github.m00sfett.glimply.Control`): `Targets` (JSON), `HasLastCapture`,
`Trigger`, `AddArea`, `EditArea`, `Rename`, `Remove`, `OpenLastCapture`,
`EditLastCapture`, `OpenCaptureFolder`, `Configure`, `DelaySeconds`,
`KeepDelayActive`, `CountdownActive`, `SetDelay`, `CancelCountdown`,
`PickColor`, and the signals `Changed` and `Captured`. Scripts can use it too,
for example `busctl --user call io.github.m00sfett.glimply /Control
io.github.m00sfett.glimply.Control Trigger s all`.

## Development

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

An optional live check, `glimply-capture-smoke`, captures a monitor-local
rectangle twice on each connected screen and compares both PNGs. Build the
target, set `GLIMPLY_CAPTURE_SMOKE=1` and run it on a Plasma Wayland session.
The rectangle defaults to (8, 64) with 400 × 300 logical pixels and can be set
in `~/.config/glimply/capture-smoke.ini`:

```ini
[General]
screen=DP-1
area=@Rect(8 64 400 300)
```

The interface is English. Translations are planned; until they ship, KDE and
Qt widgets inside glimply are kept in English too.

## License

glimply is licensed under the GNU Affero General Public License, version 3 or
later. See [LICENSE](LICENSE) for the complete license text.
