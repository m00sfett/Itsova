#include "selectiongeometry.h"

#include <QPair>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace glimply
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

} // namespace glimply
