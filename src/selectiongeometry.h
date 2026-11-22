#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>

namespace glimply
{

// Modifiers that shape a selection while it is drawn or resized.
struct SelectionConstraints {
    // The rectangle grows symmetrically around its centre (Ctrl).
    bool fromCentre = false;
    // Width and height stay equal (Alt).
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

} // namespace glimply
