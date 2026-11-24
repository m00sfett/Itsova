#include "annotationitem.h"

#include "annotationgeometry.h"

#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <limits>

namespace glimply
{
namespace
{

qreal distanceToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const qreal lenSq = ab.x() * ab.x() + ab.y() * ab.y();
    if (lenSq <= 1e-9) {
        return std::hypot(p.x() - a.x(), p.y() - a.y());
    }
    const qreal t = std::clamp(((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / lenSq, 0.0, 1.0);
    const QPointF proj(a.x() + t * ab.x(), a.y() + t * ab.y());
    return std::hypot(p.x() - proj.x(), p.y() - proj.y());
}

QRectF pointsEnvelope(const QList<QPointF> &points)
{
    if (points.isEmpty()) {
        return {};
    }
    qreal minX = points.first().x();
    qreal maxX = minX;
    qreal minY = points.first().y();
    qreal maxY = minY;
    for (qsizetype i = 1; i < points.size(); ++i) {
        minX = std::min(minX, points[i].x());
        maxX = std::max(maxX, points[i].x());
        minY = std::min(minY, points[i].y());
        maxY = std::max(maxY, points[i].y());
    }
    return QRectF(QPointF(minX, minY), QPointF(maxX, maxY));
}

QList<QPair<AnnotationHandle, QPointF>> eightBoxHandles(const QRectF &r)
{
    const QPointF c = r.center();
    return {
        {AnnotationHandle::TopLeft, r.topLeft()},
        {AnnotationHandle::Top, QPointF(c.x(), r.top())},
        {AnnotationHandle::TopRight, r.topRight()},
        {AnnotationHandle::Right, QPointF(r.right(), c.y())},
        {AnnotationHandle::BottomRight, r.bottomRight()},
        {AnnotationHandle::Bottom, QPointF(c.x(), r.bottom())},
        {AnnotationHandle::BottomLeft, r.bottomLeft()},
        {AnnotationHandle::Left, QPointF(r.left(), c.y())},
    };
}

qreal linearizeChannel(qreal channel)
{
    return channel <= 0.03928 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
}

QPolygonF arrowHeadPolygon(const QPointF &start, const QPointF &end, qreal strokeWidth, qreal extraPad = 0.0)
{
    const qreal dx = end.x() - start.x();
    const qreal dy = end.y() - start.y();
    const qreal len = std::hypot(dx, dy);
    if (len < 1e-6) {
        return {};
    }
    const qreal ux = dx / len;
    const qreal uy = dy / len;
    const qreal px = -uy;
    const qreal py = ux;

    const qreal headLength = std::min(len, std::max(12.0, strokeWidth * 3.6)) + extraPad;
    const qreal headHalfWidth = std::max(5.5, strokeWidth * 1.65) + extraPad;

    const QPointF tip(end.x() + ux * extraPad, end.y() + uy * extraPad);
    const QPointF base(tip.x() - ux * headLength, tip.y() - uy * headLength);
    return QPolygonF{
        tip,
        QPointF(base.x() + px * headHalfWidth, base.y() + py * headHalfWidth),
        QPointF(base.x() - px * headHalfWidth, base.y() - py * headHalfWidth),
    };
}

} // namespace

qreal AnnotationItem::badgeRadius() const
{
    return std::max(13.0, 10.0 + strokeWidth);
}

QRectF AnnotationItem::rawBounds() const
{
    switch (kind) {
    case ItemKind::Arrow:
        return QRectF(start, end).normalized();
    case ItemKind::Step: {
        const qreal r = badgeRadius();
        QRectF box(start.x() - r, start.y() - r, 2.0 * r, 2.0 * r);
        if (hasPointer) {
            box = box.united(QRectF(start, end).normalized());
        }
        return box;
    }
    case ItemKind::Pen:
    case ItemKind::Highlighter:
        return pointsEnvelope(points);
    case ItemKind::Rectangle:
    case ItemKind::Ellipse:
    case ItemKind::Text:
    case ItemKind::Redact:
    case ItemKind::Spotlight:
        return rect.normalized();
    }
    return {};
}

QRectF AnnotationItem::boundingRect() const
{
    switch (kind) {
    case ItemKind::Arrow: {
        const qreal headHalfWidth = std::max(5.5, strokeWidth * 1.65);
        const qreal margin = std::max(strokeWidth * 0.5, headHalfWidth) + 2.0;
        return QRectF(start, end).normalized().adjusted(-margin, -margin, margin, margin);
    }
    case ItemKind::Step: {
        const qreal r = badgeRadius() + 3.0;
        QRectF box(start.x() - r, start.y() - r, 2.0 * r, 2.0 * r);
        if (hasPointer) {
            const qreal headHalfWidth = std::max(5.5, strokeWidth * 1.65);
            const qreal margin = std::max(strokeWidth * 0.5, headHalfWidth) + 2.0;
            box = box.united(QRectF(start, end).normalized().adjusted(-margin, -margin, margin, margin));
        }
        return box;
    }
    case ItemKind::Pen:
    case ItemKind::Highlighter: {
        const QRectF env = pointsEnvelope(points);
        if (points.isEmpty()) {
            return {};
        }
        const qreal margin = strokeWidth * 0.5 + 2.0;
        return env.adjusted(-margin, -margin, margin, margin);
    }
    case ItemKind::Rectangle:
    case ItemKind::Ellipse: {
        const qreal margin = strokeWidth * 0.5 + 2.0;
        return rect.normalized().adjusted(-margin, -margin, margin, margin);
    }
    case ItemKind::Spotlight:
        return rect.normalized().adjusted(-2.0, -2.0, 2.0, 2.0);
    case ItemKind::Redact:
        return rect.normalized();
    case ItemKind::Text: {
        const QRectF r = rect.normalized();
        if (legibility == TextLegibility::Pill) {
            return r.adjusted(-8.0, -4.0, 8.0, 4.0);
        }
        if (legibility == TextLegibility::Shadow || legibility == TextLegibility::Outline) {
            return r.adjusted(-4.0, -4.0, 4.0, 4.0);
        }
        return r;
    }
    }
    return {};
}

bool AnnotationItem::intersectsLogicalRect(const QRectF &cropRect) const
{
    if (!cropRect.isValid()) {
        return false;
    }
    const QRectF bounds = boundingRect();
    if (bounds.isEmpty() && bounds.width() == 0.0 && bounds.height() == 0.0) {
        return false;
    }
    // Use a 1-pixel expanded box in case a horizontal/vertical 0-height arrow lies on the crop.
    const QRectF nonEmpty = bounds.adjusted(-0.5, -0.5, 0.5, 0.5);
    return nonEmpty.intersects(cropRect);
}

void AnnotationItem::translate(const QPointF &delta)
{
    switch (kind) {
    case ItemKind::Arrow:
    case ItemKind::Step:
        start += delta;
        end += delta;
        break;
    case ItemKind::Pen:
    case ItemKind::Highlighter:
        for (QPointF &p : points) {
            p += delta;
        }
        break;
    case ItemKind::Rectangle:
    case ItemKind::Ellipse:
    case ItemKind::Text:
    case ItemKind::Redact:
    case ItemKind::Spotlight:
        rect.translate(delta);
        break;
    }
}

QList<QPair<AnnotationHandle, QPointF>> AnnotationItem::handles() const
{
    switch (kind) {
    case ItemKind::Arrow:
        return {
            {AnnotationHandle::StartPoint, start},
            {AnnotationHandle::EndPoint, end},
        };
    case ItemKind::Step:
        if (!hasPointer) {
            return {};
        }
        return {
            {AnnotationHandle::StartPoint, start},
            {AnnotationHandle::EndPoint, end},
        };
    case ItemKind::Pen:
    case ItemKind::Highlighter:
        if (points.size() < 2) {
            return {};
        }
        return eightBoxHandles(pointsEnvelope(points));
    case ItemKind::Text: {
        if (!paragraphMode) {
            return {};
        }
        const QRectF r = rect.normalized();
        return {
            {AnnotationHandle::Left, QPointF(r.left(), r.center().y())},
            {AnnotationHandle::Right, QPointF(r.right(), r.center().y())},
        };
    }
    case ItemKind::Rectangle:
    case ItemKind::Ellipse:
    case ItemKind::Redact:
    case ItemKind::Spotlight:
        return eightBoxHandles(rect.normalized());
    }
    return {};
}

AnnotationHandle AnnotationItem::handleAt(const QPointF &logicalPos, qreal grabRadius) const
{
    AnnotationHandle best = AnnotationHandle::None;
    qreal bestDist = grabRadius;
    for (const auto &[handle, pos] : handles()) {
        const qreal dist = std::hypot(logicalPos.x() - pos.x(), logicalPos.y() - pos.y());
        if (dist <= bestDist) {
            bestDist = dist;
            best = handle;
        }
    }
    return best;
}

bool AnnotationItem::hitTest(const QPointF &logicalPos, qreal tolerance) const
{
    switch (kind) {
    case ItemKind::Arrow: {
        const qreal halfReach = std::max(tolerance, strokeWidth * 0.5 + tolerance);
        if (distanceToSegment(logicalPos, start, end) <= halfReach) {
            return true;
        }
        const QPolygonF head = arrowHeadPolygon(start, end, strokeWidth, tolerance);
        return !head.isEmpty() && head.containsPoint(logicalPos, Qt::OddEvenFill);
    }
    case ItemKind::Step: {
        const qreal badgeDist = std::hypot(logicalPos.x() - start.x(), logicalPos.y() - start.y());
        if (badgeDist <= badgeRadius() + tolerance) {
            return true;
        }
        if (hasPointer) {
            const qreal halfReach = std::max(tolerance, strokeWidth * 0.5 + tolerance);
            if (distanceToSegment(logicalPos, start, end) <= halfReach) {
                return true;
            }
            const QPolygonF head = arrowHeadPolygon(start, end, strokeWidth, tolerance);
            return !head.isEmpty() && head.containsPoint(logicalPos, Qt::OddEvenFill);
        }
        return false;
    }
    case ItemKind::Rectangle: {
        const QRectF r = rect.normalized();
        const qreal halfStroke = strokeWidth * 0.5 + tolerance;
        const QRectF outer = r.adjusted(-halfStroke, -halfStroke, halfStroke, halfStroke);
        if (!outer.contains(logicalPos)) {
            return false;
        }
        if (fillMode == ShapeFillMode::TranslucentFill) {
            return true;
        }
        const QRectF inner = r.adjusted(halfStroke, halfStroke, -halfStroke, -halfStroke);
        return !inner.isValid() || !inner.contains(logicalPos);
    }
    case ItemKind::Ellipse: {
        const QRectF r = rect.normalized();
        if (r.isEmpty()) {
            return false;
        }
        const qreal halfStroke = strokeWidth * 0.5 + tolerance;
        if (fillMode == ShapeFillMode::TranslucentFill) {
            QPainterPath filled;
            filled.addEllipse(r.adjusted(-halfStroke, -halfStroke, halfStroke, halfStroke));
            return filled.contains(logicalPos);
        }
        QPainterPath outline;
        outline.addEllipse(r);
        QPainterPathStroker stroker;
        stroker.setWidth(std::max(strokeWidth, 1.0) + 2.0 * tolerance);
        return stroker.createStroke(outline).contains(logicalPos);
    }
    case ItemKind::Pen:
    case ItemKind::Highlighter: {
        if (points.isEmpty()) {
            return false;
        }
        if (points.size() == 1) {
            const QPointF p = points.first();
            return std::hypot(logicalPos.x() - p.x(), logicalPos.y() - p.y()) <= strokeWidth * 0.5 + tolerance;
        }
        const QPainterPath path = smoothPolylinePath(points);
        QPainterPathStroker stroker;
        stroker.setWidth(std::max(strokeWidth, 1.0) + 2.0 * tolerance);
        stroker.setCapStyle(Qt::RoundCap);
        stroker.setJoinStyle(Qt::RoundJoin);
        return stroker.createStroke(path).contains(logicalPos);
    }
    case ItemKind::Spotlight: {
        const QRectF r = rect.normalized();
        const qreal margin = std::max(tolerance, 8.0);
        if (spotlightShape == SpotlightShape::Ellipse) {
            QPainterPath area;
            area.addEllipse(r.adjusted(-margin, -margin, margin, margin));
            return area.contains(logicalPos);
        }
        return r.adjusted(-margin, -margin, margin, margin).contains(logicalPos);
    }
    case ItemKind::Redact:
        return rect.normalized().adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(logicalPos);
    case ItemKind::Text:
        return rect.normalized().adjusted(-4.0, -4.0, 4.0, 4.0).contains(logicalPos);
    }
    return false;
}

QColor contrastingTextColor(const QColor &background)
{
    const qreal lum = 0.2126 * linearizeChannel(background.redF())
                    + 0.7152 * linearizeChannel(background.greenF())
                    + 0.0722 * linearizeChannel(background.blueF());
    // Crossover at lum = sqrt(1.05 * 0.05) - 0.05 (~0.1791) guarantees >= 4.58:1 contrast
    // against either pure white or pure black across the entire sRGB color space.
    const qreal contrastWhite = 1.05 / (lum + 0.05);
    const qreal contrastBlack = (lum + 0.05) / 0.05;
    return contrastBlack >= contrastWhite ? QColor(0x00, 0x00, 0x00) : QColor(0xFF, 0xFF, 0xFF);
}

} // namespace glimply
