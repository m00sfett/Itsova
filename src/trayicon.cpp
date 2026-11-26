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

QPixmap renderGlyph(int size, TrayIconState state)
{
    // The capture flash and a running recording (#99) use the inverted tile: yellow with red marks.
    const bool recording = state == TrayIconState::Recording || state == TrayIconState::RecordingPaused;
    const bool flash = state == TrayIconState::Flash || recording;
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
    if (state == TrayIconState::Recording) {
        painter.drawEllipse(QPointF(11, 11), 5, 5); // a filled red dot, as large as the flash square
    } else if (state == TrayIconState::RecordingPaused) {
        painter.drawRoundedRect(QRectF(6.5, 6, 3.4, 10), 0.9, 0.9);
        painter.drawRoundedRect(QRectF(12.1, 6, 3.4, 10), 0.9, 0.9);
    } else if (flash) {
        painter.drawRoundedRect(QRectF(6, 6, 10, 10), 1.5, 1.5);
    } else {
        painter.drawRoundedRect(QRectF(9, 9, 4, 4), 1, 1);
    }

    if (state == TrayIconState::IntervalActive) {
        painter.resetTransform();
        painter.scale(size / 22.0, size / 22.0);
        painter.setPen(QPen(ink, 1.0));
        painter.setBrush(yellow);
        painter.drawEllipse(QPointF(17.5, 17.5), 3.5, 3.5);
        painter.setPen(Qt::NoPen);
        painter.setBrush(red);
        painter.drawEllipse(QPointF(17.5, 17.5), 1.4, 1.4);
    }
    return pixmap;
}

} // namespace

QIcon trayIcon(TrayIconState state)
{
    QIcon icon;
    // Tray sizes and the same at 125 % (20, 28, 30, 40, 60), so fractional scaling stays crisp.
    for (const int size : {16, 20, 22, 24, 28, 30, 32, 40, 44, 48, 60, 64}) {
        icon.addPixmap(renderGlyph(size, state));
    }
    return icon;
}

QIcon trayIcon(bool flash)
{
    return trayIcon(flash ? TrayIconState::Flash : TrayIconState::Normal);
}

} // namespace glimply
