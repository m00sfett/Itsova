#include "replayplan.h"

#include <algorithm>

namespace itsova
{

namespace
{

const ScreenInfo *matchScreenByIdentity(const RecordingMonitorInfo &recorded,
                                        const QList<ScreenInfo> &screens,
                                        const QSet<QString> &usedConnectors)
{
    // 1. Exact hardware identity with serial.
    if (!recorded.serial.isEmpty()) {
        for (const ScreenInfo &s : screens) {
            if (!usedConnectors.contains(s.connector) && s.serialNumber == recorded.serial
                && s.manufacturer == recorded.manufacturer && s.model == recorded.model) {
                return &s;
            }
        }
    }
    // 2. Manufacturer + model match (when serial is empty).
    if (!recorded.manufacturer.isEmpty() || !recorded.model.isEmpty()) {
        for (const ScreenInfo &s : screens) {
            if (!usedConnectors.contains(s.connector) && s.manufacturer == recorded.manufacturer
                && s.model == recorded.model) {
                return &s;
            }
        }
    }
    // 3. Fallback to connector name.
    for (const ScreenInfo &s : screens) {
        if (!usedConnectors.contains(s.connector) && s.connector == recorded.connector) {
            return &s;
        }
    }
    return nullptr;
}

const ScreenInfo *findExplicitScreen(const QString &query,
                                     const QList<ScreenInfo> &screens,
                                     const QHash<QString, QString> &monitorNames)
{
    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        return nullptr;
    }
    for (const ScreenInfo &s : screens) {
        if (s.connector.compare(trimmed, Qt::CaseInsensitive) == 0) {
            return &s;
        }
        const QString custom = monitorNames.value(TargetStore::monitorKey(s));
        if (!custom.isEmpty() && custom.compare(trimmed, Qt::CaseInsensitive) == 0) {
            return &s;
        }
    }
    return nullptr;
}

const ScreenInfo *screenAtCursor(const QList<ScreenInfo> &screens, const QPoint &cursorPos)
{
    for (const ScreenInfo &s : screens) {
        if (s.geometry.contains(cursorPos)) {
            return &s;
        }
    }
    return screens.isEmpty() ? nullptr : &screens.first();
}

QString missingMonitorLabel(const RecordingMonitorInfo &m)
{
    if (!m.model.isEmpty() && !m.connector.isEmpty()) {
        return QStringLiteral("%1 (%2)").arg(m.model, m.connector);
    }
    if (!m.model.isEmpty()) {
        return m.model;
    }
    return m.connector.isEmpty() ? QStringLiteral("unknown monitor") : m.connector;
}

} // namespace

QRectF aspectFitRect(const QSizeF &contentSize, const QSizeF &boundsSize)
{
    if (contentSize.width() <= 0 || contentSize.height() <= 0 || boundsSize.width() <= 0 || boundsSize.height() <= 0) {
        return QRectF(QPointF(0, 0), boundsSize);
    }
    const qreal scale = std::min(boundsSize.width() / contentSize.width(), boundsSize.height() / contentSize.height());
    const qreal w = contentSize.width() * scale;
    const qreal h = contentSize.height() * scale;
    return QRectF((boundsSize.width() - w) / 2.0, (boundsSize.height() - h) / 2.0, w, h);
}

