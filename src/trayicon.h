#pragma once

#include <QIcon>

namespace glimply
{

// The tray icon in the roteson.de colours: yellow viewfinder corners around a small point on a red
// tile. The flash variant (yellow tile, red corners, large red square) is shown briefly after a
// successful capture.
[[nodiscard]] QIcon trayIcon(bool flash = false);

} // namespace glimply
