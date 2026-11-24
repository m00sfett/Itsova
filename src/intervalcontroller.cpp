#include "intervalcontroller.h"

#include <QDir>
#include <QStorageInfo>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace glimply
{

IntervalController::IntervalController(PlanBuilder planBuilder,
                                       DirectoryProvider directoryProvider,
                                       TemplateProvider templateProvider,
                                       CaptureRunner captureRunner,
                                       QObject *parent)
    : QObject(parent)
    , planBuilder_(std::move(planBuilder))
    , directoryProvider_(std::move(directoryProvider))
    , templateProvider_(std::move(templateProvider))
    , captureRunner_(std::move(captureRunner))
    , diskSpaceProvider_(&IntervalController::defaultFreeDiskBytes)
    , clockProvider_([] { return QDateTime::currentMSecsSinceEpoch(); })
    , tickIntervalProvider_([](int secs) { return std::clamp(secs, 1, 86400) * 1000; })
{
}

IntervalController::~IntervalController()
{
    for (auto it = runs_.begin(); it != runs_.end(); ++it) {
        if (it->timer) {
            it->timer->stop();
        }
    }
    runs_.clear();
}

void IntervalController::setDiskSpaceProvider(DiskSpaceProvider provider)
{
    diskSpaceProvider_ = std::move(provider);
}

void IntervalController::setClockProvider(ClockProvider provider)
{
    clockProvider_ = std::move(provider);
}

void IntervalController::setTickIntervalProvider(TickIntervalProvider provider)
{
    tickIntervalProvider_ = std::move(provider);
}

qint64 IntervalController::defaultFreeDiskBytes(const QString &directory)
{
    QDir dir(directory);
    while (!dir.exists() && dir.cdUp()) {
    }
    const QStorageInfo info(dir.exists() ? dir.absolutePath() : QDir::homePath());
    return info.isValid() ? info.bytesAvailable() : -1;
}

QString IntervalController::start(const QString &targetId, const QString &displayName, const IntervalConfig &rawConfig)
{
    if (targetId.isEmpty() || !planBuilder_ || !captureRunner_) {
        return tr("Invalid interval target.");
    }

    IntervalConfig config = rawConfig;
    config.seconds = std::clamp(config.seconds, 1, 86400);
    config.maxCaptures = std::clamp(config.maxCaptures, 0, 100000);
    config.maxMinutes = std::clamp(config.maxMinutes, 0, 10080);

    const TriggerPlan initialPlan = planBuilder_(targetId);
    if (initialPlan.outputs.isEmpty()) {
        if (!initialPlan.partialNotifications.isEmpty()) {
            return initialPlan.partialNotifications.join(QLatin1Char('\n'));
        }
        return initialPlan.errors.isEmpty() ? tr("The capture target is not available.")
                                            : initialPlan.errors.join(QLatin1Char('\n'));
    }

    if (runs_.contains(targetId)) {
        stop(targetId);
    }

    const std::uint64_t gen = nextGeneration_++;
    RunState state;
    state.targetId = targetId;
    state.displayName = displayName.trimmed().isEmpty()
                            ? (initialPlan.displayName.isEmpty() ? targetId : initialPlan.displayName)
                            : displayName.trimmed();
    state.config = config;
    state.startedMs = clockProvider_();
    state.startedAt = QDateTime::currentDateTime();
    state.generation = gen;
    state.timer = new QTimer(this);
    state.timer->setInterval(std::max(1, tickIntervalProvider_(config.seconds)));
    connect(state.timer, &QTimer::timeout, this, [this, targetId, gen] {
        performTick(targetId, gen);
    });

    runs_.insert(targetId, std::move(state));
    runs_[targetId].timer->start();
    Q_EMIT changed();

    // Capture the first frame immediately after control returns to the event loop.
    QTimer::singleShot(0, this, [this, targetId, gen] {
        performTick(targetId, gen);
    });
    return {};
}

bool IntervalController::stop(const QString &targetId)
{
    if (!runs_.contains(targetId)) {
        return false;
    }
    finishRun(targetId, false, {});
    return true;
}

void IntervalController::stopAll()
{
    const QStringList ids = runs_.keys();
    if (ids.isEmpty()) {
        return;
    }
    for (const QString &id : ids) {
        if (auto it = runs_.find(id); it != runs_.end()) {
            if (it->timer) {
                it->timer->stop();
                it->timer->deleteLater();
            }
            runs_.erase(it);
        }
    }
    Q_EMIT changed();
}

bool IntervalController::toggle(const QString &targetId, const QString &displayName, const IntervalConfig &config,
                                QString *error)
{
    if (isActive(targetId)) {
        stop(targetId);
        if (error) {
            error->clear();
        }
        return false;
    }
    const QString err = start(targetId, displayName, config);
    if (error) {
        *error = err;
    }
    return err.isEmpty();
}

bool IntervalController::isActive(const QString &targetId) const
{
    return runs_.contains(targetId);
}

bool IntervalController::hasActive() const
{
    return !runs_.isEmpty();
}

QList<ActiveIntervalStatus> IntervalController::activeIntervals() const
{
    QList<ActiveIntervalStatus> list;
    list.reserve(runs_.size());
    for (auto it = runs_.cbegin(); it != runs_.cend(); ++it) {
        list.append({it->targetId, it->displayName, it->config, it->captureCount, it->skippedUnchanged, it->startedAt});
    }
    std::sort(list.begin(), list.end(), [](const ActiveIntervalStatus &a, const ActiveIntervalStatus &b) {
        return a.startedAt < b.startedAt;
    });
    return list;
}

void IntervalController::performTick(const QString &targetId, std::uint64_t generation)
{
    auto it = runs_.find(targetId);
    if (it == runs_.end() || it->generation != generation || it->inFlight) {
        return;
    }

    if (it->config.maxMinutes > 0) {
        const qint64 elapsedMs = clockProvider_() - it->startedMs;
        if (elapsedMs >= qint64(it->config.maxMinutes) * 60'000LL) {
            finishRun(targetId, true, {});
            return;
        }
    }

    const QString directory = directoryProvider_ ? directoryProvider_() : TargetStore::defaultCaptureDirectory();
    const QString filenameTemplate =
        templateProvider_ ? templateProvider_() : TargetStore::defaultFilenameTemplate();

    if (diskSpaceProvider_) {
        const qint64 freeBytes = diskSpaceProvider_(directory);
        if (freeBytes >= 0 && freeBytes < minFreeDiskBytes) {
            finishRun(targetId, false,
                      tr("Stopped interval capture for “%1”: less than 100 MB of free disk space remaining.")
                          .arg(it->displayName));
            return;
        }
    }

    TriggerPlan plan = planBuilder_(targetId);
    if (plan.outputs.isEmpty()) {
        QString reason;
        if (!plan.partialNotifications.isEmpty()) {
            reason = plan.partialNotifications.join(QLatin1Char('\n'));
        } else if (!plan.errors.isEmpty()) {
            reason = plan.errors.join(QLatin1Char('\n'));
        } else {
            reason = tr("Stopped interval capture for “%1”: the target is no longer available.")
                         .arg(it->displayName);
        }
        finishRun(targetId, false, reason);
        return;
    }

    it->inFlight = true;
    // Plan errors describe members that were skipped on purpose (e.g. a disconnected monitor);
    // they must not count as a failed tick when every remaining output was merely unchanged.
    const QStringList planErrors = plan.errors;
    std::optional<TriggerEngine::FrameMap> prevFrames;
    if (it->config.onlyWhenChanged) {
        prevFrames = it->lastFrames;
    }

    captureRunner_(std::move(plan), directory, filenameTemplate, std::move(prevFrames),
                   [this, targetId, generation, planErrors](TriggerResult result,
                                                            TriggerEngine::FrameMap updatedFrames) {
                       auto activeIt = runs_.find(targetId);
                       if (activeIt == runs_.end() || activeIt->generation != generation) {
                           return;
                       }
                       activeIt->inFlight = false;
                       activeIt->skippedUnchanged += result.skippedUnchanged;

                       if (!updatedFrames.isEmpty()) {
                           for (auto fIt = updatedFrames.cbegin(); fIt != updatedFrames.cend(); ++fIt) {
                               activeIt->lastFrames.insert(fIt.key(), fIt.value());
                           }
                       }

                       if (!result.writtenPaths.isEmpty()) {
                           activeIt->captureCount += 1;
                           Q_EMIT captureSaved(targetId, result);
                       }

                       QStringList tickErrors = result.errors;
                       for (const QString &planError : planErrors) {
                           tickErrors.removeAll(planError);
                       }
                       if (!tickErrors.isEmpty() && result.writtenPaths.isEmpty()) {
                           finishRun(targetId, false,
                                     tr("Stopped interval capture for “%1”: %2")
                                         .arg(activeIt->displayName, tickErrors.join(QStringLiteral("; "))));
                           return;
                       }

                       if (activeIt->config.maxCaptures > 0 && activeIt->captureCount >= activeIt->config.maxCaptures) {
                           finishRun(targetId, true, {});
                           return;
                       }

                       Q_EMIT changed();
                   });
}

void IntervalController::finishRun(const QString &targetId, bool emitFinishedSignal, const QString &errorReason)
{
    auto it = runs_.find(targetId);
    if (it == runs_.end()) {
        return;
    }
    const QString name = it->displayName;
    const int count = it->captureCount;
    if (it->timer) {
        it->timer->stop();
        it->timer->deleteLater();
    }
    runs_.erase(it);
    Q_EMIT changed();

    if (!errorReason.isEmpty()) {
        Q_EMIT intervalStoppedWithError(targetId, name, errorReason);
    } else if (emitFinishedSignal) {
        Q_EMIT intervalFinished(targetId, name, count);
    }
}

} // namespace glimply
