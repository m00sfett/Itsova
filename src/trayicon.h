#pragma once

#include <QIcon>

namespace glimply
{

enum class TrayIconState {
    Normal,
    Flash,
    IntervalActive,
    Recording, // screen recording (#99): the inverted tile of the flash with a red dot
    RecordingPaused, // the inverted tile with two red pause bars
};

// The tray icon in the roteson.de colours: yellow viewfinder corners around a small point on a red
// tile. Flash swaps to a yellow tile briefly after capture; IntervalActive adds a crisp recording
// dot in the corner.
[[nodiscard]] QIcon trayIcon(TrayIconState state);
[[nodiscard]] QIcon trayIcon(bool flash = false);

// Whether the tray icon is there and how (#123). The mode follows TargetStore::TrayVisibility
// (0 always, 1 while something runs, 2 while recording). Hidden removes the status notifier item;
// standby keeps it in the hidden part of the tray (while the widget is shown, only in mode 0).
enum class TrayPresence { Hidden, Standby, Shown };
[[nodiscard]] TrayPresence trayPresence(int mode, bool recording, bool intervalsActive, bool widgetActive);

} // namespace glimply
