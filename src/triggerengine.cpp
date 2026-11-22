#include "triggerengine.h"

#include "imageoutput.h"

#include <QFutureWatcher>
#include <QHash>
#include <QtConcurrent>

#include <mutex>
#include <utility>

namespace glimply
{

TriggerEngine::TriggerEngine(QObject *parent)
    : QObject(parent)
    , capturer_(&CaptureService::captureScreen)
{
    qRegisterMetaType<glimply::TriggerResult>();
}

void TriggerEngine::setCapturer(Capturer capturer)
{
    capturer_ = std::move(capturer);
}

QStringList TriggerEngine::lastPaths() const
{
    return lastPaths_;
}

TriggerResult TriggerEngine::execute(const TriggerPlan &plan, const Capturer &capturer, const QString &directory,
                                     const QString &filenameTemplate, const QDateTime &timestamp)
{
    TriggerResult result;
    result.triggerId = plan.triggerId;
    result.displayName = plan.displayName;
    result.timestamp = timestamp;
    result.errors = plan.errors;
    result.warnings = plan.warnings;

    // One capture per monitor, all started together so the frames show (nearly) the same instant.
    QHash<QString, CaptureResult> frames;
    std::mutex framesMutex;
    QtConcurrent::blockingMap(plan.connectors, [&](const QString &connector) {
        CaptureResult frame = capturer(connector, plan.includeCursor);
        const std::lock_guard lock(framesMutex);
        frames.insert(connector, std::move(frame));
    });
    for (const CaptureResult &frame : std::as_const(frames)) {
        if (!frame.image.isNull()) {
            ++result.capturedMonitors;
        }
    }

    const OutputWriter writer(directory, filenameTemplate);
    for (const RenderedOutput &output : renderOutputs(plan, frames)) {
        if (output.image.isNull()) {
            result.errors.append(output.error);
            continue;
        }
        QString path;
        QString error;
        if (writer.writeImage(output.image, output.token, timestamp, &path, &error)) {
            result.writtenPaths.append(path);
        } else {
            result.errors.append(error);
        }
    }
    result.errors.removeDuplicates();
    return result;
}

void TriggerEngine::run(TriggerPlan plan, const QString &directory, const QString &filenameTemplate)
{
    const QDateTime timestamp = QDateTime::currentDateTime();
    auto *watcher = new QFutureWatcher<TriggerResult>(this);
    connect(watcher, &QFutureWatcher<TriggerResult>::finished, this, [this, watcher] {
        const TriggerResult result = watcher->result();
        watcher->deleteLater();
        if (!result.writtenPaths.isEmpty()) {
            lastPaths_ = result.writtenPaths;
        }
        Q_EMIT finished(result);
    });
    watcher->setFuture(QtConcurrent::run([plan = std::move(plan), capturer = capturer_, directory, filenameTemplate,
                                          timestamp] {
        return execute(plan, capturer, directory, filenameTemplate, timestamp);
    }));
}

} // namespace glimply
