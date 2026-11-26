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

// The magnified native pixels around nativePixel (#101): pixels outside nativeSelection (frame
// coordinates; a null rect means none) are dimmed and its edge runs on the pixel boundaries.
void paintLoupeGrid(QPainter &painter, const QRect &gridRect, const QImage &nativeImage, const QPoint &nativePixel,
                    int gridCount, int cellPx, const QColor &accent, const QRect &nativeSelection = QRect());

// snapshot: the pixels come from the Print snapshot while the overlay is live; the card says so.
void paintSelectionLoupe(QPainter &painter,
                         const QPoint &cursorLocal,
                         const QRect &containerLocal,
                         const QPalette &palette,
                         const QImage &nativeImage,
                         const QPoint &nativePixel,
                         const QPoint &displayCoord,
                         const QSize &selectionSize,
                         const QRect &nativeSelection = QRect(),
                         bool snapshot = false,
                         bool keyNudge = false);

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
