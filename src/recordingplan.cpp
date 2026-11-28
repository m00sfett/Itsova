#include "recordingplan.h"

#include <QCoreApplication>

#include "imageoutput.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <cmath>

namespace itsova
{

QString recordingAudioSourceLabel(RecordingAudioSource source)
{
    switch (source) {
    case RecordingAudioSource::None:
        return QCoreApplication::translate("RecordingPlan", "No audio");
    case RecordingAudioSource::System:
        return QCoreApplication::translate("RecordingPlan", "System audio");
    case RecordingAudioSource::Microphone:
        return QCoreApplication::translate("RecordingPlan", "Microphone");
    case RecordingAudioSource::Application:
        return QCoreApplication::translate("RecordingPlan", "Window audio");
    }
    return {};
}

QString recordingFormatKey(RecordingFormat format)
{
    switch (format) {
    case RecordingFormat::Gif:
        return QStringLiteral("gif");
    case RecordingFormat::WebM:
        return QStringLiteral("webm");
    case RecordingFormat::Mp4:
        break;
    }
    return QStringLiteral("mp4");
}

RecordingFormat recordingFormatFromKey(const QString &key)
{
    const QString k = key.trimmed().toLower();
    if (k == QLatin1String("gif")) {
        return RecordingFormat::Gif;
    }
    if (k == QLatin1String("webm")) {
        return RecordingFormat::WebM;
    }
    return RecordingFormat::Mp4;
}

QString recordingExtension(RecordingFormat format)
{
    return recordingFormatKey(format);
}

QList<RecordingFormat> allRecordingFormats()
{
    return {RecordingFormat::Mp4, RecordingFormat::Gif, RecordingFormat::WebM};
}

QList<int> recordingFrameRates()
{
    return {15, 30, 60};
}

int effectiveFrameRate(RecordingFormat format, int requested)
{
    const QList<int> rates = recordingFrameRates();
    int rate = std::clamp(requested, rates.first(), rates.last());
    if (format == RecordingFormat::Gif) {
        rate = std::min(rate, maxGifFrameRate);
    }
    return rate;
}

bool RecordingTarget::isValid() const
{
    if (kind == Kind::Window) {
        return !windowHandle.isEmpty();
    }
    return region.width() > 0 && region.height() > 0 && scale > 0;
}

qreal regionScale(const QRect &region, const QList<QRect> &screenRects, const QList<qreal> &screenScales)
{
    qreal scale = 0;
    for (qsizetype i = 0; i < screenRects.size() && i < screenScales.size(); ++i) {
        if (screenRects.at(i).intersects(region)) {
            scale = std::max(scale, screenScales.at(i));
        }
    }
    return scale > 0 ? scale : 1.0;
}

QRect clipRegionToScreens(const QRect &region, const QList<QRect> &screenRects)
{
    QRect bounds;
    for (const QRect &rect : screenRects) {
        if (rect.intersects(region)) {
            bounds = bounds.united(rect);
        }
    }
    return bounds.isNull() ? QRect() : region.intersected(bounds);
}

QRect evenSizedRegion(const QRect &region, qreal scale)
{
    if (region.isEmpty() || scale <= 0) {
        return region;
    }
    QRect result = region;
    const auto devicePixels = [scale](int logical) { return qRound(logical * scale); };
    if (devicePixels(result.width()) % 2 != 0 && result.width() > 1) {
        result.setWidth(result.width() - 1);
    }
    if (devicePixels(result.height()) % 2 != 0 && result.height() > 1) {
        result.setHeight(result.height() - 1);
    }
    return result;
}

bool isRecordingFile(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("mp4") || suffix == QLatin1String("webm") || suffix == QLatin1String("gif");
}

QString formatElapsed(qint64 milliseconds)
{
    const qint64 total = std::max<qint64>(0, milliseconds) / 1000;
    const qint64 hours = total / 3600;
    const qint64 minutes = (total / 60) % 60;
    const qint64 seconds = total % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
}

QString recordingFileName(const QString &filenameTemplate, const QString &token, const QDateTime &timestamp,
                          RecordingFormat format)
{
    // The image template decides the name; its image extension gives way to the video one.
    const QString imageName = filenameFor(filenameTemplate, token, timestamp, ImageFormat::Png);
    const QString base = QFileInfo(imageName).completeBaseName();
    return (base.isEmpty() ? QStringLiteral("recording") : base) + QLatin1Char('.') + recordingExtension(format);
}

QString uniqueRecordingPath(const QString &directory, const QString &fileName)
{
    const QDir dir(directory);
    QString candidate = dir.filePath(fileName);
    if (!QFileInfo::exists(candidate)) {
        return candidate;
    }
    const QFileInfo info(fileName);
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix();
    for (int n = 2;; ++n) {
        candidate = dir.filePath(QStringLiteral("%1-%2.%3").arg(base).arg(n).arg(suffix));
        if (!QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
}

void RecordingClock::start(qint64 nowMs)
{
    startedAt_ = nowMs;
    pausedAt_ = 0;
    pausedTotal_ = 0;
    pauses_.clear();
    running_ = true;
    paused_ = false;
}

void RecordingClock::pause(qint64 nowMs)
{
    if (running_ && !paused_) {
        paused_ = true;
        pausedAt_ = nowMs;
    }
}

void RecordingClock::resume(qint64 nowMs)
{
    if (running_ && paused_) {
        pausedTotal_ += nowMs - pausedAt_;
        pauses_.append({pausedAt_ - startedAt_, nowMs - startedAt_});
        paused_ = false;
    }
}

qint64 RecordingClock::elapsed(qint64 nowMs) const
{
    if (!running_) {
        return 0;
    }
    const qint64 end = paused_ ? pausedAt_ : nowMs;
    return std::max<qint64>(0, end - startedAt_ - pausedTotal_);
}

QString recordingStopHint(const QString &shortcut)
{
    return shortcut.isEmpty() ? QCoreApplication::translate("RecordingPlan", "Click to stop")
                              : QCoreApplication::translate("RecordingPlan", "Click or press %1 to stop").arg(shortcut);
}

RecordingHintPlacement planRecordingHint(bool windowTarget, const QRect &recorded, const QList<QRect> &screens,
                                         const QSize &hint, int margin)
{
    RecordingHintPlacement placement;
    if (screens.isEmpty()) {
        return placement;
    }
    const auto topCentre = [&](const QRect &area) {
        return QRect(QPoint(area.x() + (area.width() - hint.width()) / 2, area.y() + margin), hint);
    };
    const auto overlap = [&](const QRect &screen) {
        const QRect common = screen.intersected(recorded);
        return common.isEmpty() ? 0LL : qint64(common.width()) * common.height();
    };
    int most = 0;
    for (int i = 1; i < screens.size(); ++i) {
        if (overlap(screens.at(i)) > overlap(screens.at(most))) {
            most = i;
        }
    }
    if (windowTarget) {
        // Other windows never show in a window stream; stay near the window.
        placement.screen = most;
        placement.rect = topCentre(screens.at(most));
        return placement;
    }
    for (int i = 0; i < screens.size(); ++i) {
        if (overlap(screens.at(i)) == 0) {
            placement.screen = i;
            placement.rect = topCentre(screens.at(i));
            return placement;
        }
    }
    // Beside the region: the largest strip of a touched monitor that the hint fits into.
    const QSize needed(hint.width() + 2 * margin, hint.height() + 2 * margin);
    qint64 bestArea = 0;
    for (int i = 0; i < screens.size(); ++i) {
        const QRect screen = screens.at(i);
        const QRect inside = screen.intersected(recorded);
        if (inside.isEmpty()) {
            continue;
        }
        const QList<QRect> strips{
            QRect(screen.left(), screen.top(), screen.width(), inside.top() - screen.top()),
            QRect(screen.left(), inside.bottom() + 1, screen.width(), screen.bottom() - inside.bottom()),
            QRect(screen.left(), screen.top(), inside.left() - screen.left(), screen.height()),
            QRect(inside.right() + 1, screen.top(), screen.right() - inside.right(), screen.height()),
        };
        for (const QRect &strip : strips) {
            if (strip.width() < needed.width() || strip.height() < needed.height()) {
                continue;
            }
            const qint64 area = qint64(strip.width()) * strip.height();
            if (area > bestArea) {
                bestArea = area;
                placement.screen = i;
                placement.rect = QRect(QPoint(strip.x() + (strip.width() - hint.width()) / 2,
                                              strip.y() + (strip.height() - hint.height()) / 2),
                                       hint);
            }
        }
    }
    if (bestArea > 0) {
        return placement;
    }
    placement.screen = most;
    placement.rect = QRect(screens.at(most).center() - QPoint(hint.width() / 2, hint.height() / 2), hint);
    placement.waitBeforeRecording = true;
    return placement;
}

} // namespace itsova
