#include "triggerengine.h"

#include "imageoutput.h"

#include <QFileInfo>
#include <QFutureWatcher>
#include <QHash>
#include <QtConcurrent>

#include <cstddef>
#include <cstring>
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

bool TriggerEngine::imagesPixelEqual(const QImage &a, const QImage &b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size()) {
        return false;
    }
    const QImage *imgA = &a;
    const QImage *imgB = &b;
    QImage convertedA;
    QImage convertedB;
    if (a.format() != b.format() || a.depth() < 8) {
        convertedA = a.convertToFormat(QImage::Format_RGBA8888);
        convertedB = b.convertToFormat(QImage::Format_RGBA8888);
        imgA = &convertedA;
        imgB = &convertedB;
    }
    const qsizetype rowBytes = static_cast<qsizetype>(imgA->width()) * (imgA->depth() / 8);
    for (int y = 0; y < imgA->height(); ++y) {
        if (std::memcmp(imgA->constScanLine(y), imgB->constScanLine(y), static_cast<std::size_t>(rowBytes)) != 0) {
            return false;
        }
    }
    return true;
}

TriggerResult TriggerEngine::executeWithChangeFilter(const TriggerPlan &plan, const Capturer &capturer,
                                                    const QString &directory, const QString &filenameTemplate,
                                                    const QDateTime &timestamp, const FrameMap *previousFrames,
                                                    FrameMap *updatedFrames)
{
    TriggerResult result;
    result.triggerId = plan.triggerId;
    result.displayName = plan.displayName;
    result.timestamp = timestamp;
    result.errors = plan.errors;
    result.warnings = plan.warnings;
    result.partialNotifications = plan.partialNotifications;

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
        const QString frameKey = output.targetId.isEmpty() ? output.token : output.targetId;
        if (previousFrames) {
            const auto prevIt = previousFrames->constFind(frameKey);
            if (prevIt != previousFrames->cend() && imagesPixelEqual(*prevIt, output.image)) {
                ++result.skippedUnchanged;
                continue;
            }
        }
        QString path;
        QString error;
        if (writer.writeImage(output.image, output.token, timestamp, output.exportOptions, &path, &error)) {
            result.writtenPaths.append(path);
            result.outputInfos.append({frameKey, path, output.image.width(), output.image.height(),
                                       formatConfigKey(output.exportOptions.format), QFileInfo(path).size()});
            if (updatedFrames) {
                updatedFrames->insert(frameKey, output.image);
            }
        } else {
            result.errors.append(error);
        }
    }
    result.errors.removeDuplicates();
    return result;
}

TriggerResult TriggerEngine::execute(const TriggerPlan &plan, const Capturer &capturer, const QString &directory,
                                     const QString &filenameTemplate, const QDateTime &timestamp)
{
    return executeWithChangeFilter(plan, capturer, directory, filenameTemplate, timestamp, nullptr, nullptr);
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

void TriggerEngine::runIntervalTick(TriggerPlan plan, const QString &directory, const QString &filenameTemplate,
                                    std::optional<FrameMap> previousFrames,
                                    std::function<void(TriggerResult, FrameMap)> onDone)
{
    struct TickResult
    {
        TriggerResult result;
        FrameMap updatedFrames;
    };

    const QDateTime timestamp = QDateTime::currentDateTime();
    auto *watcher = new QFutureWatcher<TickResult>(this);
    connect(watcher, &QFutureWatcher<TickResult>::finished, this,
            [this, watcher, callback = std::move(onDone)] {
                TickResult out = watcher->result();
                watcher->deleteLater();
                out.result.fromInterval = true;
                if (!out.result.writtenPaths.isEmpty()) {
                    lastPaths_ = out.result.writtenPaths;
                }
                if (callback) {
                    callback(std::move(out.result), std::move(out.updatedFrames));
                }
            });
    watcher->setFuture(QtConcurrent::run(
        [plan = std::move(plan), capturer = capturer_, directory, filenameTemplate, timestamp,
         prev = std::move(previousFrames)] {
            TickResult out;
            out.result = executeWithChangeFilter(plan, capturer, directory, filenameTemplate, timestamp,
                                                 prev ? &*prev : nullptr, &out.updatedFrames);
            return out;
        }));
}

} // namespace glimply
