#include "annotationgeometry.h"

#include "annotationtextlayout.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace itsova
{
namespace
{

qreal pointToSegmentDist(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const qreal lenSq = ab.x() * ab.x() + ab.y() * ab.y();
    if (lenSq <= 1e-12) {
        return std::hypot(p.x() - a.x(), p.y() - a.y());
    }
    const qreal t = std::clamp(((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / lenSq, 0.0, 1.0);
    const QPointF proj(a.x() + t * ab.x(), a.y() + t * ab.y());
    return std::hypot(p.x() - proj.x(), p.y() - proj.y());
}

void rdpRecursive(const QList<QPointF> &points, qsizetype first, qsizetype last, qreal epsilon,
                  QList<bool> &keep)
{
    if (last <= first + 1) {
        return;
    }
    qreal maxDist = -1.0;
    qsizetype maxIndex = -1;
    for (qsizetype i = first + 1; i < last; ++i) {
        const qreal d = pointToSegmentDist(points[i], points[first], points[last]);
        if (d > maxDist) {
            maxDist = d;
            maxIndex = i;
        }
    }
    if (maxDist > epsilon && maxIndex >= 0) {
        keep[maxIndex] = true;
        rdpRecursive(points, first, maxIndex, epsilon, keep);
        rdpRecursive(points, maxIndex, last, epsilon, keep);
    }
}

QRectF pointsBox(const QList<QPointF> &points)
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

} // namespace

QRectF constrainedBox(const QPointF &anchor, const QPointF &current, DrawModifiers mods, const QRectF &bounds)
{
    const bool hasBounds = bounds.isValid();
    const QPointF clampedAnchor = hasBounds
        ? QPointF(std::clamp(anchor.x(), bounds.left(), bounds.right()),
                  std::clamp(anchor.y(), bounds.top(), bounds.bottom()))
        : anchor;

    if (mods.fromCenter) {
        qreal hx = std::abs(current.x() - clampedAnchor.x());
        qreal hy = std::abs(current.y() - clampedAnchor.y());
        if (hasBounds) {
            const qreal maxHx = std::max(0.0, std::min(clampedAnchor.x() - bounds.left(), bounds.right() - clampedAnchor.x()));
            const qreal maxHy = std::max(0.0, std::min(clampedAnchor.y() - bounds.top(), bounds.bottom() - clampedAnchor.y()));
            hx = std::min(hx, maxHx);
            hy = std::min(hy, maxHy);
            if (mods.constrain) {
                const qreal h = std::min(std::max(hx, hy), std::min(maxHx, maxHy));
                hx = h;
                hy = h;
            }
        } else if (mods.constrain) {
            const qreal h = std::max(hx, hy);
            hx = h;
            hy = h;
        }
        return QRectF(clampedAnchor.x() - hx, clampedAnchor.y() - hy, 2.0 * hx, 2.0 * hy);
    }

    const qreal dx = current.x() - clampedAnchor.x();
    const qreal dy = current.y() - clampedAnchor.y();
    const qreal signX = dx < 0.0 ? -1.0 : 1.0;
    const qreal signY = dy < 0.0 ? -1.0 : 1.0;
    qreal w = std::abs(dx);
    qreal h = std::abs(dy);

    if (hasBounds) {
        const qreal maxW = std::max(0.0, signX > 0.0 ? bounds.right() - clampedAnchor.x() : clampedAnchor.x() - bounds.left());
        const qreal maxH = std::max(0.0, signY > 0.0 ? bounds.bottom() - clampedAnchor.y() : clampedAnchor.y() - bounds.top());
        if (mods.constrain) {
            const qreal side = std::min(std::max(w, h), std::min(maxW, maxH));
            w = side;
            h = side;
        } else {
            w = std::min(w, maxW);
            h = std::min(h, maxH);
        }
    } else if (mods.constrain) {
        const qreal side = std::max(w, h);
        w = side;
        h = side;
    }

    const QPointF opposite(clampedAnchor.x() + signX * w, clampedAnchor.y() + signY * h);
    return QRectF(clampedAnchor, opposite).normalized();
}

QRectF resizedBox(const QRectF &startRect, AnnotationHandle handle, const QPointF &delta, DrawModifiers mods,
                  qreal minSide)
{
    const QRectF start = startRect.normalized();
    const bool moveL = handle == AnnotationHandle::TopLeft || handle == AnnotationHandle::Left
                    || handle == AnnotationHandle::BottomLeft;
    const bool moveR = handle == AnnotationHandle::TopRight || handle == AnnotationHandle::Right
                    || handle == AnnotationHandle::BottomRight;
    const bool moveT = handle == AnnotationHandle::TopLeft || handle == AnnotationHandle::Top
                    || handle == AnnotationHandle::TopRight;
    const bool moveB = handle == AnnotationHandle::BottomLeft || handle == AnnotationHandle::Bottom
                    || handle == AnnotationHandle::BottomRight;

    const bool moveH = moveL || moveR;
    const bool moveV = moveT || moveB;
    if (!moveH && !moveV) {
        return start;
    }

    if (mods.fromCenter) {
        const QPointF center = start.center();
        qreal halfW = start.width() * 0.5;
        qreal halfH = start.height() * 0.5;
        if (moveH) {
            const qreal movedX = (moveL ? start.left() : start.right()) + delta.x();
            halfW = std::max(minSide * 0.5, std::abs(movedX - center.x()));
        }
        if (moveV) {
            const qreal movedY = (moveT ? start.top() : start.bottom()) + delta.y();
            halfH = std::max(minSide * 0.5, std::abs(movedY - center.y()));
        }
        if (mods.constrain) {
            const qreal h = (moveH && moveV) ? std::max(halfW, halfH) : (moveH ? halfW : halfH);
            halfW = h;
            halfH = h;
        }
        return QRectF(center.x() - halfW, center.y() - halfH, 2.0 * halfW, 2.0 * halfH);
    }

    const qreal anchorX = moveL ? start.right() : (moveR ? start.left() : start.center().x());
    const qreal anchorY = moveT ? start.bottom() : (moveB ? start.top() : start.center().y());

    qreal w = start.width();
    qreal h = start.height();
    qreal signX = moveL ? -1.0 : 1.0;
    qreal signY = moveT ? -1.0 : 1.0;

    if (moveH) {
        const qreal movedX = (moveL ? start.left() : start.right()) + delta.x();
        const qreal diff = movedX - anchorX;
        if (std::abs(diff) >= minSide) {
            signX = diff < 0.0 ? -1.0 : 1.0;
            w = std::abs(diff);
        } else {
            signX = diff == 0.0 ? (moveL ? -1.0 : 1.0) : (diff < 0.0 ? -1.0 : 1.0);
            w = minSide;
        }
    }
    if (moveV) {
        const qreal movedY = (moveT ? start.top() : start.bottom()) + delta.y();
        const qreal diff = movedY - anchorY;
        if (std::abs(diff) >= minSide) {
            signY = diff < 0.0 ? -1.0 : 1.0;
            h = std::abs(diff);
        } else {
            signY = diff == 0.0 ? (moveT ? -1.0 : 1.0) : (diff < 0.0 ? -1.0 : 1.0);
            h = minSide;
        }
    }

    if (mods.constrain) {
        const qreal side = (moveH && moveV) ? std::max(w, h) : (moveH ? w : h);
        w = side;
        h = side;
    }

    qreal left = start.left();
    qreal top = start.top();
    if (moveH) {
        left = signX > 0.0 ? anchorX : anchorX - w;
    } else if (mods.constrain) {
        left = anchorX - w * 0.5;
    }
    if (moveV) {
        top = signY > 0.0 ? anchorY : anchorY - h;
    } else if (mods.constrain) {
        top = anchorY - h * 0.5;
    }
    return QRectF(left, top, w, h);
}

AnnotationItem applyHandleDrag(const AnnotationItem &startItem, AnnotationHandle handle, const QPointF &delta,
                               DrawModifiers mods)
{
    AnnotationItem item = startItem;
    switch (startItem.kind) {
    case ItemKind::Arrow:
    case ItemKind::Step:
        if (handle == AnnotationHandle::StartPoint) {
            const QPointF candidate = startItem.start + delta;
            item.start = mods.constrain ? snapAngle15(startItem.end, candidate) : candidate;
        } else if (handle == AnnotationHandle::EndPoint) {
            const QPointF candidate = startItem.end + delta;
            item.end = mods.constrain ? snapAngle15(startItem.start, candidate) : candidate;
        }
        break;
    case ItemKind::Text:
        if (handle == AnnotationHandle::Left || handle == AnnotationHandle::Right) {
            item.rect = resizedBox(startItem.rect, handle, delta, DrawModifiers{false, mods.fromCenter}, 60.0);
            item.rect.setSize(measureAnnotationText(item));
        }
        break;
    case ItemKind::Pen:
    case ItemKind::Highlighter: {
        const QRectF oldBox = pointsBox(startItem.points);
        if (oldBox.width() < 1e-3 && oldBox.height() < 1e-3) {
            item.translate(delta);
            break;
        }
        const QRectF padded(oldBox.left(), oldBox.top(), std::max(oldBox.width(), 1.0), std::max(oldBox.height(), 1.0));
        const QRectF newBox = resizedBox(padded, handle, delta, mods, 4.0);
        for (qsizetype i = 0; i < item.points.size(); ++i) {
            const QPointF &p = startItem.points[i];
            const qreal rx = oldBox.width() > 1e-3 ? (p.x() - oldBox.left()) / oldBox.width() : 0.5;
            const qreal ry = oldBox.height() > 1e-3 ? (p.y() - oldBox.top()) / oldBox.height() : 0.5;
            item.points[i] = QPointF(newBox.left() + rx * newBox.width(), newBox.top() + ry * newBox.height());
        }
        break;
    }
    case ItemKind::Rectangle:
    case ItemKind::Ellipse:
    case ItemKind::Redact:
    case ItemKind::Spotlight:
        item.rect = resizedBox(startItem.rect, handle, delta, mods, 4.0);
        break;
    }
    return item;
}

QPointF snapAngle15(const QPointF &origin, const QPointF &target)
{
    const qreal dx = target.x() - origin.x();
    const qreal dy = target.y() - origin.y();
    const qreal len = std::hypot(dx, dy);
    if (len < 1e-6) {
        return target;
    }
    constexpr qreal step = 3.14159265358979323846 / 12.0; // 15 degrees
    const qreal angle = std::atan2(dy, dx);
    const qreal snapped = std::round(angle / step) * step;
    return QPointF(origin.x() + len * std::cos(snapped), origin.y() + len * std::sin(snapped));
}

QList<QPointF> simplifyPolyline(const QList<QPointF> &points, qreal epsilon)
{
    if (points.size() <= 2 || epsilon <= 0.0) {
        return points;
    }
    QList<bool> keep(points.size(), false);
    keep.first() = true;
    keep.last() = true;
    rdpRecursive(points, 0, points.size() - 1, epsilon, keep);

    QList<QPointF> out;
    out.reserve(points.size());
    for (qsizetype i = 0; i < points.size(); ++i) {
        if (keep[i]) {
            out.append(points[i]);
        }
    }
    return out;
}

QPainterPath smoothPolylinePath(const QList<QPointF> &points)
{
    QPainterPath path;
    if (points.isEmpty()) {
        return path;
    }
    if (points.size() == 1) {
        path.moveTo(points.first());
        path.lineTo(points.first() + QPointF(0.01, 0.0));
        return path;
    }
    if (points.size() == 2) {
        path.moveTo(points[0]);
        path.lineTo(points[1]);
        return path;
    }
    path.moveTo(points.first());
    const QPointF firstMid = (points[0] + points[1]) * 0.5;
    path.lineTo(firstMid);
    for (qsizetype i = 1; i + 1 < points.size(); ++i) {
        const QPointF mid = (points[i] + points[i + 1]) * 0.5;
        path.quadTo(points[i], mid);
    }
    path.lineTo(points.last());
    return path;
}

} // namespace itsova
