# glimply

glimply is a KDE Plasma tray application for capturing fixed screen areas,
individual monitors, or all monitors the moment a global shortcut is pressed:
no selection, no dialog, no overlay.

The project is in early development (0.1.x prototype) and targets KDE Plasma 6
on Wayland.

## Features

- **Areas** bound to one monitor, stored in logical coordinates and captured
  at the monitor's native resolution. glimply names them `<connector>-<n>`
  (for example `DP-1-1`); you can rename them.
- **Monitors and All monitors** are always available. *All monitors* is one
  image in the current arrangement at the highest monitor scale; gaps between
  monitors are transparent.
- **Groups** capture several targets with one shortcut. Each monitor is
  captured once per trigger, so all files show the same moment and share one
  timestamp.
- **Global shortcuts** for every target and group, registered with KDE's global
  shortcut service (also editable in System Settings). There are no default
  shortcuts.
- **Tray menu** with every target and its shortcut, *Open Last Capture*,
  *Open Capture Folder* and the settings.
- **Feedback** without interruptions: the tray icon blinks once after a
  capture. Errors, such as a missing monitor, raise a notification.
- Captures are saved as PNG in `~/Pictures/glimply/` (your XDG pictures folder)
  as `{date}_{time}_{area}.png`. Existing files are never overwritten.

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

On first start glimply adds itself to autostart. You can turn that off under
*Configure glimply… → General*.

### Screen capture permission

glimply uses KWin's `org.kde.KWin.ScreenShot2` interface. KWin grants access to
applications whose desktop entry declares
`X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2` and whose `Exec`
points to the running executable. The installed desktop entry does this. If
captures fail with a permission error, reinstall with `scripts/install.sh`.

## Usage

1. Click the tray icon and choose **Add Area…**. Every monitor shows a
   frozen, dimmed picture. Drag a rectangle on the monitor you want, adjust it
   (drag to move, arrow keys to move by a pixel, Shift+arrow keys to resize,
   Ctrl for steps of ten) and press **Enter**.
2. Record a shortcut for the new area in the settings window that opens.
3. Press the shortcut. The tray icon blinks once and the PNG is in the capture
   folder.

Groups are created under *Configure glimply… → Groups*.

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
