#include "annotationrenderer.h"

#include "annotationgeometry.h"
#include "annotationtextlayout.h"
#include "redactionprocessor.h"

#include <QFontMetricsF>
#include <QPainterPath>
#include <QPolygonF>
#include <QTransform>

#include <algorithm>
#include <cmath>

namespace itsova
{
namespace
{

struct RedactRenderContext {
    const QImage *sourceImage = nullptr;
    qreal sourceScale = 1.0;
    QPointF logicalOrigin;
    bool isExport = false;
};

void drawBarbedArrow(QPainter &painter, const QPointF &start, const QPointF &end, const QColor &color,
                     qreal strokeWidth)
{
    const qreal dx = end.x() - start.x();
    const qreal dy = end.y() - start.y();
    const qreal len = std::hypot(dx, dy);
    if (len < 1e-3) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(start, strokeWidth * 0.5, strokeWidth * 0.5);
        return;
    }
    const qreal ux = dx / len;
    const qreal uy = dy / len;
    const qreal px = -uy;
    const qreal py = ux;

    const qreal headLength = std::min(len, std::max(12.0, strokeWidth * 3.6));
    const qreal headHalfWidth = std::max(5.5, strokeWidth * 1.65);
    const qreal notchOffset = headLength * 0.18;

    const QPointF base(end.x() - ux * headLength, end.y() - uy * headLength);
    const QPointF notch(base.x() + ux * notchOffset, base.y() + uy * notchOffset);
    const qreal shaftStop = std::max(headLength * 0.72, strokeWidth * 0.5 + 2.0);
    const QPointF shaftEnd(end.x() - ux * shaftStop, end.y() - uy * shaftStop);

    if (len > shaftStop) {
        QPen pen(color, strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(start, shaftEnd);
    }

    const QPolygonF head{
        end,
        QPointF(base.x() + px * headHalfWidth, base.y() + py * headHalfWidth),
        notch,
        QPointF(base.x() - px * headHalfWidth, base.y() - py * headHalfWidth),
    };
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawPolygon(head);
}

void drawHighlighterStroke(QPainter &painter, const QRectF &logicalClipRect, const AnnotationItem &item)
{
    if (item.points.isEmpty()) {
        return;
    }
    const QRectF itemLogical = item.boundingRect().intersected(logicalClipRect);
    if (itemLogical.isEmpty()) {
        return;
    }
    const QTransform world = painter.worldTransform();
    const qreal dpr = painter.device() ? painter.device()->devicePixelRatioF() : 1.0;
    const QRect worldBox = world.mapRect(itemLogical).adjusted(-2.0, -2.0, 2.0, 2.0).toAlignedRect();
    if (worldBox.isEmpty()) {
        return;
    }
    const QSize pixSize(std::max(1, int(std::ceil(worldBox.width() * dpr))),
                        std::max(1, int(std::ceil(worldBox.height() * dpr))));
    QImage strokeBuffer(pixSize, QImage::Format_ARGB32_Premultiplied);
    strokeBuffer.setDevicePixelRatio(dpr);
    strokeBuffer.fill(Qt::transparent);

    {
        QPainter bp(&strokeBuffer);
        bp.setRenderHint(QPainter::Antialiasing);
        bp.setWorldTransform(world * QTransform::fromTranslate(-worldBox.left(), -worldBox.top()));

        QColor opaque = item.color;
        opaque.setAlpha(255);
        QPen pen(opaque, item.strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        bp.setPen(pen);
        bp.setBrush(Qt::NoBrush);
        bp.drawPath(smoothPolylinePath(item.points));
    }

    painter.save();
    painter.setWorldTransform(QTransform());
    painter.setCompositionMode(QPainter::CompositionMode_Multiply);
    painter.drawImage(worldBox.topLeft(), strokeBuffer);
    painter.restore();
}

void drawStepBadge(QPainter &painter, const AnnotationItem &item)
{
    if (item.hasPointer) {
        drawBarbedArrow(painter, item.start, item.end, item.color, item.strokeWidth);
    }
    const qreal r = item.badgeRadius();
    const QRectF badgeRect(item.start.x() - r, item.start.y() - r, 2.0 * r, 2.0 * r);

    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 75));
    painter.drawEllipse(badgeRect.translated(0.0, 1.5).adjusted(-1.0, -1.0, 1.0, 1.0));

    QColor solidFill = item.color;
    solidFill.setAlpha(255);
    const QColor textCol = contrastingTextColor(solidFill);
    const QColor ringCol = textCol.lightness() > 128 ? QColor(255, 255, 255, 220) : QColor(20, 20, 22, 180);

