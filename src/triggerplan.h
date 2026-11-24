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
    enum class Kind { Area, Screen, AllMonitors };

    Kind kind = Kind::Area;
    QString targetId;
    QString token; // filename token
    QString connector; // Area and Screen only
    QRect rect; // Area only, monitor-local logical coordinates
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
};

struct RenderedOutput
{
    QString token;
    QImage image;
    QString error;
    ExportOptions exportOptions;
};

[[nodiscard]] TriggerPlan planTrigger(const TargetStore &store, const QString &triggerId,
                                      const QList<ScreenInfo> &screens);

// Composes all monitors into one image at the highest scale of all frames. Areas not covered by a
// monitor stay transparent.
[[nodiscard]] QImage composeAllMonitors(const QList<ScreenInfo> &screens, const QHash<QString, CaptureResult> &frames,
                                        QString *error = nullptr);

[[nodiscard]] QList<RenderedOutput> renderOutputs(const TriggerPlan &plan,
                                                  const QHash<QString, CaptureResult> &frames);

} // namespace glimply
