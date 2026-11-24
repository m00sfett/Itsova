#pragma once

#include "captureservice.h"
#include "triggerplan.h"

#include <QDateTime>
#include <QObject>
#include <QStringList>

#include <functional>
#include <optional>

namespace glimply
{

struct TriggerOutputInfo
{
    QString targetId;
    QString path;
    int width = 0;
    int height = 0;
    QString format;
    qint64 bytes = 0;
};

struct TriggerResult
{
    QString triggerId;
    QString displayName;
    QDateTime timestamp;
    QStringList writtenPaths;
    QList<TriggerOutputInfo> outputInfos;
    QStringList errors;
    QStringList warnings;
    QStringList partialNotifications;
    int capturedMonitors = 0;
    int skippedUnchanged = 0;
    bool fromInterval = false;
};

// Runs a trigger: one capture per involved monitor (in parallel), then crops/compositions, then
// files that share one timestamp. Shortcuts, menu actions, and interval timers call execute/run.
class TriggerEngine : public QObject
{
    Q_OBJECT

public:
    using Capturer = std::function<CaptureResult(const QString &connector, bool includeCursor)>;
    using FrameMap = QHash<QString, QImage>;

    explicit TriggerEngine(QObject *parent = nullptr);

    void setCapturer(Capturer capturer);

    // Runs the plan on worker threads. finished() is emitted on this object's thread.
    void run(TriggerPlan plan, const QString &directory, const QString &filenameTemplate);

    // Runs an interval capture tick asynchronously using an optional previous-frame map for
    // exact pixel change detection. Does not emit finished().
    void runIntervalTick(TriggerPlan plan, const QString &directory, const QString &filenameTemplate,
                         std::optional<FrameMap> previousFrames,
                         std::function<void(TriggerResult, FrameMap)> onDone);

    [[nodiscard]] QStringList lastPaths() const;

    [[nodiscard]] static bool imagesPixelEqual(const QImage &a, const QImage &b);

    // Synchronous cores, also used by tests.
    [[nodiscard]] static TriggerResult execute(const TriggerPlan &plan, const Capturer &capturer,
                                               const QString &directory, const QString &filenameTemplate,
                                               const QDateTime &timestamp);
    [[nodiscard]] static TriggerResult executeWithChangeFilter(const TriggerPlan &plan, const Capturer &capturer,
                                                               const QString &directory,
                                                               const QString &filenameTemplate,
                                                               const QDateTime &timestamp,
                                                               const FrameMap *previousFrames,
                                                               FrameMap *updatedFrames);

Q_SIGNALS:
    void finished(const glimply::TriggerResult &result);

private:
    Capturer capturer_;
    QStringList lastPaths_;
};

} // namespace glimply
