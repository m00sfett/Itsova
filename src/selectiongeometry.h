#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>

namespace glimply
{

// Modifiers that shape a selection while it is drawn or resized.
struct SelectionConstraints {
    // The rectangle grows symmetrically around its centre (Alt).
    bool fromCentre = false;
    // Width and height stay equal (Shift).
    bool square = false;
};

// A new selection dragged from anchor to position. Without fromCentre the anchor is a corner,
// with it the anchor is the centre. The result always lies inside bounds; constraints survive the
// clamping (a centred square stays a centred square, only smaller).
[[nodiscard]] QRect drawnSelection(const QPoint &anchor, const QPoint &position, SelectionConstraints constraints,
                                   const QSize &bounds);

// start resized by moving the given edges by delta. With fromCentre the opposite edges move by
// the mirrored amount. With square a corner takes the longer side for both; a single edge sets
// the other side to the same length, centred on its previous centre. Dragging past the opposite
// edge flips the rectangle.
[[nodiscard]] QRect resizedSelection(const QRect &start, Qt::Edges edges, const QPoint &delta,
                                     SelectionConstraints constraints, const QSize &bounds);

// rect changed by one arrow key press; step is the signed distance on one axis (the other is 0).
// Without constraints the rectangle moves. With fromCentre it grows (right, down) or shrinks
// (left, up) by step on both sides of that axis, so its centre stays exactly where it is. With
// square it becomes a square of its width that grows or shrinks by step, from the top left corner,
// or by step on every side around its centre with fromCentre. The size never drops below minimum
// and the rectangle never leaves bounds; at the border a centred rectangle stops growing instead of
// moving its centre.
[[nodiscard]] QRect steppedSelection(const QRect &rect, const QPoint &step, SelectionConstraints constraints,
                                     int minimum, const QSize &bounds);

} // namespace glimply
