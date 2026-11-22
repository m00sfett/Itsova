#pragma once

#include <QColor>
#include <QIcon>

namespace glimply
{

// The monochrome tray glyph: viewfinder corners around a small point. The flash variant fills the
// frame and is shown briefly after a successful capture.
[[nodiscard]] QIcon trayIcon(const QColor &color, bool flash = false);

} // namespace glimply
