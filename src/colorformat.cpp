#include "colorformat.h"

#include <QCoreApplication>

#include <algorithm>
#include <array>
#include <cmath>

namespace glimply
{
namespace
{

class ColorFormatText
{
    Q_DECLARE_TR_FUNCTIONS(ColorFormat)
};

} // namespace

ColorFormat nextColorFormat(ColorFormat current)
{
    switch (current) {
    case ColorFormat::Hex:
        return ColorFormat::Rgb;
    case ColorFormat::Rgb:
        return ColorFormat::Hsl;
    case ColorFormat::Hsl:
        return ColorFormat::Hsv;
    case ColorFormat::Hsv:
        return ColorFormat::Hex;
    }
    return ColorFormat::Hex;
}

QString colorFormatConfigKey(ColorFormat format)
{
    switch (format) {
    case ColorFormat::Hex:
        return QStringLiteral("hex");
    case ColorFormat::Rgb:
        return QStringLiteral("rgb");
    case ColorFormat::Hsl:
        return QStringLiteral("hsl");
    case ColorFormat::Hsv:
        return QStringLiteral("hsv");
    }
    return QStringLiteral("hex");
}

ColorFormat colorFormatFromKey(const QString &key)
{
    const QString lower = key.trimmed().toLower();
    if (lower == QLatin1String("rgb")) {
        return ColorFormat::Rgb;
    }
    if (lower == QLatin1String("hsl")) {
        return ColorFormat::Hsl;
    }
    if (lower == QLatin1String("hsv")) {
        return ColorFormat::Hsv;
    }
    return ColorFormat::Hex;
}

QString colorFormatDisplayName(ColorFormat format)
{
    switch (format) {
    case ColorFormat::Hex:
        return ColorFormatText::tr("HEX");
    case ColorFormat::Rgb:
        return ColorFormatText::tr("RGB");
    case ColorFormat::Hsl:
        return ColorFormatText::tr("HSL");
    case ColorFormat::Hsv:
        return ColorFormatText::tr("HSV");
    }
    return ColorFormatText::tr("HEX");
}

QString formatColor(const QColor &color, ColorFormat format)
{
    const QColor c = color.isValid() ? color.toRgb() : QColor(0, 0, 0);
    const int r = c.red();
    const int g = c.green();
    const int b = c.blue();

    switch (format) {
    case ColorFormat::Hex:
        return QStringLiteral("#%1%2%3")
            .arg(r, 2, 16, QLatin1Char('0'))
            .arg(g, 2, 16, QLatin1Char('0'))
            .arg(b, 2, 16, QLatin1Char('0'))
            .toUpper();
    case ColorFormat::Rgb:
        return QStringLiteral("rgb(%1, %2, %3)").arg(r).arg(g).arg(b);
    case ColorFormat::Hsl: {
        const double hueF = double(c.hslHueF());
        const int h = hueF < 0.0 ? 0 : int(std::lround(hueF * 360.0)) % 360;
        const int s = int(std::lround(std::max(0.0, double(c.hslSaturationF())) * 100.0));
        const int l = int(std::lround(std::max(0.0, double(c.lightnessF())) * 100.0));
        return QStringLiteral("hsl(%1, %2%, %3%)").arg(h).arg(s).arg(l);
    }
    case ColorFormat::Hsv: {
        const double hueF = double(c.hsvHueF());
        const int h = hueF < 0.0 ? 0 : int(std::lround(hueF * 360.0)) % 360;
        const int s = int(std::lround(std::max(0.0, double(c.hsvSaturationF())) * 100.0));
        const int v = int(std::lround(std::max(0.0, double(c.valueF())) * 100.0));
        return QStringLiteral("hsv(%1, %2%, %3%)").arg(h).arg(s).arg(v);
    }
    }
    return {};
}

QPoint logicalToNativePixel(const QPointF &logicalPos, const QPointF &logicalOrigin, qreal scale,
                            const QSize &nativeSize)
{
    if (nativeSize.isEmpty() || !std::isfinite(scale) || scale <= 0.0) {
        return QPoint(-1, -1);
    }
    const int px = std::clamp(int(std::floor((logicalPos.x() - logicalOrigin.x()) * scale)), 0, nativeSize.width() - 1);
    const int py = std::clamp(int(std::floor((logicalPos.y() - logicalOrigin.y()) * scale)), 0, nativeSize.height() - 1);
    return QPoint(px, py);
}

QColor sampleNativePixel(const QImage &nativeImage, const QPoint &nativePixel)
{
    if (nativeImage.isNull()) {
        return {};
    }
    const int x = std::clamp(nativePixel.x(), 0, nativeImage.width() - 1);
    const int y = std::clamp(nativePixel.y(), 0, nativeImage.height() - 1);
    const QColor c = nativeImage.pixelColor(x, y);
    return QColor(c.red(), c.green(), c.blue());
}

QImage extractNativePatch(const QImage &nativeImage, const QPoint &nativeCenter, int gridSize)
{
    if (nativeImage.isNull() || gridSize <= 0 || (gridSize % 2) == 0) {
        return {};
    }
    QImage patch(gridSize, gridSize, QImage::Format_ARGB32);
    patch.fill(Qt::transparent);
    const int half = gridSize / 2;
    const QRect bounds = nativeImage.rect();
    for (int dy = -half; dy <= half; ++dy) {
        const int sy = nativeCenter.y() + dy;
        if (sy < bounds.top() || sy > bounds.bottom()) {
            continue;
        }
        for (int dx = -half; dx <= half; ++dx) {
            const int sx = nativeCenter.x() + dx;
            if (sx < bounds.left() || sx > bounds.right()) {
                continue;
            }
            const QColor c = nativeImage.pixelColor(sx, sy);
            patch.setPixel(dx + half, dy + half, qRgba(c.red(), c.green(), c.blue(), 255));
        }
    }
    return patch;
}

QRect placeLoupeRect(const QPoint &cursorPos, const QSize &loupeSize, const QRect &containerRect, int offset,
                     int edgeMargin)
{
    if (!loupeSize.isValid() || loupeSize.isEmpty()) {
        return QRect(cursorPos + QPoint(offset, offset), loupeSize);
    }
    const std::array<QPoint, 4> origins{
        QPoint(cursorPos.x() + offset, cursorPos.y() + offset),
        QPoint(cursorPos.x() - offset - loupeSize.width(), cursorPos.y() + offset),
        QPoint(cursorPos.x() + offset, cursorPos.y() - offset - loupeSize.height()),
        QPoint(cursorPos.x() - offset - loupeSize.width(), cursorPos.y() - offset - loupeSize.height()),
    };

    const QRect safe = containerRect.adjusted(edgeMargin, edgeMargin, -edgeMargin, -edgeMargin);
    const QRect bounds = (safe.isValid() && safe.width() >= loupeSize.width() && safe.height() >= loupeSize.height())
                             ? safe
                             : containerRect;
    for (const QPoint &origin : origins) {
        const QRect candidate(origin, loupeSize);
        if (bounds.contains(candidate)) {
            return candidate;
        }
    }

    QRect fallback(origins.front(), loupeSize);
    if (bounds.isValid() && !bounds.isEmpty()) {
        const int maxX = std::max(bounds.left(), bounds.left() + bounds.width() - loupeSize.width());
        const int maxY = std::max(bounds.top(), bounds.top() + bounds.height() - loupeSize.height());
        fallback.moveLeft(std::clamp(fallback.left(), bounds.left(), maxX));
        fallback.moveTop(std::clamp(fallback.top(), bounds.top(), maxY));
    }
    return fallback;
}

} // namespace glimply
