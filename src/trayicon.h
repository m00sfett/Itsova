#pragma once

#include <QIcon>

namespace glimply
{

enum class TrayIconState {
    Normal,
    Flash,
    IntervalActive,
    Recording, // screen recording (#30): a record disc inside the viewfinder
    RecordingPaused, // two pause bars inside the viewfinder
};

// The tray icon in the roteson.de colours: yellow viewfinder corners around a small point on a red
// tile. Flash swaps to a yellow tile briefly after capture; IntervalActive adds a crisp recording
// dot in the corner.
[[nodiscard]] QIcon trayIcon(TrayIconState state);
[[nodiscard]] QIcon trayIcon(bool flash = false);

} // namespace glimply
