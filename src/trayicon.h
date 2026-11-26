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

} // namespace glimply
