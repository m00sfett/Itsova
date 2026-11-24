#pragma once

#include "captureservice.h"
#include "targets.h"

#include <QHash>
#include <QImage>
#include <QList>
#include <QRect>
#include <QString>
#include <QStringList>

namespace glimply
{

struct OutputSpec
{
    enum class Kind { Area, CrossMonitorArea, Screen, AllMonitors };

    Kind kind = Kind::Area;
    QString targetId;
    QString token; // filename token
    QString connector; // Area and Screen only
    QStringList connectors; // CrossMonitorArea only
    QRect rect; // Area: monitor-local logical; CrossMonitorArea: global logical coordinates
    ExportOptions exportOptions;
};

// What one trigger (shortcut, menu, later a timer) has to do. Every involved monitor is captured
// exactly once; all outputs are derived from these frames.
struct TriggerPlan
{
    QString triggerId;
    QString displayName;
    QStringList connectors;
    bool includeCursor = false;
    QList<OutputSpec> outputs;
    QList<ScreenInfo> screens; // layout snapshot used for composition
    QStringList errors; // skipped targets
    QStringList warnings; // captured, but something looks off
    QStringList partialNotifications; // Q8: passive notice when >=1 cross-monitor display is missing
};

struct RenderedOutput
{
    QString token;
    QImage image;
    QString error;
    ExportOptions exportOptions;
    QString targetId;
};

[[nodiscard]] QList<ScreenInfo> intersectingScreens(const QList<ScreenInfo> &screens, const QRect &globalRect);

[[nodiscard]] TriggerPlan planTrigger(const TargetStore &store, const QString &triggerId,
                                      const QList<ScreenInfo> &screens);

// Composes only the given screens clipped to globalLogicalRect at the highest scale among them.
// Regions outside monitor geometries stay transparent.
[[nodiscard]] QImage composeMonitorsCrop(const QList<ScreenInfo> &screens, const QHash<QString, CaptureResult> &frames,
                                         const QRect &globalLogicalRect, qreal *outScale = nullptr,
                                         QString *error = nullptr);

// Composes all monitors into one image at the highest scale of all frames. Areas not covered by a
// monitor stay transparent.
[[nodiscard]] QImage composeAllMonitors(const QList<ScreenInfo> &screens, const QHash<QString, CaptureResult> &frames,
                                        QString *error = nullptr);

[[nodiscard]] QList<RenderedOutput> renderOutputs(const TriggerPlan &plan,
                                                  const QHash<QString, CaptureResult> &frames);

} // namespace glimply
