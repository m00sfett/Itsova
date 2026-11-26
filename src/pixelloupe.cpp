#include "pixelloupe.h"

#include <QCoreApplication>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QIcon>

#include <algorithm>

namespace glimply::PixelLoupe
{
namespace
{

class LoupeText
{
    Q_DECLARE_TR_FUNCTIONS(PixelLoupe)
};

void paintCardShell(QPainter &painter, const QRect &box, const QPalette &palette, qreal radius = 8.0)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (int i = 4; i > 0; --i) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 10));
        painter.drawRoundedRect(QRectF(box).adjusted(-i, -i + 2, i, i + 2), radius + i, radius + i);
    }
    painter.setBrush(palette.color(QPalette::Window));
    QColor border = palette.color(QPalette::WindowText);
    border.setAlpha(60);
    painter.setPen(QPen(border, 1));
    painter.drawRoundedRect(QRectF(box).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    painter.restore();
}

} // namespace

void paintLoupeGrid(QPainter &painter, const QRect &gridRect, const QImage &nativeImage, const QPoint &nativePixel,
                    int gridCount, int cellPx, const QColor &accent, const QRect &nativeSelection)
{
    const QImage patch = extractNativePatch(nativeImage, nativePixel, gridCount);
    const int half = gridCount / 2;
    const QPoint origin = nativePixel - QPoint(half, half); // native pixel of the top-left cell

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);

    // Dark backing for any cells that fall outside the screen edge.
    painter.fillRect(gridRect, QColor(24, 24, 24));
    if (!patch.isNull()) {
        painter.drawImage(gridRect, patch);
    }

    // Pixels outside the selection are dimmed like in the overlay (#101).
    if (!nativeSelection.isNull()) {
        const QRect insideCells = nativeSelection.translated(-origin);
        for (int row = 0; row < gridCount; ++row) {
            for (int column = 0; column < gridCount; ++column) {
                if (!insideCells.contains(column, row)) {
                    painter.fillRect(QRect(gridRect.left() + column * cellPx, gridRect.top() + row * cellPx, cellPx,
                                           cellPx),
                                     QColor(0, 0, 0, 120));
                }
            }
        }
    }

    // Subtle 1-px grid between native pixel cells.
    painter.setPen(QPen(QColor(128, 128, 128, 60), 1));
    for (int i = 1; i < gridCount; ++i) {
        const int x = gridRect.left() + i * cellPx;
        const int y = gridRect.top() + i * cellPx;
        painter.drawLine(x, gridRect.top(), x, gridRect.bottom());
        painter.drawLine(gridRect.left(), y, gridRect.right(), y);
    }

    // The selection edge exactly on the pixel boundaries: two pixels wide, centred on the line
    // between an outside and an inside cell; a corner reads as an L.
    if (!nativeSelection.isNull()) {
        const QRect cells = nativeSelection.translated(-origin);
        const int x0 = gridRect.left() + cells.left() * cellPx;
        const int x1 = gridRect.left() + (cells.right() + 1) * cellPx;
        const int y0 = gridRect.top() + cells.top() * cellPx;
        const int y1 = gridRect.top() + (cells.bottom() + 1) * cellPx;
        painter.setClipRect(gridRect);
        const QColor edge = accent;
        const auto vertical = [&](int x) { painter.fillRect(QRect(x - 1, y0 - 1, 2, y1 - y0 + 2), edge); };
        const auto horizontal = [&](int y) { painter.fillRect(QRect(x0 - 1, y - 1, x1 - x0 + 2, 2), edge); };
        vertical(x0);
        vertical(x1);
        horizontal(y0);
        horizontal(y1);
        painter.setClipping(false);
    }

    painter.setPen(QPen(QColor(0, 0, 0, 140), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(gridRect.adjusted(0, 0, -1, -1));

    // Dual-contrast center cell outline leaving the center pixel interior unobstructed.
    const QRect centerCell(gridRect.left() + half * cellPx, gridRect.top() + half * cellPx, cellPx, cellPx);
    painter.setPen(QPen(QColor(0, 0, 0, 210), 1));
    painter.drawRect(centerCell.adjusted(-1, -1, 0, 0));
    painter.setPen(QPen(QColor(255, 255, 255, 235), 1));
    painter.drawRect(centerCell.adjusted(0, 0, -1, -1));
    painter.setPen(QPen(accent, 1));
    painter.drawPoint(centerCell.topLeft() + QPoint(-1, -1));
    painter.drawPoint(centerCell.topRight() + QPoint(1, -1));
    painter.drawPoint(centerCell.bottomLeft() + QPoint(-1, 1));
    painter.drawPoint(centerCell.bottomRight() + QPoint(1, 1));
    painter.restore();
}

void paintSelectionLoupe(QPainter &painter, const QPoint &cursorLocal, const QRect &containerLocal,
                         const QPalette &palette, const QImage &nativeImage, const QPoint &nativePixel,
                         const QPoint &displayCoord, const QSize &selectionSize, const QRect &nativeSelection,
                         bool snapshot, bool keyNudge)
{
    constexpr int gridCount = 11;
    constexpr int cellPx = 11;
    constexpr int gridPx = gridCount * cellPx; // 121x121
    constexpr int pad = 8;
    constexpr int spacing = 6;

    const QString label = LoupeText::tr("%1, %2 · %3 × %4")
                              .arg(displayCoord.x())
                              .arg(displayCoord.y())
                              .arg(selectionSize.width())
                              .arg(selectionSize.height());
    QFont font = painter.font();
    font.setBold(true);
    const QFontMetrics metrics(font);
    const QSize textSize = metrics.size(Qt::TextSingleLine, label);
    // Live overlay: the pixels are the Print snapshot, the screen may have moved on (#101).
    const QString hint = snapshot ? LoupeText::tr("As of Print") : QString();
    QFont hintFont = painter.font();
    hintFont.setPointSizeF(std::max(7.0, hintFont.pointSizeF() * 0.85));
    const QFontMetrics hintMetrics(hintFont);
    const QSize hintSize = hint.isEmpty() ? QSize() : hintMetrics.size(Qt::TextSingleLine, hint);

    const int cardW = std::max({gridPx, textSize.width(), hintSize.width()}) + 2 * pad;
    const int cardH = pad + gridPx + spacing + textSize.height() + (hint.isEmpty() ? 0 : hintSize.height() + 2) + pad;
    const QRect card = placeLoupeRect(cursorLocal, QSize(cardW, cardH), containerLocal);

    paintCardShell(painter, card, palette);

    const QRect gridRect(card.left() + (card.width() - gridPx) / 2, card.top() + pad, gridPx, gridPx);
    paintLoupeGrid(painter, gridRect, nativeImage, nativePixel, gridCount, cellPx, palette.color(QPalette::Highlight),
                   nativeSelection);

    painter.save();
    painter.setFont(font);
    painter.setPen(palette.color(QPalette::WindowText));
    const QRect textRect(card.left() + pad, gridRect.bottom() + 1 + spacing, card.width() - 2 * pad, textSize.height());
    painter.drawText(textRect, Qt::AlignCenter, label);
    if (keyNudge) {
        // Arrow keys hold the corner; the mouse takes over again after a larger move (#102).
        const QIcon key = QIcon::fromTheme(QStringLiteral("input-keyboard"));
        const int s = 16;
        key.paint(&painter, QRect(gridRect.right() - s - 3, gridRect.top() + 3, s, s));
    }
    if (!hint.isEmpty()) {
        painter.setFont(hintFont);
        QColor muted = palette.color(QPalette::WindowText);
        muted.setAlphaF(0.7);
        painter.setPen(muted);
        painter.drawText(QRect(card.left() + pad, textRect.bottom() + 3, card.width() - 2 * pad, hintSize.height()),
                         Qt::AlignCenter, hint);
    }
    painter.restore();
}

void paintDimensionPill(QPainter &painter, const QPoint &cursorLocal, const QRect &containerLocal,
                        const QPalette &palette, const QPoint &displayCoord, const QSize &selectionSize)
{
    const QString label = LoupeText::tr("%1, %2 · %3 × %4")
                              .arg(displayCoord.x())
                              .arg(displayCoord.y())
                              .arg(selectionSize.width())
                              .arg(selectionSize.height());
    QFont font = painter.font();
    font.setBold(true);
    const QFontMetrics metrics(font);
    const QSize textSize = metrics.size(Qt::TextSingleLine, label);
    const QSize pillSize = textSize + QSize(18, 10);
    const QRect box = placeLoupeRect(cursorLocal, pillSize, containerLocal, 16, 8);

    paintCardShell(painter, box, palette, 6.0);

    painter.save();
    painter.setFont(font);
    painter.setPen(palette.color(QPalette::WindowText));
    painter.drawText(box, Qt::AlignCenter, label);
    painter.restore();
}

void paintColorPickerLoupe(QPainter &painter, const QPoint &cursorLocal, const QRect &containerLocal,
                           const QPalette &palette, const QImage &nativeImage, const QPoint &nativePixel,
                           ColorFormat format, bool showAltHint)
{
    constexpr int gridCount = 15;
    constexpr int cellPx = 10;
    constexpr int gridPx = gridCount * cellPx; // 150x150
    constexpr int pad = 8;
    constexpr int swatchSize = 16;
    constexpr int swatchGap = 6;

    const QColor sampled = sampleNativePixel(nativeImage, nativePixel);
    const QString valueText = formatColor(sampled, format);
    const QString hintText = showAltHint ? LoupeText::tr("Click: Copy · Alt+Click: Use · Tab: Format")
                                         : LoupeText::tr("Click: Copy · Tab: Format · Esc: Cancel");

    QFont valueFont = painter.font();
    valueFont.setBold(true);
    const QFontMetrics valueMetrics(valueFont);
    const QSize valueSize = valueMetrics.size(Qt::TextSingleLine, valueText);

    QFont hintFont = painter.font();
    if (hintFont.pointSizeF() > 0.0) {
        hintFont.setPointSizeF(std::max(7.0, hintFont.pointSizeF() * 0.85));
    }
    const QFontMetrics hintMetrics(hintFont);
    const QSize hintSize = hintMetrics.size(Qt::TextSingleLine, hintText);

    const int colorRowW = swatchSize + swatchGap + valueSize.width();
    const int colorRowH = std::max(swatchSize, valueSize.height());
    const int contentW = std::max({gridPx, colorRowW, hintSize.width()});
    const int cardW = contentW + 2 * pad;
    const int cardH = pad + gridPx + 8 + colorRowH + 4 + hintSize.height() + pad;
    const QRect card = placeLoupeRect(cursorLocal, QSize(cardW, cardH), containerLocal);

    paintCardShell(painter, card, palette);

    const QRect gridRect(card.left() + (card.width() - gridPx) / 2, card.top() + pad, gridPx, gridPx);
    paintLoupeGrid(painter, gridRect, nativeImage, nativePixel, gridCount, cellPx, palette.color(QPalette::Highlight));

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const int rowY = gridRect.bottom() + 1 + 8;
    const int rowStartX = card.left() + std::max(pad, (card.width() - colorRowW) / 2);
    const bool rtl = painter.layoutDirection() == Qt::RightToLeft
                     || (painter.layoutDirection() == Qt::LayoutDirectionAuto
                         && QGuiApplication::layoutDirection() == Qt::RightToLeft);
    const int swatchX = rtl ? rowStartX + valueSize.width() + swatchGap : rowStartX;
    const int valueX = rtl ? rowStartX : swatchX + swatchSize + swatchGap;
    const QRect swatchRect(swatchX, rowY + (colorRowH - swatchSize) / 2, swatchSize, swatchSize);
    painter.setBrush(sampled);
    QColor swatchBorder = palette.color(QPalette::WindowText);
    swatchBorder.setAlpha(150);
    painter.setPen(QPen(swatchBorder, 1));
    painter.drawRoundedRect(QRectF(swatchRect).adjusted(0.5, 0.5, -0.5, -0.5), 3.0, 3.0);

    painter.setFont(valueFont);
    painter.setPen(palette.color(QPalette::WindowText));
    const QRect valueRect(valueX, rowY, valueSize.width() + 2, colorRowH);
    painter.drawText(valueRect, Qt::AlignVCenter | (rtl ? Qt::AlignRight : Qt::AlignLeft), valueText);

    painter.setFont(hintFont);
    QColor muted = palette.color(QPalette::WindowText);
    muted.setAlphaF(0.76);
    painter.setPen(muted);
    const QRect hintRect(card.left() + pad, rowY + colorRowH + 4, card.width() - 2 * pad, hintSize.height());
    painter.drawText(hintRect, Qt::AlignCenter, hintText);
    painter.restore();
}

} // namespace glimply::PixelLoupe
