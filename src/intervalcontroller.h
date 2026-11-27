#pragma once

#include "targets.h"
#include "triggerengine.h"
#include "triggerplan.h"

#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <optional>

class QTimer;

namespace itsova
{

struct ActiveIntervalStatus
{
    QString targetId;
    QString displayName;
    IntervalConfig config;
    int captureCount = 0;
    int skippedUnchanged = 0;
    QDateTime startedAt;
};

// Manages concurrent, independent scheduled capture intervals per target or group.
// All capture work is delegated through an injectable runner (TriggerEngine in production),
// keeping group simultaneity and raw pixel change detection consistent with manual triggers.
class IntervalController : public QObject
{
    Q_OBJECT

public:
    static constexpr qint64 minFreeDiskBytes = 100LL * 1024 * 1024; // 100 MiB

    using PlanBuilder = std::function<TriggerPlan(const QString &targetId)>;
    using DirectoryProvider = std::function<QString()>;
    using TemplateProvider = std::function<QString()>;
    using CaptureRunner = std::function<void(TriggerPlan, const QString &directory, const QString &filenameTemplate,
                                             std::optional<TriggerEngine::FrameMap> previousFrames,
                                             std::function<void(TriggerResult, TriggerEngine::FrameMap)>)>;
    using DiskSpaceProvider = std::function<qint64(const QString &directory)>;
    using ClockProvider = std::function<qint64()>;
    using TickIntervalProvider = std::function<int(int seconds)>;

    explicit IntervalController(PlanBuilder planBuilder,
                                DirectoryProvider directoryProvider,
                                TemplateProvider templateProvider,
                                CaptureRunner captureRunner,
                                QObject *parent = nullptr);
    ~IntervalController() override;

    void setDiskSpaceProvider(DiskSpaceProvider provider);
    void setClockProvider(ClockProvider provider);
    void setTickIntervalProvider(TickIntervalProvider provider);

    // Starts an interval for targetId. Returns an error string on validation failure, or an empty
    // string on success. If targetId is already active, restarts cleanly with the new config.
    [[nodiscard]] QString start(const QString &targetId, const QString &displayName, const IntervalConfig &config);
    bool stop(const QString &targetId);
    void stopAll();
    bool toggle(const QString &targetId, const QString &displayName, const IntervalConfig &config,
                QString *error = nullptr);

    [[nodiscard]] bool isActive(const QString &targetId) const;
    [[nodiscard]] bool hasActive() const;
    [[nodiscard]] QList<ActiveIntervalStatus> activeIntervals() const;

    [[nodiscard]] static qint64 defaultFreeDiskBytes(const QString &directory);

Q_SIGNALS:
    void changed();
    void captureSaved(const QString &targetId, const itsova::TriggerResult &result);
    void intervalFinished(const QString &targetId, const QString &displayName, int captureCount);
    void intervalStoppedWithError(const QString &targetId, const QString &displayName, const QString &reason);

private:
    struct RunState
    {
        QString targetId;
        QString displayName;
        IntervalConfig config;
        QTimer *timer = nullptr;
        qint64 startedMs = 0;
        QDateTime startedAt;
        int captureCount = 0;
        int skippedUnchanged = 0;
        bool inFlight = false;
        std::uint64_t generation = 0;
        TriggerEngine::FrameMap lastFrames;
    };

    void performTick(const QString &targetId, std::uint64_t generation);
    void finishRun(const QString &targetId, bool emitFinishedSignal, const QString &errorReason);

    PlanBuilder planBuilder_;
    DirectoryProvider directoryProvider_;
    TemplateProvider templateProvider_;
    CaptureRunner captureRunner_;
    DiskSpaceProvider diskSpaceProvider_;
    ClockProvider clockProvider_;
    TickIntervalProvider tickIntervalProvider_;
    QHash<QString, RunState> runs_;
    std::uint64_t nextGeneration_ = 1;
};

} // namespace itsova
