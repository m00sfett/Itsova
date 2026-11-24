#include "annotationtextlayout.h"

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPainterPath>
#include <QTextLayout>
#include <QTextOption>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace glimply
{
namespace
{

struct BuiltTextLines {
    std::vector<std::unique_ptr<QTextLayout>> layouts;
    QSizeF naturalSize;
};

BuiltTextLines buildLayouts(const AnnotationItem &item, const QFont &font, qreal boxWidth)
{
    BuiltTextLines result;
    const QStringList paragraphs = item.text.split(QLatin1Char('\n'));
    const QFontMetricsF metrics(font);
    const qreal lineSpacing = std::max(metrics.lineSpacing(), metrics.height());

    QTextOption opt;
    opt.setAlignment(item.textAlignment);
    opt.setWrapMode(item.paragraphMode ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);

    qreal y = 0.0;
    qreal maxW = 0.0;
    for (const QString &para : paragraphs) {
        auto layout = std::make_unique<QTextLayout>(para, font);
        layout->setTextOption(opt);
        layout->beginLayout();
        if (para.isEmpty()) {
            QTextLine line = layout->createLine();
            if (line.isValid()) {
                line.setLineWidth(boxWidth);
                line.setPosition(QPointF(0.0, y));
            }
            y += lineSpacing;
        } else {
            for (;;) {
                QTextLine line = layout->createLine();
                if (!line.isValid()) {
                    break;
                }
                line.setLineWidth(boxWidth);
                line.setPosition(QPointF(0.0, y));
                maxW = std::max(maxW, line.naturalTextWidth());
                y += std::max(line.height(), lineSpacing);
            }
        }
        layout->endLayout();
        result.layouts.push_back(std::move(layout));
    }
    result.naturalSize = QSizeF(std::max(16.0, maxW), std::max(lineSpacing, y));
    return result;
}

void paintLayoutsAt(QPainter &painter, const std::vector<std::unique_ptr<QTextLayout>> &layouts,
                    const QPointF &origin)
{
    for (const auto &layout : layouts) {
        layout->draw(&painter, origin);
    }
}

void boxBlurPremultipliedAlpha(QImage &img, int radius)
{
    if (radius <= 0 || img.isNull()) {
        return;
    }
    const int w = img.width();
    const int h = img.height();
    QImage tmp(w, h, QImage::Format_ARGB32_Premultiplied);
    for (int pass = 0; pass < 2; ++pass) {
        // Horizontal pass -> tmp.
        for (int y = 0; y < h; ++y) {
            const QRgb *src = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            QRgb *dst = reinterpret_cast<QRgb *>(tmp.scanLine(y));
            for (int x = 0; x < w; ++x) {
                int sumA = 0;
                int count = 0;
                const int x0 = std::max(0, x - radius);
                const int x1 = std::min(w - 1, x + radius);
                for (int k = x0; k <= x1; ++k) {
                    sumA += qAlpha(src[k]);
                    ++count;
                }
                const int a = sumA / count;
                dst[x] = qRgba(0, 0, 0, a);
            }
        }
        // Vertical pass -> img.
        for (int y = 0; y < h; ++y) {
            QRgb *dst = reinterpret_cast<QRgb *>(img.scanLine(y));
            const int y0 = std::max(0, y - radius);
            const int y1 = std::min(h - 1, y + radius);
            const int count = y1 - y0 + 1;
            for (int x = 0; x < w; ++x) {
                int sumA = 0;
                for (int k = y0; k <= y1; ++k) {
                    const QRgb *src = reinterpret_cast<const QRgb *>(tmp.constScanLine(k));
                    sumA += qAlpha(src[x]);
                }
                const int a = sumA / count;
                dst[x] = qRgba(0, 0, 0, a);
            }
        }
    }
}

void drawEffectBuffer(QPainter &painter, const QRectF &logicalBounds, const QFont &font,
                      const BuiltTextLines &built, const QColor &effectColor, bool shadowBlur,
                      const QPointF &logicalOffset)
{
    const QTransform world = painter.worldTransform();
    const qreal dpr = painter.device() ? painter.device()->devicePixelRatioF() : 1.0;
    const qreal worldScale = std::max(0.1, std::hypot(world.m11(), world.m12()));
    const QRect worldBox = world.mapRect(logicalBounds.adjusted(-6.0, -6.0, 8.0, 8.0)).toAlignedRect();
    if (worldBox.isEmpty()) {
        return;
    }

    const QSize pixSize(std::max(1, int(std::ceil(worldBox.width() * dpr))),
                        std::max(1, int(std::ceil(worldBox.height() * dpr))));
    QImage buf(pixSize, QImage::Format_ARGB32_Premultiplied);
    buf.setDevicePixelRatio(dpr);
    buf.fill(Qt::transparent);

    {
        QPainter bp(&buf);
        bp.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
        bp.setWorldTransform(world * QTransform::fromTranslate(-worldBox.left(), -worldBox.top()));
        bp.setFont(font);
        QColor opaqueEffect = effectColor;
        opaqueEffect.setAlpha(255);
        bp.setPen(shadowBlur ? opaqueEffect : effectColor);

        if (shadowBlur) {
            const QPointF base = logicalBounds.topLeft() + logicalOffset;
            for (const QPointF tap : {QPointF(0.0, 0.0), QPointF(0.5, 0.0), QPointF(0.0, 0.5), QPointF(0.5, 0.5)}) {
                paintLayoutsAt(bp, built.layouts, base + tap);
            }
        } else {
            // Circular 12-sample stroke ring preserves HarfBuzz RTL/ZWNJ/emoji shaping cleanly.
            constexpr int ringSamples = 12;
            constexpr qreal outlineRadius = 2.0;
            constexpr qreal twoPi = 6.28318530717958647692;
            for (int i = 0; i < ringSamples; ++i) {
                const qreal ang = (twoPi * i) / ringSamples;
                const QPointF off(outlineRadius * std::cos(ang), outlineRadius * std::sin(ang));
                paintLayoutsAt(bp, built.layouts, logicalBounds.topLeft() + off);
            }
        }
    }

    if (shadowBlur) {
        const int blurRadius = std::clamp(int(std::lround(1.2 * worldScale * dpr)), 1, 5);
        boxBlurPremultipliedAlpha(buf, blurRadius);
        // Re-tint the blurred alpha mask to effectColor.
        const int r = effectColor.red();
        const int g = effectColor.green();
        const int b = effectColor.blue();
        const qreal alphaScale = effectColor.alphaF() * 1.6;
        for (int y = 0; y < buf.height(); ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(buf.scanLine(y));
            for (int x = 0; x < buf.width(); ++x) {
                const int a = std::clamp(int(std::lround(qAlpha(line[x]) * alphaScale)), 0, 230);
                line[x] = qRgba((r * a + 127) / 255, (g * a + 127) / 255, (b * a + 127) / 255, a);
            }
        }
    }

    painter.save();
    painter.setWorldTransform(QTransform());
    painter.drawImage(worldBox.topLeft(), buf);
    painter.restore();
}

} // namespace

QFont resolvedAnnotationFont(const AnnotationItem &item)
{
    QFont f = item.font;
    if (f.family().isEmpty() && QGuiApplication::instance()) {
        const QFont sys = QGuiApplication::font();
        f.setFamily(sys.family());
    }
    if (f.pointSizeF() <= 0.0) {
        f.setPointSizeF(16.0);
    }
    return f;
}

QSizeF measureAnnotationText(const AnnotationItem &item)
{
    const QFont font = resolvedAnnotationFont(item);
    const qreal wrapW = item.paragraphMode ? std::max(60.0, item.rect.normalized().width()) : 1.0e6;
    const BuiltTextLines built = buildLayouts(item, font, wrapW);
    if (item.paragraphMode) {
        return QSizeF(wrapW, built.naturalSize.height());
    }
    return built.naturalSize;
}

void drawAnnotationText(QPainter &painter, const AnnotationItem &item)
{
    if (item.text.isEmpty()) {
        return;
    }
    const QFont font = resolvedAnnotationFont(item);
    const QRectF norm = item.rect.normalized();
    const qreal measureWidth = item.paragraphMode ? std::max(60.0, norm.width()) : 1.0e6;
    BuiltTextLines built = buildLayouts(item, font, measureWidth);

    const QSizeF boxSize = item.paragraphMode
        ? QSizeF(measureWidth, built.naturalSize.height())
        : built.naturalSize;

    // Re-run layout at exact box width for Point Text with center/right alignment.
    if (!item.paragraphMode && !item.textAlignment.testFlag(Qt::AlignLeft)) {
        built = buildLayouts(item, font, boxSize.width());
    }

    const QRectF logicalBox(norm.topLeft(), boxSize);

    painter.save();
    painter.setFont(font);

    switch (item.legibility) {
    case TextLegibility::None:
        break;
    case TextLegibility::Pill: {
        const QColor contrast = contrastingTextColor(item.color);
        const QColor backing = contrast.lightness() < 128 ? QColor(18, 18, 20, 225) : QColor(250, 250, 252, 235);
        painter.setPen(Qt::NoPen);
        painter.setBrush(backing);
        painter.drawRoundedRect(logicalBox.adjusted(-8.0, -4.0, 8.0, 4.0), 6.0, 6.0);
        break;
    }
    case TextLegibility::Shadow: {
        const QColor shadowColor = contrastingTextColor(item.color);
        QColor tinted = shadowColor;
        tinted.setAlpha(185);
        drawEffectBuffer(painter, logicalBox, font, built, tinted, true, QPointF(1.5, 2.0));
        break;
    }
    case TextLegibility::Outline: {
        const QColor outlineColor = contrastingTextColor(item.color);
        drawEffectBuffer(painter, logicalBox, font, built, outlineColor, false, QPointF());
        break;
    }
    }

    painter.setPen(item.color);
    paintLayoutsAt(painter, built.layouts, logicalBox.topLeft());
    painter.restore();
}

} // namespace glimply
