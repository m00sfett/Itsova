#include "selectiongeometry.h"

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

} // namespace glimply
