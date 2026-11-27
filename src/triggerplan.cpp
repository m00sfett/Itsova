#include "triggerplan.h"

#include "imageoutput.h"

#include <QCoreApplication>
#include <QPainter>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace itsova
{
namespace
{

class PlanText
{
    Q_DECLARE_TR_FUNCTIONS(TriggerPlan)
};

const ScreenInfo *findScreen(const QList<ScreenInfo> &screens, const QString &connector)
{
    const auto it = std::find_if(screens.cbegin(), screens.cend(),
                                 [&](const ScreenInfo &s) { return s.connector == connector; });
    return it == screens.cend() ? nullptr : &*it;
}

void addConnector(TriggerPlan &plan, const QString &connector)
{
    if (!plan.connectors.contains(connector)) {
        plan.connectors.append(connector);
    }
}

void addTarget(const TargetStore &store, const QList<ScreenInfo> &screens, const QString &targetId, TriggerPlan &plan,
               const QString &groupId = QString())
{
    const ExportOptions exportOptions = store.effectiveExportOptions(targetId, groupId);
    if (targetId == allMonitorsTargetId()) {
        if (screens.isEmpty()) {
            plan.errors.append(PlanText::tr("No monitor is connected."));
            return;
        }
        for (const ScreenInfo &screen : screens) {
            addConnector(plan, screen.connector);
        }
        plan.outputs.append({OutputSpec::Kind::AllMonitors, targetId, allMonitorsTargetId(), {}, {}, {}, exportOptions});
        return;
    }

    if (isScreenTargetId(targetId)) {
        const QString connector = connectorFromScreenTargetId(targetId);
        const ScreenInfo *screen = findScreen(screens, connector);
        if (!screen) {
            plan.errors.append(PlanText::tr("Monitor %1 is not connected.").arg(connector));
            return;
        }
        addConnector(plan, connector);
        plan.outputs.append({OutputSpec::Kind::Screen, targetId, store.monitorName(*screen), connector, {}, {},
                             exportOptions});
        return;
    }

    const Area *area = store.area(targetId);
    if (!area) {
        plan.errors.append(PlanText::tr("The capture target no longer exists."));
        return;
    }
    if (area->isCrossMonitor()) {
        QList<ScreenInfo> connectedBindings;
        QStringList missingConnectors;
        for (const AreaMonitorBinding &binding : area->monitors) {
            if (const ScreenInfo *s = findScreen(screens, binding.connector)) {
                connectedBindings.append(*s);
                if (!binding.monitorIdentity.isEmpty() && !s->identity().isEmpty()
                    && binding.monitorIdentity != s->identity()) {
                    plan.warnings.append(PlanText::tr("Area %1: a different monitor is connected to %2 than when the "
                                                      "area was defined.")
                                             .arg(area->name, binding.connector));
                }
            } else {
                missingConnectors.append(binding.connector);
            }
        }

        if (missingConnectors.isEmpty()) {
            const QList<ScreenInfo> active = intersectingScreens(screens, area->rect);
            if (active.isEmpty()) {
                plan.errors.append(
                    PlanText::tr("Area %1 was skipped: it no longer overlaps any connected monitor.").arg(area->name));
                return;
            }
            if (active.size() == 1 && active.first().geometry.contains(area->rect)) {
                addConnector(plan, active.first().connector);
                plan.outputs.append({OutputSpec::Kind::Area, targetId, area->name, active.first().connector, {},
                                     area->rect.translated(-active.first().geometry.topLeft()), exportOptions});
            } else {
                QStringList activeConnectors;
                for (const ScreenInfo &s : active) {
                    addConnector(plan, s.connector);
                    activeConnectors.append(s.connector);
                }
                plan.outputs.append(
                    {OutputSpec::Kind::CrossMonitorArea, targetId, area->name, {}, activeConnectors, area->rect,
                     exportOptions});
            }
            return;
        }

        QList<ScreenInfo> visibleScreens;
        QRect visibleBounds;
        for (const ScreenInfo &s : std::as_const(connectedBindings)) {
            const QRect vis = area->rect.intersected(s.geometry);
            if (!vis.isEmpty()) {
                visibleScreens.append(s);
                visibleBounds = visibleBounds.united(vis);
            }
        }
        const QString missingList = missingConnectors.join(QStringLiteral(", "));
        if (visibleScreens.isEmpty()) {
            if (connectedBindings.isEmpty()) {
                plan.partialNotifications.append(
                    PlanText::tr("Area %1 was not captured: none of its monitors (%2) are connected.")
                        .arg(area->name, missingList));
            } else {
                plan.partialNotifications.append(
                    PlanText::tr("Area %1 was not captured: it does not overlap any connected monitor.")
                        .arg(area->name));
            }
            return;
        }

        QStringList connectedNames;
        connectedNames.reserve(visibleScreens.size());
        for (const ScreenInfo &s : std::as_const(visibleScreens)) {
            connectedNames.append(store.monitorName(s));
        }
        plan.partialNotifications.append(
            PlanText::tr("%1 not connected — saved visible part of %2 on %3.")
                .arg(missingList, area->name, connectedNames.join(QStringLiteral(", "))));
        if (visibleScreens.size() == 1 && visibleScreens.first().geometry.contains(visibleBounds)) {
            addConnector(plan, visibleScreens.first().connector);
            plan.outputs.append({OutputSpec::Kind::Area, targetId, area->name, visibleScreens.first().connector, {},
                                 visibleBounds.translated(-visibleScreens.first().geometry.topLeft()), exportOptions});
        } else {
            QStringList visConnectors;
            for (const ScreenInfo &s : std::as_const(visibleScreens)) {
                addConnector(plan, s.connector);
                visConnectors.append(s.connector);
            }
            plan.outputs.append({OutputSpec::Kind::CrossMonitorArea, targetId, area->name, {}, visConnectors,
                                 visibleBounds, exportOptions});
        }
        return;
    }

    const ScreenInfo *screen = findScreen(screens, area->connector);
    if (!screen) {
        plan.errors.append(PlanText::tr("Area %1 was skipped: monitor %2 is not connected.")
                               .arg(area->name, area->connector));
        return;
    }
    if (!area->rect.isValid() || !QRect(QPoint(), screen->geometry.size()).contains(area->rect)) {
        plan.errors.append(PlanText::tr("Area %1 was skipped: it no longer fits on monitor %2.")
                               .arg(area->name, area->connector));
        return;
    }
    if (!area->monitorIdentity.isEmpty() && !screen->identity().isEmpty()
        && area->monitorIdentity != screen->identity()) {
        plan.warnings.append(PlanText::tr("Area %1: a different monitor is connected to %2 than when the "
                                          "area was defined.")
                                 .arg(area->name, area->connector));
    }
    addConnector(plan, area->connector);
    plan.outputs.append(
        {OutputSpec::Kind::Area, targetId, area->name, area->connector, {}, area->rect, exportOptions});
}

} // namespace

QList<ScreenInfo> intersectingScreens(const QList<ScreenInfo> &screens, const QRect &globalRect)
{
    QList<ScreenInfo> hit;
    if (globalRect.isEmpty()) {
        return hit;
    }
    for (const ScreenInfo &screen : screens) {
        if (!screen.geometry.intersected(globalRect).isEmpty()) {
            hit.append(screen);
        }
    }
    return hit;
}

TriggerPlan planTrigger(const TargetStore &store, const QString &triggerId, const QList<ScreenInfo> &screens)
{
    TriggerPlan plan;
    plan.triggerId = triggerId;
    plan.screens = screens;
    plan.includeCursor = store.targetIncludesCursor(triggerId);

    if (const Group *group = store.group(triggerId)) {
        plan.displayName = group->name;
        QSet<QString> seen;
        for (const QString &targetId : group->targetIds) {
            if (!seen.contains(targetId) && !store.group(targetId)) {
                seen.insert(targetId);
                addTarget(store, screens, targetId, plan, group->id);
            }
        }
        if (group->targetIds.isEmpty()) {
            plan.errors.append(PlanText::tr("Group %1 contains no targets.").arg(group->name));
        }
        return plan;
    }

    if (const Area *area = store.area(triggerId)) {
        plan.displayName = area->name;
    } else if (triggerId == allMonitorsTargetId()) {
        plan.displayName = PlanText::tr("All monitors");
    } else {
        const QString connector = connectorFromScreenTargetId(triggerId);
        const ScreenInfo *screen = findScreen(screens, connector);
        plan.displayName = screen ? store.monitorName(*screen) : connector;
    }
    addTarget(store, screens, triggerId, plan);
    return plan;
}

QImage composeMonitorsCrop(const QList<ScreenInfo> &screens, const QHash<QString, CaptureResult> &frames,
                           const QRect &globalLogicalRect, qreal *outScale, QString *error)
{
    if (globalLogicalRect.isEmpty() || screens.isEmpty()) {
        if (error) {
            *error = PlanText::tr("Selection does not overlap any connected monitor.");
        }
        return {};
    }

    qreal scale = 0.0;
    for (const ScreenInfo &screen : screens) {
        const auto frame = frames.constFind(screen.connector);
        if (frame == frames.cend() || frame->image.isNull()) {
            if (error) {
                *error = PlanText::tr("Monitor %1 could not be captured for the combined image.").arg(screen.connector);
            }
            return {};
        }
        scale = std::max(scale, frame->scale > 0.0 ? frame->scale : 1.0);
    }
    if (scale <= 0.0) {
        if (error) {
            *error = PlanText::tr("Invalid monitor scale for combined capture.");
        }
        return {};
    }

    const auto edge = [](qint64 logical, qreal factor) { return int(std::llround(double(logical) * factor)); };
    const int canvasW = edge(globalLogicalRect.width(), scale);
    const int canvasH = edge(globalLogicalRect.height(), scale);
    if (canvasW <= 0 || canvasH <= 0) {
        if (error) {
            *error = PlanText::tr("Selection size is empty.");
        }
        return {};
    }

    QImage canvas(canvasW, canvasH, QImage::Format_ARGB32_Premultiplied);
    if (canvas.isNull()) {
        if (error) {
            *error = PlanText::tr("There is not enough memory for the combined image.");
        }
        return {};
    }
    canvas.fill(Qt::transparent);

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    for (const ScreenInfo &screen : screens) {
        const QRect overlap = screen.geometry.intersected(globalLogicalRect);
        if (overlap.isEmpty()) {
            continue;
        }
        const CaptureResult &frame = frames.value(screen.connector);
        const qreal srcScale = frame.scale > 0.0 ? frame.scale : 1.0;
        const QImage &image = frame.image;
        const QRect localOverlap = overlap.translated(-screen.geometry.topLeft());

        const int srcLeft = std::clamp(edge(localOverlap.x(), srcScale), 0, image.width());
        const int srcTop = std::clamp(edge(localOverlap.y(), srcScale), 0, image.height());
        const int srcRight = std::clamp(edge(qint64(localOverlap.x()) + localOverlap.width(), srcScale), 0, image.width());
        const int srcBottom =
            std::clamp(edge(qint64(localOverlap.y()) + localOverlap.height(), srcScale), 0, image.height());
        const QRect srcRect(srcLeft, srcTop, srcRight - srcLeft, srcBottom - srcTop);

        const int dstLeft = edge(qint64(overlap.x()) - globalLogicalRect.x(), scale);
        const int dstTop = edge(qint64(overlap.y()) - globalLogicalRect.y(), scale);
        const int dstRight = edge(qint64(overlap.x()) + overlap.width() - globalLogicalRect.x(), scale);
        const int dstBottom = edge(qint64(overlap.y()) + overlap.height() - globalLogicalRect.y(), scale);
        const QRect dstRect(dstLeft, dstTop, dstRight - dstLeft, dstBottom - dstTop);

        if (srcRect.isEmpty() || dstRect.isEmpty()) {
            continue;
        }
        const QImage opaque = image.hasAlphaChannel() ? image.convertToFormat(QImage::Format_RGB32) : image;
        painter.drawImage(dstRect, opaque, srcRect);
    }
    painter.end();

    if (outScale) {
        *outScale = scale;
    }
    if (error) {
        error->clear();
    }
    return canvas;
}

QImage composeAllMonitors(const QList<ScreenInfo> &screens, const QHash<QString, CaptureResult> &frames,
                          QString *error)
{
    QRect bounds;
    qreal scale = 0;
    for (const ScreenInfo &screen : screens) {
        const auto frame = frames.constFind(screen.connector);
        if (frame == frames.cend() || frame->image.isNull()) {
            if (error) {
                *error = PlanText::tr("Monitor %1 could not be captured for the combined image.").arg(screen.connector);
            }
            return {};
        }
        bounds = bounds.united(screen.geometry);
        scale = std::max(scale, frame->scale);
    }
    if (bounds.isEmpty() || scale <= 0) {
        if (error) {
            *error = PlanText::tr("No monitor is connected.");
        }
        return {};
    }

    const auto edge = [scale](qint64 logical) { return int(std::llround(double(logical) * scale)); };
    QImage canvas(edge(bounds.width()), edge(bounds.height()), QImage::Format_ARGB32_Premultiplied);
    if (canvas.isNull()) {
        if (error) {
            *error = PlanText::tr("There is not enough memory for the combined image.");
        }
        return {};
    }
    canvas.fill(Qt::transparent);

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    for (const ScreenInfo &screen : screens) {
        const QImage &image = frames.value(screen.connector).image;
        const int left = edge(screen.geometry.x() - bounds.x());
        const int top = edge(screen.geometry.y() - bounds.y());
        const int right = edge(qint64(screen.geometry.x()) + screen.geometry.width() - bounds.x());
        const int bottom = edge(qint64(screen.geometry.y()) + screen.geometry.height() - bounds.y());
        const QRect target(left, top, right - left, bottom - top);
        // Frames are opaque; drop any undefined alpha channel before compositing.
        const QImage opaque = image.hasAlphaChannel() ? image.convertToFormat(QImage::Format_RGB32) : image;
        painter.drawImage(target, opaque);
    }
    painter.end();
    if (error) {
        error->clear();
    }
    return canvas;
}

QList<RenderedOutput> renderOutputs(const TriggerPlan &plan, const QHash<QString, CaptureResult> &frames)
{
    QList<RenderedOutput> rendered;
    for (const OutputSpec &output : plan.outputs) {
        RenderedOutput result{output.token, {}, {}, output.exportOptions, output.targetId};
        if (output.kind == OutputSpec::Kind::AllMonitors) {
            result.image = composeAllMonitors(plan.screens, frames, &result.error);
        } else if (output.kind == OutputSpec::Kind::CrossMonitorArea) {
            QList<ScreenInfo> targetScreens;
            for (const QString &conn : output.connectors) {
                if (const ScreenInfo *s = findScreen(plan.screens, conn)) {
                    targetScreens.append(*s);
                }
            }
            result.image = composeMonitorsCrop(targetScreens, frames, output.rect, nullptr, &result.error);
            if (!result.error.isEmpty()) {
                result.error = PlanText::tr("Area %1: %2").arg(output.token, result.error);
            }
        } else {
            const auto frame = frames.constFind(output.connector);
            if (frame == frames.cend() || frame->image.isNull()) {
                result.error = frame == frames.cend() || frame->error.isEmpty()
                                   ? PlanText::tr("Monitor %1 could not be captured.").arg(output.connector)
                                   : frame->error;
            } else if (output.kind == OutputSpec::Kind::Screen) {
                result.image = frame->image;
            } else {
                result.image = cropLogicalArea(frame->image, output.rect, frame->scale, &result.error);
                if (!result.error.isEmpty()) {
                    result.error = PlanText::tr("Area %1: %2").arg(output.token, result.error);
                }
            }
        }
        rendered.append(result);
    }
    return rendered;
}

} // namespace itsova