    painter.setPen(QPen(ringCol, 1.5));
    painter.setBrush(solidFill);
    painter.drawEllipse(badgeRect);

    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(std::max(11, int(std::lround(r * 1.15))));
    painter.setFont(font);
    painter.setPen(textCol);
    painter.drawText(badgeRect, Qt::AlignCenter, QString::number(item.stepNumber));
    painter.restore();
}

void drawRedactItem(QPainter &painter, const AnnotationItem &item, const RedactRenderContext &ctx)
{
    const QRectF r = item.rect.normalized();
    if (r.isEmpty()) {
        return;
    }

    if (ctx.sourceImage && !ctx.sourceImage->isNull() && ctx.sourceScale > 0.0) {
        const QRect physRect = redactionPhysicalRect(r, ctx.logicalOrigin, ctx.sourceScale, ctx.sourceImage->size());
        if (physRect.isEmpty()) {
            return;
        }
        const QPoint docOrigin(int(std::lround(ctx.logicalOrigin.x() * ctx.sourceScale)) + physRect.left(),
                               int(std::lround(ctx.logicalOrigin.y() * ctx.sourceScale)) + physRect.top());
        const QImage patch = renderRedactedPatch(*ctx.sourceImage, physRect, docOrigin, item, ctx.sourceScale);
        if (!patch.isNull()) {
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing, false);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
            if (ctx.isExport) {
                painter.setWorldTransform(QTransform());
                painter.setClipping(false);
                painter.setCompositionMode(QPainter::CompositionMode_Source);
                painter.drawImage(physRect.topLeft(), patch);
            } else {
                const QRectF destLogical(ctx.logicalOrigin.x() + physRect.left() / ctx.sourceScale,
                                         ctx.logicalOrigin.y() + physRect.top() / ctx.sourceScale,
                                         physRect.width() / ctx.sourceScale,
                                         physRect.height() / ctx.sourceScale);
                painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
                painter.drawImage(destLogical, patch);
            }
            painter.restore();
            return;
        }
    }

    QColor opaque = item.redactionMode == RedactionMode::Solid ? item.color : QColor(0, 0, 0);
    opaque.setAlpha(255);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setCompositionMode(ctx.isExport ? QPainter::CompositionMode_Source : QPainter::CompositionMode_SourceOver);
    painter.setPen(Qt::NoPen);
    painter.setBrush(opaque);
    painter.drawRect(r);
    painter.restore();
}

