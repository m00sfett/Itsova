#pragma once

#include <QList>
#include <QRect>
#include <QString>

namespace glimply
{

// A window KWin can capture on its own.
struct WindowInfo {
    QString handle; // KWin UUID, valid while the window is open
    QString caption;
    QString appName; // resource name, used as file name token
    QString iconName; // desktop file name or resource class, for QIcon::fromTheme
    QRect geometry; // logical, compositor space
    bool minimized = false;
    bool fullscreen = false;
    int layer = 2;                 // KWin's stacking layer (2 = normal, 4 = keep above, …)
    bool onCurrentDesktop = true;  // on the current virtual desktop and activity
};

// Open application windows in KWin's order (most recently used first). Skips windows that hide
// from the taskbar, refuse captures, or belong to excludedPid. Blocking; call on the GUI thread.
[[nodiscard]] QList<WindowInfo> listWindows(qint64 excludedPid);

// File name token for a window capture, e.g. "window-konsole".
[[nodiscard]] QString windowToken(const WindowInfo &window);

} // namespace glimply
