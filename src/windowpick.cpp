#include "windowpick.h"

#include <algorithm>

namespace glimply
{

QList<WindowInfo> pickableWindows(const QList<WindowInfo> &windows)
{
    QList<WindowInfo> pickable;
    for (const WindowInfo &window : windows) {
        if (!window.minimized && window.onCurrentDesktop && !window.geometry.isEmpty()) {
            pickable.append(window);
        }
    }
    std::stable_sort(pickable.begin(), pickable.end(),
                     [](const WindowInfo &a, const WindowInfo &b) { return a.layer > b.layer; });
    return pickable;
}

PickTarget pickTargetAt(const QPoint &globalPos, const QList<WindowInfo> &windows, const QList<ScreenInfo> &screens)
{
    const auto screen = std::find_if(screens.cbegin(), screens.cend(),
                                     [&](const ScreenInfo &s) { return s.geometry.contains(globalPos); });
    if (screen == screens.cend()) {
        return {};
    }
    PickTarget monitor;
    monitor.kind = PickTarget::Kind::Monitor;
    monitor.connector = screen->connector;
    monitor.rect = screen->geometry;
    for (const WindowInfo &window : windows) {
        if (!window.geometry.contains(globalPos)) {
            continue;
        }
        if (window.fullscreen) {
            return monitor;
        }
        PickTarget pick;
        pick.kind = PickTarget::Kind::Window;
        pick.window = window;
        pick.rect = window.geometry;
        return pick;
    }
    return monitor;
}

} // namespace glimply
