#pragma once

#include "recordingmetadata.h"
#include "targets.h"

#include <QHash>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QStringList>

#include <optional>

namespace itsova
{

// Placement of one borderless fullscreen replay window on one connected monitor (#104).
struct ReplayWindowPlacement
{
    QString connector;
    QRect screenGeometry; // global logical geometry of the target monitor
    QRectF sourceRect;    // cut-out of the decoded video frame, in video pixels
    QRectF targetRect;    // destination rectangle in monitor-local logical coordinates
};

struct ReplayPlan
{
    QList<ReplayWindowPlacement> windows;
    QStringList missingMonitors;
    bool usedMetadata = false;
    QString error;
};

// Computes aspect-ratio-preserving centered destination rectangle inside bounds (letterbox/pillarbox).
[[nodiscard]] QRectF aspectFitRect(const QSizeF &contentSize, const QSizeF &boundsSize);

// Plans where a video should play across connected monitors (#104):
// - explicitScreen (from `--screen <name>`): fullscreen aspect-fit on that monitor.
// - metadata with monitor info: matched by monitor identity (not position); all-monitor/monitor
//   recordings fill each matched monitor (scaled if resolution/scale changed), window/area recordings
//   play at original size and position with black surround; missing monitors are listed so the caller
//   can notify while playing the rest.
// - foreign / metadata-less video: fullscreen aspect-fit on the monitor under cursorPos.
[[nodiscard]] ReplayPlan planReplay(const QSize &videoSize,
                                    const std::optional<RecordingMetadata> &metadata,
                                    const QList<ScreenInfo> &screens,
                                    const QHash<QString, QString> &monitorNames,
                                    const QString &explicitScreen,
                                    const QPoint &cursorPos);

} // namespace itsova