void drawNonSpotlightItem(QPainter &painter, const QRectF &logicalClipRect, const AnnotationItem &item,
                          const RedactRenderContext &ctx)
{
    switch (item.kind) {
    case ItemKind::Arrow:
        drawBarbedArrow(painter, item.start, item.end, item.color, item.strokeWidth);
        break;
    case ItemKind::Rectangle:
    case ItemKind::Ellipse: {
        const QRectF r = item.rect.normalized();
        if (r.isEmpty()) {
            break;
        }
        painter.save();
        painter.setPen(QPen(item.color, item.strokeWidth, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        if (item.fillMode == ShapeFillMode::TranslucentFill) {
            QColor fill = item.color;
            fill.setAlphaF(std::clamp(item.fillOpacity, 0.0, 1.0));
            painter.setBrush(fill);
        } else {
            painter.setBrush(Qt::NoBrush);
        }
        if (item.kind == ItemKind::Rectangle) {
            painter.drawRect(r);
        } else {
            painter.drawEllipse(r);
        }
        painter.restore();
        break;
    }
    case ItemKind::Pen: {
        if (item.points.isEmpty()) {
            break;
        }
        painter.save();
        painter.setPen(QPen(item.color, item.strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(smoothPolylinePath(item.points));
        painter.restore();
        break;
    }
    case ItemKind::Highlighter:
        drawHighlighterStroke(painter, logicalClipRect, item);
        break;
    case ItemKind::Step:
        drawStepBadge(painter, item);
        break;
    case ItemKind::Redact:
        drawRedactItem(painter, item, ctx);
        break;
    case ItemKind::Text:
        drawAnnotationText(painter, item);
        break;
    case ItemKind::Spotlight:
        break;
    }
}

void drawUnifiedSpotlights(QPainter &painter, const QRectF &logicalClipRect,
                           const QList<const AnnotationItem *> &spotlights)
{
    if (spotlights.isEmpty()) {
        return;
    }
    QPainterPath holes;
    holes.setFillRule(Qt::WindingFill);
    qreal dimOpacity = 0.0;
    for (const AnnotationItem *sp : spotlights) {
        const QRectF r = sp->rect.normalized();
        if (r.isEmpty()) {
            continue;
        }
        if (sp->spotlightShape == SpotlightShape::Ellipse) {
            holes.addEllipse(r);
        } else {
            holes.addRoundedRect(r, 4.0, 4.0);
        }
        dimOpacity = std::max(dimOpacity, std::clamp(sp->dimOpacity, 0.0, 0.95));
    }
    if (dimOpacity <= 0.0) {
        return;
    }
    QPainterPath shade;
    shade.addRect(logicalClipRect);
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor::fromRgbF(0.0, 0.0, 0.0, dimOpacity));
    painter.drawPath(shade.subtracted(holes));
    painter.restore();
}

void renderFourStagePipeline(QPainter &painter, const QRectF &logicalClipRect,
                             const QList<const AnnotationItem *> &items,
                             const RedactRenderContext &ctx)
{
    qsizetype lastRedactIdx = -1;
    QList<const AnnotationItem *> spotlights;
    for (qsizetype i = 0; i < items.size(); ++i) {
        const AnnotationItem *item = items[i];
        if (!item->intersectsLogicalRect(logicalClipRect)) {
            continue;
        }
        if (item->kind == ItemKind::Redact) {
            lastRedactIdx = i;
        } else if (item->kind == ItemKind::Spotlight) {
            spotlights.append(item);
        }
    }

    // Stage 2: items up to and including the last redaction.
    for (qsizetype i = 0; i <= lastRedactIdx; ++i) {
        const AnnotationItem *item = items[i];
        if (item->kind == ItemKind::Spotlight || !item->intersectsLogicalRect(logicalClipRect)) {
            continue;
        }
        drawNonSpotlightItem(painter, logicalClipRect, *item, ctx);
    }

    // Stage 3: unified spotlight dim layer.
    drawUnifiedSpotlights(painter, logicalClipRect, spotlights);

    // Stage 4: remaining non-redaction callout items above the spotlight dim layer.
    for (qsizetype i = lastRedactIdx + 1; i < items.size(); ++i) {
        const AnnotationItem *item = items[i];
        if (item->kind == ItemKind::Spotlight || !item->intersectsLogicalRect(logicalClipRect)) {
            continue;
        }
        drawNonSpotlightItem(painter, logicalClipRect, *item, ctx);
    }
}

} // namespace

QImage renderAnnotatedCrop(const QImage &nativeCrop, const QRectF &logicalCropRect, qreal nativeScale,
                           const AnnotationDocument &document)
{
    if (nativeCrop.isNull() || !logicalCropRect.isValid() || nativeScale <= 0.0) {
        return nativeCrop;
    }

    // Fresh image guarantees zero inherited text chunks, EXIF tags, or ICC/thumbnail metadata.
    QImage result(nativeCrop.size(), QImage::Format_ARGB32_Premultiplied);
    result.setDevicePixelRatio(1.0);
    {
        QPainter copyPainter(&result);
        copyPainter.setCompositionMode(QPainter::CompositionMode_Source);
        copyPainter.drawImage(0, 0, nativeCrop);
    }
    if (document.isEmpty()) {
        return result;
    }

    QList<const AnnotationItem *> refs;
    refs.reserve(document.count());
    for (const AnnotationItem &item : document.items()) {
        refs.append(&item);
    }

    QPainter painter(&result);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    painter.scale(nativeScale, nativeScale);
    painter.translate(-logicalCropRect.topLeft());
    painter.setClipRect(logicalCropRect);

    const RedactRenderContext ctx{&result, nativeScale, logicalCropRect.topLeft(), true};
    renderFourStagePipeline(painter, logicalCropRect, refs, ctx);
    return result;
}

void paintAnnotationOverlay(QPainter &painter, const QRectF &logicalViewportRect, const AnnotationDocument &document,
                            const QColor &accentColor, int hoveredId, const AnnotationItem *draftItem,
                            const QImage *nativeBackdrop, qreal backdropScale, const QPointF &backdropLogicalOrigin,
                            int hiddenItemId)
{
    if (!logicalViewportRect.isValid()) {
        return;
    }
    painter.save();
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    painter.setClipRect(logicalViewportRect);

    QList<const AnnotationItem *> refs;
    refs.reserve(document.count() + (draftItem ? 1 : 0));
    for (const AnnotationItem &item : document.items()) {
        if (hiddenItemId > 0 && item.id == hiddenItemId) {
            continue;
        }
        refs.append(&item);
    }
    if (draftItem) {
        if (draftItem->kind == ItemKind::Text && draftItem->text.isEmpty()) {
            const QRectF r = draftItem->rect.normalized();
            if (!r.isEmpty()) {
                QPen pen(accentColor, 1.5, Qt::DashLine);
                pen.setCosmetic(true);
                painter.setPen(pen);
                painter.setBrush(Qt::NoBrush);
                painter.drawRect(r);
            }
        } else {
            refs.append(draftItem);
        }
    }
    const RedactRenderContext ctx{nativeBackdrop, backdropScale, backdropLogicalOrigin, false};
    renderFourStagePipeline(painter, logicalViewportRect, refs, ctx);

    // Editor/overlay visual cue: subtle corner L-ticks on hovered or selected Redaction items,
    // inset past the corner resize handles so they stay visible while selected.
    for (const AnnotationItem *it : refs) {
        if (it->kind == ItemKind::Redact && (it->id == hoveredId || it->id == document.selectedId())) {
            const QRectF r = it->rect.normalized();
            const qreal margin = (r.width() >= 24.0 && r.height() >= 24.0) ? 6.0 : 2.0;
            const QRectF inset = r.adjusted(margin, margin, -margin, -margin);
            const qreal tickLen = std::min({7.0, inset.width() * 0.35, inset.height() * 0.35});
            if (tickLen >= 1.5 && inset.isValid() && !inset.isEmpty()) {
                const QList<QLineF> ticks{
                    QLineF(inset.left(), inset.top() + tickLen, inset.left(), inset.top()),
                    QLineF(inset.left(), inset.top(), inset.left() + tickLen, inset.top()),
                    QLineF(inset.right() - tickLen, inset.top(), inset.right(), inset.top()),
                    QLineF(inset.right(), inset.top(), inset.right(), inset.top() + tickLen),
                    QLineF(inset.left(), inset.bottom() - tickLen, inset.left(), inset.bottom()),
                    QLineF(inset.left(), inset.bottom(), inset.left() + tickLen, inset.bottom()),
                    QLineF(inset.right() - tickLen, inset.bottom(), inset.right(), inset.bottom()),
                    QLineF(inset.right(), inset.bottom(), inset.right(), inset.bottom() - tickLen),
                };
                painter.save();
                painter.setRenderHint(QPainter::Antialiasing, false);
                painter.setBrush(Qt::NoBrush);
                if (it->redactionMode != RedactionMode::Solid) {
                    QPen halo(QColor(0, 0, 0, 145), 3.0, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin);
                    halo.setCosmetic(true);
                    painter.setPen(halo);
                    painter.drawLines(ticks);
                }
                QColor tickCol = it->redactionMode == RedactionMode::Solid ? contrastingTextColor(it->color)
                                                                           : QColor(255, 255, 255);
                tickCol.setAlpha(210);
                QPen tickPen(tickCol, 1.0, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin);
                tickPen.setCosmetic(true);
                painter.setPen(tickPen);
                painter.drawLines(ticks);
                painter.restore();
            }
        }
    }

    if (hoveredId > 0 && hoveredId != document.selectedId()) {
        if (const AnnotationItem *hovered = document.findItem(hoveredId)) {
            QColor hoverColor = accentColor;
            hoverColor.setAlpha(170);
            QPen pen(hoverColor, 1.5, Qt::DashLine);
            pen.setCosmetic(true);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(hovered->rawBounds().adjusted(-3.0, -3.0, 3.0, 3.0));
        }
    }

    if (const AnnotationItem *selected = document.selectedItem()) {
        const bool isLine = selected->kind == ItemKind::Arrow
                         || (selected->kind == ItemKind::Step && selected->hasPointer);
        if (!isLine && selected->kind != ItemKind::Step) {
            QPen framePen(accentColor, 1.25, Qt::DashLine);
            framePen.setCosmetic(true);
            painter.setPen(framePen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(selected->rawBounds());
        } else if (selected->kind == ItemKind::Step && !selected->hasPointer) {
            const qreal r = selected->badgeRadius() + 3.0;
            QPen ringPen(accentColor, 1.5, Qt::DashLine);
            ringPen.setCosmetic(true);
            painter.setPen(ringPen);
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(selected->start, r, r);
        }

        const qreal invScale = 1.0 / std::max(0.1, std::hypot(painter.worldTransform().m11(),
                                                              painter.worldTransform().m12()));
        const qreal handleRadius = 4.5 * invScale;
        QPen handlePen(QColor(255, 255, 255, 240), 1.5);
        handlePen.setCosmetic(true);
        painter.setPen(handlePen);
        painter.setBrush(accentColor);
        for (const auto &[handle, pos] : selected->handles()) {
            Q_UNUSED(handle);
            painter.drawEllipse(pos, handleRadius, handleRadius);
        }
    }

    painter.restore();
}

} // namespace itsova
