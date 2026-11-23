#include "trayicon.h"

#include <QPainter>
#include <QPixmap>
#include <QPolygonF>

namespace glimply
{
namespace
{

// The colours of the roteson.de tools (see data/io.github.m00sfett.glimply-tray.svg).
const QColor ink(0x08, 0x06, 0x04);
const QColor red(0xB1, 0x12, 0x18);
const QColor yellow(0xFF, 0xD2, 0x1F);

QPixmap renderGlyph(int size, bool flash)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    // Drawn on the 22 px grid of the SVG, the Plasma tray size.
    painter.scale(size / 22.0, size / 22.0);

    painter.setPen(QPen(ink, 1.0));
    painter.setBrush(flash ? yellow : red);
    painter.drawRoundedRect(QRectF(0.5, 0.5, 21, 21), 4.5, 4.5);

    painter.translate(11, 11);
    painter.scale(0.86, 0.86);
    painter.translate(-11, -11);
    painter.setPen(Qt::NoPen);
    painter.setBrush(flash ? red : yellow);
    const QList<QPolygonF> corners{
        QPolygonF({{3, 3}, {9, 3}, {9, 5}, {5, 5}, {5, 9}, {3, 9}}),
        QPolygonF({{13, 3}, {19, 3}, {19, 9}, {17, 9}, {17, 5}, {13, 5}}),
        QPolygonF({{19, 13}, {19, 19}, {13, 19}, {13, 17}, {17, 17}, {17, 13}}),
        QPolygonF({{3, 13}, {5, 13}, {5, 17}, {9, 17}, {9, 19}, {3, 19}}),
    };
    for (const QPolygonF &corner : corners) {
        painter.drawPolygon(corner);
    }
    if (flash) {
        painter.drawRoundedRect(QRectF(6, 6, 10, 10), 1.5, 1.5);
    } else {
        painter.drawRoundedRect(QRectF(9, 9, 4, 4), 1, 1);
    }
    return pixmap;
}

} // namespace

QIcon trayIcon(bool flash)
{
    QIcon icon;
    for (const int size : {16, 22, 24, 32, 44, 48, 64}) {
        icon.addPixmap(renderGlyph(size, flash));
    }
    return icon;
}

} // namespace glimply
