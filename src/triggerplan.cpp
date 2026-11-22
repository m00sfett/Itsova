#include "triggerplan.h"

#include "imageoutput.h"

#include <QCoreApplication>
#include <QPainter>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace glimply
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

void addTarget(const TargetStore &store, const QList<ScreenInfo> &screens, const QString &targetId, TriggerPlan &plan)
{
    if (targetId == allMonitorsTargetId()) {
        if (screens.isEmpty()) {
            plan.errors.append(PlanText::tr("No monitor is connected."));
            return;
        }
        for (const ScreenInfo &screen : screens) {
            addConnector(plan, screen.connector);
        }
        plan.outputs.append({OutputSpec::Kind::AllMonitors, targetId, allMonitorsTargetId(), {}, {}});
        return;
    }

    if (isScreenTargetId(targetId)) {
        const QString connector = connectorFromScreenTargetId(targetId);
        if (!findScreen(screens, connector)) {
            plan.errors.append(PlanText::tr("Monitor %1 is not connected.").arg(connector));
            return;
        }
        addConnector(plan, connector);
        plan.outputs.append({OutputSpec::Kind::Screen, targetId, connector, connector, {}});
        return;
    }

    const Region *region = store.region(targetId);
    if (!region) {
        plan.errors.append(PlanText::tr("The capture target no longer exists."));
        return;
    }
    const ScreenInfo *screen = findScreen(screens, region->connector);
    if (!screen) {
        plan.errors.append(PlanText::tr("Region %1 was skipped: monitor %2 is not connected.")
                               .arg(region->name, region->connector));
        return;
    }
    if (!region->rect.isValid() || !QRect(QPoint(), screen->geometry.size()).contains(region->rect)) {
        plan.errors.append(PlanText::tr("Region %1 was skipped: it no longer fits on monitor %2.")
                               .arg(region->name, region->connector));
        return;
    }
    if (!region->monitorIdentity.isEmpty() && !screen->identity().isEmpty()
        && region->monitorIdentity != screen->identity()) {
        plan.warnings.append(PlanText::tr("Region %1: a different monitor is connected to %2 than when the "
                                          "region was defined.")
                                 .arg(region->name, region->connector));
    }
    addConnector(plan, region->connector);
    plan.outputs.append({OutputSpec::Kind::Region, targetId, region->name, region->connector, region->rect});
}

} // namespace

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
                addTarget(store, screens, targetId, plan);
            }
        }
        if (group->targetIds.isEmpty()) {
            plan.errors.append(PlanText::tr("Group %1 contains no targets.").arg(group->name));
        }
        return plan;
    }

    if (const Region *region = store.region(triggerId)) {
        plan.displayName = region->name;
    } else if (triggerId == allMonitorsTargetId()) {
        plan.displayName = PlanText::tr("All monitors");
    } else {
        plan.displayName = connectorFromScreenTargetId(triggerId);
    }
    addTarget(store, screens, triggerId, plan);
    return plan;
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
        RenderedOutput result{output.token, {}, {}};
        if (output.kind == OutputSpec::Kind::AllMonitors) {
            result.image = composeAllMonitors(plan.screens, frames, &result.error);
        } else {
            const auto frame = frames.constFind(output.connector);
            if (frame == frames.cend() || frame->image.isNull()) {
                result.error = frame == frames.cend() || frame->error.isEmpty()
                                   ? PlanText::tr("Monitor %1 could not be captured.").arg(output.connector)
                                   : frame->error;
            } else if (output.kind == OutputSpec::Kind::Screen) {
                result.image = frame->image;
            } else {
                result.image = cropLogicalRegion(frame->image, output.rect, frame->scale, &result.error);
                if (!result.error.isEmpty()) {
                    result.error = PlanText::tr("Region %1: %2").arg(output.token, result.error);
                }
            }
        }
        rendered.append(result);
    }
    return rendered;
}

} // namespace glimply
