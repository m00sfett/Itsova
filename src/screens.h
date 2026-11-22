#pragma once

#include "targets.h"

#include <QList>

class QScreen;

namespace glimply
{

[[nodiscard]] ScreenInfo screenInfo(const QScreen *screen);
// The current monitor layout. Call on the GUI thread.
[[nodiscard]] QList<ScreenInfo> currentScreens();
[[nodiscard]] QScreen *findScreen(const QString &connector);

} // namespace glimply
