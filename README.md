# glimply

glimply is a KDE Plasma tray application for capturing fixed screen regions,
individual monitors, or all monitors with global shortcuts.

The project is in early development. The current executable is a buildable
application skeleton and does not capture screens yet.

## Build requirements

- CMake 3.25 or later
- C++20 compiler
- Qt 6.12 or later, including Qt Widgets
- KDE Frameworks 6 Config, GlobalAccel, and StatusNotifierItem

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

The executable is written to `build/glimply`.

## License

glimply is licensed under the GNU Affero General Public License, version 3 or
later. See [LICENSE](LICENSE) for the complete license text.

## Capture core checks

The automated checks use generated images and temporary output files.

```sh
cmake --build build
ctest --test-dir build --output-on-failure
```

An optional live check captures a monitor-local rectangle twice on each connected
screen. Keep the selected area still. The default rectangle starts at (8, 64)
and measures 400 × 300 logical pixels. To change it, use the Qt INI settings
file `~/.config/glimply/capture-smoke.ini`, for example:

```ini
[General]
screen=DP-1
region=@Rect(8 64 400 300)
```

Build the `glimply-capture-smoke` target. On a Plasma Wayland session, register
the generated `org.glimply.CaptureSmoke.desktop` in the user's applications
directory for the duration of the check. Its Exec field points to the built
test binary and declares its restricted KWin interface. Set
`GLIMPLY_CAPTURE_SMOKE=1` and run the binary. It reports pixel dimensions and
compares both encoded PNGs. Temporary screenshots live in a private runtime
directory and are removed when the check exits normally. Remove the test
desktop entry after use.
