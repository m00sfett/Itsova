#include "selectiongeometry.h"

#include <QPair>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace itsova
{
namespace
{

// One dimension of a selection, described by what stays fixed: an edge (one-sided, growing in the
// direction of sign) or the centre. Coordinates are edge positions, so right and bottom are
// exclusive and a centre can sit between two pixels.
struct Axis {
    double anchor = 0;
    int side = 0;
    int sign = 1;
    bool centred = false;

    // Longest side that still fits into [0, limit] around the anchor.
    [[nodiscard]] int maxSide(int limit) const
    {
        if (centred) {
            return static_cast<int>(std::floor(2.0 * std::min(anchor, limit - anchor)));
        }
        return static_cast<int>(std::floor(sign > 0 ? limit - anchor : anchor));
    }

    [[nodiscard]] int low(int limit) const
    {
        double start = 0;
        if (centred) {
            start = anchor - side / 2.0;
        } else {
            start = sign > 0 ? anchor : anchor - side;
        }
        // Rounding a half-pixel centre may push a full-size side over the border; shift it back.
        return std::clamp(static_cast<int>(std::lround(start)), 0, std::max(0, limit - side));
    }
};

Axis drawnAxis(int anchor, int position, bool fromCentre)
{
    const int distance = position - anchor;
    Axis axis;
    axis.anchor = anchor;
    axis.centred = fromCentre;
    axis.side = fromCentre ? 2 * std::abs(distance) : std::abs(distance);
    axis.sign = distance < 0 ? -1 : 1;
    return axis;
}

// low and high are edge positions of the start rectangle on this axis.
Axis resizedAxis(int low, int high, bool lowMoves, bool highMoves, int delta, SelectionConstraints constraints)
{
    Axis axis;
    const double centre = (low + high) / 2.0;
    if (!lowMoves && !highMoves) {
        if (constraints.square) {
            // The other dimension follows the moving one, around this dimension's centre.
            axis.anchor = centre;
            axis.centred = true;
        } else {
            axis.anchor = low;
        }
        axis.side = high - low;
        return axis;
    }
    const int moved = (lowMoves ? low : high) + delta;
    if (constraints.fromCentre) {
        axis.anchor = centre;
        axis.centred = true;
        axis.side = static_cast<int>(std::lround(2.0 * std::abs(moved - centre)));
        return axis;
    }
    axis.anchor = lowMoves ? high : low;
    const int distance = moved - static_cast<int>(axis.anchor);
    axis.side = std::abs(distance);
    axis.sign = distance < 0 ? -1 : 1;
    return axis;
}

QRect fit(Axis x, Axis y, bool square, int squareSide, const QSize &bounds)
{
    if (square) {
        const int side = std::clamp(squareSide, 0, std::min(x.maxSide(bounds.width()), y.maxSide(bounds.height())));
        x.side = side;
        y.side = side;
    } else {
        x.side = std::clamp(x.side, 0, x.maxSide(bounds.width()));
        y.side = std::clamp(y.side, 0, y.maxSide(bounds.height()));
    }
    return QRect(x.low(bounds.width()), y.low(bounds.height()), x.side, y.side);
}

// Largest value <= limit whose parity matches reference.
int floorWithParity(int limit, int reference)
{
    return (limit - reference) % 2 == 0 ? limit : limit - 1;
}

// Smallest value >= minimum whose parity matches reference.
int ceilWithParity(int minimum, int reference)
{
    return (minimum - reference) % 2 == 0 ? minimum : minimum + 1;
}

// One axis resized symmetrically: low and side of the result. The side keeps its parity, so the
// centre (low + side / 2) does not move by half a pixel.
QPair<int, int> centredStep(int low, int side, int delta, int minimum, int limit)
{
    const int doubledCentre = 2 * low + side;
    const int largest = floorWithParity(std::min(doubledCentre, 2 * limit - doubledCentre), side);
    const int smallest = ceilWithParity(minimum, side);
    if (smallest > largest) {
        return {low, side};
    }
    const int wanted = std::clamp(side + 2 * delta, smallest, std::max(largest, side));
    const int result = std::min(wanted, std::max(largest, side));
    return {(doubledCentre - result) / 2, result};
}

} // namespace

QRect drawnSelection(const QPoint &anchor, const QPoint &position, SelectionConstraints constraints, const QSize &bounds)
{
    const Axis x = drawnAxis(anchor.x(), position.x(), constraints.fromCentre);
    const Axis y = drawnAxis(anchor.y(), position.y(), constraints.fromCentre);
    return fit(x, y, constraints.square, std::max(x.side, y.side), bounds);
}

QRect resizedSelection(const QRect &start, Qt::Edges edges, const QPoint &delta, SelectionConstraints constraints,
                       const QSize &bounds)
{
    const bool horizontal = edges & (Qt::LeftEdge | Qt::RightEdge);
    const bool vertical = edges & (Qt::TopEdge | Qt::BottomEdge);
    const Axis x = resizedAxis(start.left(), start.left() + start.width(), edges & Qt::LeftEdge,
                               edges & Qt::RightEdge, delta.x(), constraints);
    const Axis y = resizedAxis(start.top(), start.top() + start.height(), edges & Qt::TopEdge,
                               edges & Qt::BottomEdge, delta.y(), constraints);
    int squareSide = 0;
    if (horizontal && vertical) {
        squareSide = std::max(x.side, y.side);
    } else {
        squareSide = horizontal ? x.side : y.side;
    }
    return fit(x, y, constraints.square, squareSide, bounds);
}

QRect steppedSelection(const QRect &rect, const QPoint &step, SelectionConstraints constraints, int minimum,
                       const QSize &bounds)
{
    if (!constraints.fromCentre && !constraints.square) {
        QRect moved = rect.translated(step);
        moved.moveLeft(std::clamp(moved.left(), 0, std::max(0, bounds.width() - moved.width())));
        moved.moveTop(std::clamp(moved.top(), 0, std::max(0, bounds.height() - moved.height())));
        return moved;
    }
    const int delta = step.x() + step.y();
    if (!constraints.square) {
        QRect result = rect;
        if (step.x() != 0) {
            const auto [left, width] = centredStep(rect.left(), rect.width(), step.x(), minimum, bounds.width());
            result.setRect(left, rect.top(), width, rect.height());
        } else if (step.y() != 0) {
            const auto [top, height] = centredStep(rect.top(), rect.height(), step.y(), minimum, bounds.height());
            result.setRect(rect.left(), top, rect.width(), height);
        }
        return result;
    }
    if (!constraints.fromCentre) {
        const int largest = std::min(bounds.width() - rect.left(), bounds.height() - rect.top());
        if (largest < minimum) {
            return rect;
        }
        const int side = std::clamp(rect.width() + delta, minimum, largest);
        return QRect(rect.left(), rect.top(), side, side);
    }
    // Centred square: the width keeps its exact centre; the height follows around its own centre.
    const int doubledX = 2 * rect.left() + rect.width();
    const int doubledY = 2 * rect.top() + rect.height();
    const int roomX = std::min(doubledX, 2 * bounds.width() - doubledX);
    const int roomY = std::min(doubledY, 2 * bounds.height() - doubledY);
    const int largest = floorWithParity(std::min(roomX, roomY), rect.width());
    const int smallest = ceilWithParity(minimum, rect.width());
    if (smallest > largest) {
        return rect;
    }
    const int side = std::clamp(rect.width() + 2 * delta, smallest, largest);
    const int top = std::clamp((doubledY - side) / 2, 0, std::max(0, bounds.height() - side));
    return QRect((doubledX - side) / 2, top, side, side);
}

QRect clampToBounds(QRect rect, const QRect &bounds)
{
    rect = rect.normalized();
    if (bounds.isEmpty()) {
        return rect;
    }
    rect.setWidth(std::min(rect.width(), bounds.width()));
    rect.setHeight(std::min(rect.height(), bounds.height()));
    rect.moveLeft(std::clamp(rect.left(), bounds.left(), bounds.left() + bounds.width() - rect.width()));
    rect.moveTop(std::clamp(rect.top(), bounds.top(), bounds.top() + bounds.height() - rect.height()));
    return rect;
}

QRect drawnSelection(const QPoint &anchor, const QPoint &position, SelectionConstraints constraints, const QRect &bounds)
{
    if (bounds.isEmpty()) {
        return {};
    }
    const QPoint origin = bounds.topLeft();
    return drawnSelection(anchor - origin, position - origin, constraints, bounds.size()).translated(origin);
}

QRect resizedSelection(const QRect &start, Qt::Edges edges, const QPoint &delta, SelectionConstraints constraints,
                       const QRect &bounds)
{
    if (bounds.isEmpty()) {
        return {};
    }
    const QPoint origin = bounds.topLeft();
    return resizedSelection(start.translated(-origin), edges, delta, constraints, bounds.size()).translated(origin);
}

QRect steppedSelection(const QRect &rect, const QPoint &step, SelectionConstraints constraints, int minimum,
                       const QRect &bounds)
{
    if (bounds.isEmpty()) {
        return rect;
    }
    const QPoint origin = bounds.topLeft();
    return steppedSelection(rect.translated(-origin), step, constraints, minimum, bounds.size()).translated(origin);
}

namespace
{

int snapAxisCoordinate(int value, const QList<int> &candidates, int threshold)
{
    if (threshold <= 0 || candidates.isEmpty()) {
        return value;
    }
    int bestValue = value;
    int bestDistance = threshold + 1;
    for (const int candidate : candidates) {
        const int dist = std::abs(value - candidate);
        if (dist < bestDistance) {
            bestDistance = dist;
            bestValue = candidate;
        }
    }
    return bestDistance <= threshold ? bestValue : value;
}

} // namespace

QPoint snapPointToMonitorEdges(const QPoint &pos, const QList<QRect> &screenRects, int threshold)
{
    if (threshold <= 0 || screenRects.isEmpty()) {
        return pos;
    }
    QList<int> xEdges;
    QList<int> yEdges;
    xEdges.reserve(screenRects.size() * 2);
    yEdges.reserve(screenRects.size() * 2);
    for (const QRect &screen : screenRects) {
        xEdges.append(screen.left());
        xEdges.append(screen.left() + screen.width());
        yEdges.append(screen.top());
        yEdges.append(screen.top() + screen.height());
    }
    return QPoint(snapAxisCoordinate(pos.x(), xEdges, threshold), snapAxisCoordinate(pos.y(), yEdges, threshold));
}

QPoint snapResizeDeltaToMonitorEdges(const QRect &start, Qt::Edges edges, const QPoint &delta,
                                     const QList<QRect> &screenRects, int threshold)
{
    if (threshold <= 0 || screenRects.isEmpty()) {
        return delta;
    }
    QList<int> xEdges;
    QList<int> yEdges;
    xEdges.reserve(screenRects.size() * 2);
    yEdges.reserve(screenRects.size() * 2);
    for (const QRect &screen : screenRects) {
        xEdges.append(screen.left());
        xEdges.append(screen.left() + screen.width());
        yEdges.append(screen.top());
        yEdges.append(screen.top() + screen.height());
    }

    QPoint out = delta;
    if (edges & Qt::LeftEdge) {
        const int targetX = start.left() + delta.x();
        out.rx() += snapAxisCoordinate(targetX, xEdges, threshold) - targetX;
    } else if (edges & Qt::RightEdge) {
        const int targetX = start.left() + start.width() + delta.x();
        out.rx() += snapAxisCoordinate(targetX, xEdges, threshold) - targetX;
    }
    if (edges & Qt::TopEdge) {
        const int targetY = start.top() + delta.y();
        out.ry() += snapAxisCoordinate(targetY, yEdges, threshold) - targetY;
    } else if (edges & Qt::BottomEdge) {
        const int targetY = start.top() + start.height() + delta.y();
        out.ry() += snapAxisCoordinate(targetY, yEdges, threshold) - targetY;
    }
    return out;
}

QRect snapMovedRectToMonitorEdges(const QRect &rect, const QList<QRect> &screenRects, const QRect &desktopBounds,
                                  int threshold)
{
    QRect out = clampToBounds(rect, desktopBounds);
    if (threshold <= 0 || screenRects.isEmpty()) {
        return out;
    }
    int bestDx = 0;
    int bestXDist = threshold + 1;
    int bestDy = 0;
    int bestYDist = threshold + 1;

    const int left = out.left();
    const int right = out.left() + out.width();
    const int top = out.top();
    const int bottom = out.top() + out.height();

    for (const QRect &screen : screenRects) {
        const int sLeft = screen.left();
        const int sRight = screen.left() + screen.width();
        for (const int edgeX : {sLeft, sRight}) {
            if (const int d = std::abs(left - edgeX); d < bestXDist) {
                bestXDist = d;
                bestDx = edgeX - left;
            }
            if (const int d = std::abs(right - edgeX); d < bestXDist) {
                bestXDist = d;
                bestDx = edgeX - right;
            }
        }
        const int sTop = screen.top();
        const int sBottom = screen.top() + screen.height();
        for (const int edgeY : {sTop, sBottom}) {
            if (const int d = std::abs(top - edgeY); d < bestYDist) {
                bestYDist = d;
                bestDy = edgeY - top;
            }
            if (const int d = std::abs(bottom - edgeY); d < bestYDist) {
                bestYDist = d;
                bestDy = edgeY - bottom;
            }
        }
    }

    if (bestXDist <= threshold) {
        out.translate(bestDx, 0);
    }
    if (bestYDist <= threshold) {
        out.translate(0, bestDy);
    }
    return clampToBounds(out, desktopBounds);
}

QPoint placeOverlayPanel(const QSize &panelSize, const QSize &overlaySize, const QRect &selection,
                         const std::optional<QPoint> &userPosition, int margin)
{
    const int maxX = std::max(0, overlaySize.width() - panelSize.width());
    const int maxY = std::max(0, overlaySize.height() - panelSize.height());
    if (userPosition) {
        return {std::clamp(userPosition->x(), 0, maxX), std::clamp(userPosition->y(), 0, maxY)};
    }
    const int x = maxX / 2;
    const QPoint top(x, std::min(margin, maxY));
    if (!selection.isValid()) {
        return top;
    }
    const QPoint bottom(x, std::max(0, maxY - margin));
    const auto covered = [&](const QPoint &at) {
        const QRect overlap = QRect(at, panelSize).adjusted(-margin, -margin, margin, margin).intersected(selection);
        return overlap.isEmpty() ? qint64(0) : qint64(overlap.width()) * overlap.height();
    };
    const qint64 atTop = covered(top);
    if (atTop == 0) {
        return top;
    }
    return covered(bottom) < atTop ? bottom : top;
}

QRect overlayCloseButtonRect(const QRect &globalRect, const QRect &bounds, int buttonSize,
                             Qt::LayoutDirection direction)
{
    QRect button(0, 0, buttonSize, buttonSize);
    // Inside the trailing top corner, clear of the corner handle; outside on small rectangles.
    constexpr int inset = 20;
    const bool rtl = direction == Qt::RightToLeft;
    if (globalRect.width() >= 3 * inset && globalRect.height() >= 3 * inset) {
        const int cx = rtl ? globalRect.left() + inset : globalRect.right() - inset;
        button.moveCenter(QPoint(cx, globalRect.top() + inset));
    } else {
        const int cx = rtl ? globalRect.left() - buttonSize / 2 - 4 : globalRect.right() + buttonSize / 2 + 4;
        button.moveCenter(QPoint(cx, globalRect.top() - buttonSize / 2 - 4));
    }
    return clampToBounds(button, bounds);
}

QPointF overlaySelectionBadgeCenter(const QRectF &frame, Qt::LayoutDirection direction)
{
    return direction == Qt::RightToLeft ? frame.topRight() + QPointF(-14, 14) : frame.topLeft() + QPointF(14, 14);
}

Qt::Edges draggedEdges(const QPoint &anchor, const QPoint &pointer)
{
    return Qt::Edges(pointer.x() >= anchor.x() ? Qt::RightEdge : Qt::LeftEdge)
         | (pointer.y() >= anchor.y() ? Qt::BottomEdge : Qt::TopEdge);
}

QMargins nudgedMargins(const QMargins &base, Qt::Edges edges, const QPoint &delta)
{
    QMargins result = base;
    if (edges.testFlag(Qt::LeftEdge)) {
        result.setLeft(result.left() + delta.x());
    }
    if (edges.testFlag(Qt::RightEdge)) {
        result.setRight(result.right() + delta.x());
    }
    if (edges.testFlag(Qt::TopEdge)) {
        result.setTop(result.top() + delta.y());
    }
    if (edges.testFlag(Qt::BottomEdge)) {
        result.setBottom(result.bottom() + delta.y());
    }
    return result;
}

QRect nudgedNativeArea(const QRect &nativeArea, const QMargins &nudge)
{
    if (nativeArea.isNull()) {
        return nativeArea;
    }
    const int left = nativeArea.left() + nudge.left();
    const int top = nativeArea.top() + nudge.top();
    const int right = nativeArea.left() + nativeArea.width() + nudge.right();
    const int bottom = nativeArea.top() + nativeArea.height() + nudge.bottom();
    return QRect(left, top, std::max(1, right - left), std::max(1, bottom - top));
}

void NudgeDeadZone::keyPressed(const QPoint &pointer)
{
    active_ = true;
    keyPointer_ = pointer;
}

std::optional<QPoint> NudgeDeadZone::filter(const QPoint &pointer)
{
    if (active_) {
        const QPoint moved = pointer - keyPointer_;
        if (moved.x() * moved.x() + moved.y() * moved.y() <= nudgeDeadZonePx * nudgeDeadZonePx) {
            return std::nullopt;
        }
        // Take over from where the corner is: the movement that left the zone does not count.
        active_ = false;
        offset_ -= moved;
    }
    return pointer + offset_;
}

void NudgeDeadZone::reset()
{
    active_ = false;
    offset_ = {};
    keyPointer_ = {};
}

} // namespace itsova
