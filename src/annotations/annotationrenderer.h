#pragma once

#include "annotationdocument.h"

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPointF>
#include <QRectF>

namespace itsova
{

// Renders document items intersecting logicalCropRect directly into nativeCrop at nativeScale,
// returning a flattened ARGB32_Premultiplied image with pixel ratio 1.0.
[[nodiscard]] QImage renderAnnotatedCrop(const QImage &nativeCrop,
                                         const QRectF &logicalCropRect,
                                         qreal nativeScale,
                                         const AnnotationDocument &document);

// Renders live annotation items (plus an optional in-progress draft item) and selection/hover
// handles into painter, whose current world transform maps logical coordinates onto the viewport.
// If nativeBackdrop is provided, destructive redactions (Pixelate/Blur) sample from it.
void paintAnnotationOverlay(QPainter &painter,
                            const QRectF &logicalViewportRect,
                            const AnnotationDocument &document,
                            const QColor &accentColor,
                            int hoveredId = 0,
                            const AnnotationItem *draftItem = nullptr,
                            const QImage *nativeBackdrop = nullptr,
                            qreal backdropScale = 1.0,
                            const QPointF &backdropLogicalOrigin = {},
                            int hiddenItemId = 0);

} // namespace itsova
