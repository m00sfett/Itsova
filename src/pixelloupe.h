#pragma once

#include "colorformat.h"

#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QPoint>
#include <QRect>
#include <QSize>

namespace glimply::PixelLoupe
{

void paintSelectionLoupe(QPainter &painter,
                         const QPoint &cursorLocal,
                         const QRect &containerLocal,
                         const QPalette &palette,
                         const QImage &nativeImage,
                         const QPoint &nativePixel,
                         const QPoint &displayCoord,
                         const QSize &selectionSize);

void paintDimensionPill(QPainter &painter,
                        const QPoint &cursorLocal,
                        const QRect &containerLocal,
                        const QPalette &palette,
                        const QPoint &displayCoord,
                        const QSize &selectionSize);

void paintColorPickerLoupe(QPainter &painter,
                           const QPoint &cursorLocal,
                           const QRect &containerLocal,
                           const QPalette &palette,
                           const QImage &nativeImage,
                           const QPoint &nativePixel,
                           ColorFormat format,
                           bool showAltHint);

} // namespace glimply::PixelLoupe
