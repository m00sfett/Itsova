#pragma once

#include "annotationitem.h"

#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>

namespace itsova
{

// Maps a logical redaction item rectangle into device-pixel coordinates inside an image whose
// top-left sits at logicalOrigin. Rounds outward (floor left/top, ceil right/bottom) so fractional
// HiDPI scales never leave an unredacted 1-px edge fringe.
[[nodiscard]] QRect redactionPhysicalRect(const QRectF &logicalItemRect,
                                          const QPointF &logicalOrigin,
                                          qreal nativeScale,
                                          const QSize &imageSize);

// Produces an opaque ARGB32_Premultiplied replacement patch of size physRect.size() for item.
// All output pixels carry alpha == 255; Pixelate and Blur enforce hard secrecy floors (12 px
// logical block / 14 px logical radius) and sample strictly inside physRect.
[[nodiscard]] QImage renderRedactedPatch(const QImage &sourceImage,
                                         const QRect &physRect,
                                         const QPoint &docGridPhysOrigin,
                                         const AnnotationItem &item,
                                         qreal nativeScale);

} // namespace itsova
