#pragma once

#include <QList>
#include <QMargins>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <Qt>

#include <optional>

namespace glimply
{

// Logical pixel threshold for magnetic snapping onto connected monitor edges.
inline constexpr int monitorEdgeSnapPx = 8;

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

// Native-pixel nudging while a corner or edge is dragged (#102). The selection keeps its logical
// rectangle and moves each edge by a whole number of native pixels on top (QMargins: left, top,
// right, bottom; positive moves an edge right or down).
// The corner a new selection's pointer drags, from the anchor's point of view.
[[nodiscard]] Qt::Edges draggedEdges(const QPoint &anchor, const QPoint &pointer);
// base with delta (native pixels) applied to the dragged edges only.
[[nodiscard]] QMargins nudgedMargins(const QMargins &base, Qt::Edges edges, const QPoint &delta);
// A native area with its edges moved by nudge.
[[nodiscard]] QRect nudgedNativeArea(const QRect &nativeArea, const QMargins &nudge);

// Wayland cannot move the pointer, so after an arrow key the mouse is ignored until it has moved
// more than nudgeDeadZonePx logical pixels from where it was at the key press; then it takes over
// from where the corner is, without a jump (#102).
inline constexpr int nudgeDeadZonePx = 10;
class NudgeDeadZone
{
public:
    void keyPressed(const QPoint &pointer);
    // The position to drag to for this pointer position, or nothing while the dead zone holds.
    [[nodiscard]] std::optional<QPoint> filter(const QPoint &pointer);
    [[nodiscard]] bool active() const { return active_; }
    void reset();

private:
    bool active_ = false;
    QPoint keyPointer_;
    QPoint offset_;
};

// Virtual-desktop bounds overloads handling arbitrary (including non-zero or negative) origins.
[[nodiscard]] QRect clampToBounds(QRect rect, const QRect &bounds);
[[nodiscard]] QRect drawnSelection(const QPoint &anchor, const QPoint &position, SelectionConstraints constraints,
                                   const QRect &bounds);
[[nodiscard]] QRect resizedSelection(const QRect &start, Qt::Edges edges, const QPoint &delta,
                                     SelectionConstraints constraints, const QRect &bounds);
[[nodiscard]] QRect steppedSelection(const QRect &rect, const QPoint &step, SelectionConstraints constraints,
                                     int minimum, const QRect &bounds);

// Magnetic edge snapping across connected monitor geometries (using half-open right/bottom edges
// matching QRect::left() + QRect::width()).
[[nodiscard]] QPoint snapPointToMonitorEdges(const QPoint &pos, const QList<QRect> &screenRects,
                                             int threshold = monitorEdgeSnapPx);
[[nodiscard]] QPoint snapResizeDeltaToMonitorEdges(const QRect &start, Qt::Edges edges, const QPoint &delta,
                                                   const QList<QRect> &screenRects,
                                                   int threshold = monitorEdgeSnapPx);
[[nodiscard]] QRect snapMovedRectToMonitorEdges(const QRect &rect, const QList<QRect> &screenRects,
                                                const QRect &desktopBounds, int threshold = monitorEdgeSnapPx);

// Placement helpers for overlay chrome (capture rects and monitors are never mirrored in RTL).
// The session panel on one monitor (#69, Q5b/Q17), in overlay coordinates. A position the user
// chose wins and is only kept on the monitor. Otherwise the panel sits top centre and moves to the
// bottom centre when it would cover the selection (the side that covers less, if both do).
[[nodiscard]] QPoint placeOverlayPanel(const QSize &panelSize, const QSize &overlaySize, const QRect &selection,
                                       const std::optional<QPoint> &userPosition, int margin);
[[nodiscard]] QRect overlayCloseButtonRect(const QRect &globalRect, const QRect &bounds, int buttonSize,
                                           Qt::LayoutDirection direction);
[[nodiscard]] QPointF overlaySelectionBadgeCenter(const QRectF &frame, Qt::LayoutDirection direction);

} // namespace glimply
