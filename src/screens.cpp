#include "screens.h"

#include <QGuiApplication>
#include <QScreen>

#include <algorithm>
#include <utility>

namespace glimply
{

ScreenInfo screenInfo(const QScreen *screen)
{
    return {screen->name(), screen->geometry(), screen->manufacturer(), screen->model(), screen->serialNumber()};
}

QList<ScreenInfo> currentScreens()
{
    QList<ScreenInfo> screens;
    for (const QScreen *screen : QGuiApplication::screens()) {
        if (!screen->name().isEmpty() && screen->geometry().isValid()) {
            screens.append(screenInfo(screen));
        }
    }
    // Left to right, then top to bottom, as the user sees them.
    std::sort(screens.begin(), screens.end(), [](const ScreenInfo &a, const ScreenInfo &b) {
        return std::pair(a.geometry.x(), a.geometry.y()) < std::pair(b.geometry.x(), b.geometry.y());
    });
    return screens;
}

QScreen *findScreen(const QString &connector)
{
    for (QScreen *screen : QGuiApplication::screens()) {
        if (screen->name() == connector) {
            return screen;
        }
    }
    return nullptr;
}

QString monitorName(const TargetStore &store, const QString &connector)
{
    const QScreen *screen = findScreen(connector);
    return screen ? store.monitorName(screenInfo(screen)) : connector;
}

} // namespace glimply
