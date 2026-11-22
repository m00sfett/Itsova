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
