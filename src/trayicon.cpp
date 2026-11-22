#include "trayicon.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <cmath>

namespace glimply
{
namespace
{

QPixmap renderGlyph(int size, const QColor &color, bool flash)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    // Designed on a 22 px grid, the Plasma tray size.
    const qreal unit = size / 22.0;
    const qreal stroke = std::max(1.0, std::round(1.5 * unit));
    const qreal inset = 2.0 * unit + stroke / 2.0;
    const qreal far = size - inset;
    const qreal arm = 6.0 * unit;

    QPainterPath corners;
    corners.moveTo(inset, inset + arm);
    corners.lineTo(inset, inset);
    corners.lineTo(inset + arm, inset);
    corners.moveTo(far - arm, inset);
    corners.lineTo(far, inset);
    corners.lineTo(far, inset + arm);
    corners.moveTo(far, far - arm);
    corners.lineTo(far, far);
    corners.lineTo(far - arm, far);
    corners.moveTo(inset + arm, far);
    corners.lineTo(inset, far);
    corners.lineTo(inset, far - arm);

    QPen pen(color, stroke, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(corners);

    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    const qreal centre = size / 2.0;
    const qreal half = (flash ? 5.0 : 2.0) * unit;
    painter.drawRoundedRect(QRectF(centre - half, centre - half, 2 * half, 2 * half), unit, unit);
    return pixmap;
}

} // namespace

QIcon trayIcon(const QColor &color, bool flash)
{
    QIcon icon;
    for (const int size : {16, 22, 24, 32, 44, 48, 64}) {
        icon.addPixmap(renderGlyph(size, color, flash));
    }
    return icon;
}

} // namespace glimply