ReplayPlan planReplay(const QSize &videoSize,
                      const std::optional<RecordingMetadata> &metadata,
                      const QList<ScreenInfo> &screens,
                      const QHash<QString, QString> &monitorNames,
                      const QString &explicitScreen,
                      const QPoint &cursorPos)
{
    ReplayPlan plan;
    if (screens.isEmpty()) {
        plan.error = QStringLiteral("No monitor is connected.");
        return plan;
    }
    const QRectF fullVideo(0, 0, std::max(1, videoSize.width()), std::max(1, videoSize.height()));

    // 1. Explicit --screen <name> always plays aspect-fit fullscreen on the requested monitor.
    if (!explicitScreen.trimmed().isEmpty()) {
        const ScreenInfo *chosen = findExplicitScreen(explicitScreen, screens, monitorNames);
        if (!chosen) {
            plan.error = QStringLiteral("Monitor \"%1\" is not connected.").arg(explicitScreen.trimmed());
            return plan;
        }
        plan.windows.append({chosen->connector, chosen->geometry, fullVideo,
                             aspectFitRect(fullVideo.size(), QSizeF(chosen->geometry.size()))});
        return plan;
    }

    // 2. Itsova recording with metadata (#103): place by monitor identity.
    if (metadata && !metadata->monitors.isEmpty()) {
        QSet<QString> used;
        const bool isWindow = (metadata->kind == QLatin1String("window"));
        const bool isArea = (metadata->kind == QLatin1String("area"));

        for (const RecordingMonitorInfo &recMon : metadata->monitors) {
            const ScreenInfo *matched = matchScreenByIdentity(recMon, screens, used);
            if (!matched) {
                plan.missingMonitors.append(missingMonitorLabel(recMon));
                continue;
            }
            used.insert(matched->connector);

            const QRectF srcRect =
                (!isWindow && recMon.video.isValid() && !recMon.video.isEmpty()) ? QRectF(recMon.video) : fullVideo;
            QRectF dstRect;

            if (isWindow && metadata->windowRect.isValid() && !metadata->windowRect.isEmpty()) {
                // Window recording: original size at original position on the matched monitor, rest black.
                QRectF localWin(metadata->windowRect);
                if (recMon.logical.isValid() && !recMon.logical.isEmpty()
                    && recMon.logical.contains(metadata->windowRect.topLeft())) {
                    localWin.translate(-recMon.logical.topLeft());
                }
                const QSizeF screenSz(matched->geometry.size());
                if (localWin.width() <= screenSz.width() && localWin.height() <= screenSz.height()) {
                    const qreal x = std::clamp(localWin.x(), 0.0, screenSz.width() - localWin.width());
                    const qreal y = std::clamp(localWin.y(), 0.0, screenSz.height() - localWin.height());
                    dstRect = QRectF(x, y, localWin.width(), localWin.height());
                } else {
                    dstRect = aspectFitRect(localWin.size(), screenSz);
                }
            } else if (isArea && metadata->region.isValid() && !metadata->region.isEmpty() && recMon.logical.isValid()) {
                // Area recording: original size at original position on the matched monitor, rest black.
                const QRect inter = metadata->region.intersected(recMon.logical);
                if (inter.isEmpty()) {
                    continue;
                }
                QRectF localArea(inter.translated(-recMon.logical.topLeft()));
                const QSizeF screenSz(matched->geometry.size());
                if (localArea.width() <= screenSz.width() && localArea.height() <= screenSz.height()) {
                    const qreal x = std::clamp(localArea.x(), 0.0, screenSz.width() - localArea.width());
                    const qreal y = std::clamp(localArea.y(), 0.0, screenSz.height() - localArea.height());
                    dstRect = QRectF(x, y, localArea.width(), localArea.height());
                } else {
                    dstRect = aspectFitRect(localArea.size(), screenSz);
                }
            } else {
                // Monitor or all-monitors recording: fill the matched monitor (scaled if resolution/scale changed).
                dstRect = QRectF(QPointF(0, 0), QSizeF(matched->geometry.size()));
            }

            plan.windows.append({matched->connector, matched->geometry, srcRect, dstRect});
        }

        if (!plan.windows.isEmpty()) {
            plan.usedMetadata = true;
            return plan;
        }
    }

    // 3. Foreign video or no matched monitor from metadata: fullscreen aspect-fit on the pointer's monitor.
    const ScreenInfo *target = screenAtCursor(screens, cursorPos);
    plan.windows.append(
        {target->connector, target->geometry, fullVideo, aspectFitRect(fullVideo.size(), QSizeF(target->geometry.size()))});
    return plan;
}

} // namespace itsova
