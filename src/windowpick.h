#pragma once

#include "targets.h"
#include "windowlist.h"

#include <QList>
#include <QPoint>
#include <QRect>

namespace glimply
{

// What a click into the overlay takes (#97): the window under the pointer, or the monitor over the
// desktop and over a full-screen window.
struct PickTarget
{
    enum class Kind { None, Window, Monitor };
    Kind kind = Kind::None;
    WindowInfo window;  // Kind::Window
    QString connector;  // Kind::Monitor
    QRect rect;         // global logical, what the highlight frames
};

// The windows a pointer can land on, topmost first: visible on the current desktop and activity,
// not minimized; higher KWin layers (keep above, full screen) before lower ones, otherwise KWin's
// most-recently-used order, which follows the stacking of windows raised by activation.
[[nodiscard]] QList<WindowInfo> pickableWindows(const QList<WindowInfo> &windows);

// windows must be pickableWindows() order.
[[nodiscard]] PickTarget pickTargetAt(const QPoint &globalPos, const QList<WindowInfo> &windows,
                                      const QList<ScreenInfo> &screens);

} // namespace glimply
