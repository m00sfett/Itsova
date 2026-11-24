#pragma once

#include "captureservice.h"
#include "triggerplan.h"

#include <QDateTime>
#include <QObject>
#include <QStringList>

#include <functional>

namespace glimply
{

struct TriggerResult
{
    QString triggerId;
    QString displayName;
    QDateTime timestamp;
    QStringList writtenPaths;
    QStringList errors;
    QStringList warnings;
    QStringList partialNotifications;
    int capturedMonitors = 0;
};

// Runs a trigger: one capture per involved monitor (in parallel), then crops/compositions, then
// files that share one timestamp. Shortcuts and menu actions call trigger(); a later interval timer
// can do the same.
class TriggerEngine : public QObject
{
    Q_OBJECT

public:
    using Capturer = std::function<CaptureResult(const QString &connector, bool includeCursor)>;

    explicit TriggerEngine(QObject *parent = nullptr);

    void setCapturer(Capturer capturer);

    // Runs the plan on worker threads. finished() is emitted on this object's thread.
    void run(TriggerPlan plan, const QString &directory, const QString &filenameTemplate);

    [[nodiscard]] QStringList lastPaths() const;

    // Synchronous core, also used by tests.
    [[nodiscard]] static TriggerResult execute(const TriggerPlan &plan, const Capturer &capturer,
                                               const QString &directory, const QString &filenameTemplate,
                                               const QDateTime &timestamp);

Q_SIGNALS:
    void finished(const glimply::TriggerResult &result);

private:
    Capturer capturer_;
    QStringList lastPaths_;
};

} // namespace glimply
