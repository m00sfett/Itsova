#include "annotationrenderer.h"

#include "annotationgeometry.h"

#include <QFontMetricsF>
#include <QPainterPath>
#include <QPolygonF>
#include <QTransform>

#include <algorithm>
#include <cmath>

namespace glimply
{
namespace
{

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

void drawRedactItem(QPainter &painter, const AnnotationItem &item)
{
    const QRectF r = item.rect.normalized();
    if (r.isEmpty()) {
        return;
    }
    QColor opaque = item.color;
    opaque.setAlpha(255);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.setPen(Qt::NoPen);
    painter.setBrush(opaque);
    painter.drawRect(r);
    painter.restore();
}

void drawNonSpotlightItem(QPainter &painter, const QRectF &logicalClipRect, const AnnotationItem &item)
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
        drawRedactItem(painter, item);
        break;
    case ItemKind::Text: {
        if (item.text.isEmpty()) {
            break;
        }
        painter.save();
        painter.setFont(item.font);
        painter.setPen(item.color);
        const int flags = int(item.textAlignment) | Qt::TextWordWrap;
        painter.drawText(item.rect.normalized(), flags, item.text);
        painter.restore();
        break;
    }
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
                             const QList<const AnnotationItem *> &items)
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
        drawNonSpotlightItem(painter, logicalClipRect, *item);
    }

    // Stage 3: unified spotlight dim layer.
    drawUnifiedSpotlights(painter, logicalClipRect, spotlights);

    // Stage 4: remaining non-redaction callout items above the spotlight dim layer.
    for (qsizetype i = lastRedactIdx + 1; i < items.size(); ++i) {
        const AnnotationItem *item = items[i];
        if (item->kind == ItemKind::Spotlight || !item->intersectsLogicalRect(logicalClipRect)) {
            continue;
        }
        drawNonSpotlightItem(painter, logicalClipRect, *item);
    }
}

} // namespace

QImage renderAnnotatedCrop(const QImage &nativeCrop, const QRectF &logicalCropRect, qreal nativeScale,
                           const AnnotationDocument &document)
{
    if (nativeCrop.isNull() || !logicalCropRect.isValid() || nativeScale <= 0.0) {
        return nativeCrop;
    }

    QImage result = nativeCrop.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    result.setDevicePixelRatio(1.0);
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

    renderFourStagePipeline(painter, logicalCropRect, refs);
    return result;
}

void paintAnnotationOverlay(QPainter &painter, const QRectF &logicalViewportRect, const AnnotationDocument &document,
                            const QColor &accentColor, int hoveredId, const AnnotationItem *draftItem,
                            const QImage *, qreal, const QPointF &)
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
        refs.append(&item);
    }
    if (draftItem) {
        refs.append(draftItem);
    }
    renderFourStagePipeline(painter, logicalViewportRect, refs);

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

} // namespace glimply
