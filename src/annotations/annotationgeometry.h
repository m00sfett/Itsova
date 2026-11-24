#pragma once

#include "annotationitem.h"

#include <QList>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>

namespace glimply
{

// Modifiers active while drawing or resizing an annotation (PhotoCraft / Photoshop aligned):
// Shift = constrain (1:1 aspect ratio or 15-degree angle snap); Alt = grow from center.
struct DrawModifiers {
    bool constrain = false;
    bool fromCenter = false;
};

[[nodiscard]] QRectF constrainedBox(const QPointF &anchor, const QPointF &current, DrawModifiers mods,
                                    const QRectF &bounds = {});

[[nodiscard]] QRectF resizedBox(const QRectF &startRect, AnnotationHandle handle, const QPointF &delta,
                                DrawModifiers mods, qreal minSide = 4.0);

[[nodiscard]] AnnotationItem applyHandleDrag(const AnnotationItem &startItem, AnnotationHandle handle,
                                             const QPointF &delta, DrawModifiers mods);

[[nodiscard]] QPointF snapAngle15(const QPointF &origin, const QPointF &target);

[[nodiscard]] QList<QPointF> simplifyPolyline(const QList<QPointF> &points, qreal epsilon = 1.0);

[[nodiscard]] QPainterPath smoothPolylinePath(const QList<QPointF> &points);

} // namespace glimply
