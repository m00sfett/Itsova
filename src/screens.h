#pragma once

#include "targets.h"

#include <QList>

class QScreen;

namespace itsova
{

[[nodiscard]] ScreenInfo screenInfo(const QScreen *screen);
// The current monitor layout. Call on the GUI thread.
[[nodiscard]] QList<ScreenInfo> currentScreens();
[[nodiscard]] QScreen *findScreen(const QString &connector);
// The user's name for the monitor on connector, or the connector if it has none or is gone.
[[nodiscard]] QString monitorName(const TargetStore &store, const QString &connector);

} // namespace itsova
