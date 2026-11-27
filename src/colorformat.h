#pragma once

#include <QColor>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QString>

namespace itsova
{

enum class ColorFormat { Hex, Rgb, Hsl, Hsv };

[[nodiscard]] ColorFormat nextColorFormat(ColorFormat current);
[[nodiscard]] QString colorFormatConfigKey(ColorFormat format);
[[nodiscard]] ColorFormat colorFormatFromKey(const QString &key);
[[nodiscard]] QString colorFormatDisplayName(ColorFormat format);
[[nodiscard]] QString formatColor(const QColor &color, ColorFormat format);

[[nodiscard]] QPoint logicalToNativePixel(const QPointF &logicalPos, const QPointF &logicalOrigin, qreal scale,
                                            const QSize &nativeSize);
[[nodiscard]] QColor sampleNativePixel(const QImage &nativeImage, const QPoint &nativePixel);
[[nodiscard]] QImage extractNativePatch(const QImage &nativeImage, const QPoint &nativeCenter, int gridSize);
[[nodiscard]] QRect placeLoupeRect(const QPoint &cursorPos, const QSize &loupeSize, const QRect &containerRect,
                                     int offset = 20, int edgeMargin = 8);

} // namespace itsova
