#include "redactionprocessor.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <vector>

namespace glimply
{
namespace
{

int floorDiv(int a, int b)
{
    int q = a / b;
    const int r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) {
        --q;
    }
    return q;
}

quint32 mix32(quint32 x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

QImage pixelatePatch(const QImage &srcArgb32, const QPoint &docOrigin, int cellPx, quint32 seed)
{
    const int w = srcArgb32.width();
    const int h = srcArgb32.height();
    QImage out(w, h, QImage::Format_ARGB32_Premultiplied);

    const int gxMin = floorDiv(docOrigin.x(), cellPx);
    const int gxMax = floorDiv(docOrigin.x() + w - 1, cellPx);
    const int gyMin = floorDiv(docOrigin.y(), cellPx);
    const int gyMax = floorDiv(docOrigin.y() + h - 1, cellPx);

    for (int gy = gyMin; gy <= gyMax; ++gy) {
        const int ly0 = std::max(0, gy * cellPx - docOrigin.y());
        const int ly1 = std::min(h, (gy + 1) * cellPx - docOrigin.y());
        if (ly1 <= ly0) {
            continue;
        }
        for (int gx = gxMin; gx <= gxMax; ++gx) {
            const int lx0 = std::max(0, gx * cellPx - docOrigin.x());
            const int lx1 = std::min(w, (gx + 1) * cellPx - docOrigin.x());
            if (lx1 <= lx0) {
                continue;
            }

            qint64 sumR = 0;
            qint64 sumG = 0;
            qint64 sumB = 0;
            qint64 count = 0;
            for (int y = ly0; y < ly1; ++y) {
                const QRgb *line = reinterpret_cast<const QRgb *>(srcArgb32.constScanLine(y));
                for (int x = lx0; x < lx1; ++x) {
                    const QRgb px = line[x];
                    sumR += qRed(px);
                    sumG += qGreen(px);
                    sumB += qBlue(px);
                    ++count;
                }
            }

            const int avgR = int(std::lround(double(sumR) / double(count)));
            const int avgG = int(std::lround(double(sumG) / double(count)));
            const int avgB = int(std::lround(double(sumB) / double(count)));

            // 16-level RGB quantization (step 17) + bounded content-independent PRNG jitter (±6).
            const int qR = std::clamp(int(std::lround(avgR / 17.0)) * 17, 0, 255);
            const int qG = std::clamp(int(std::lround(avgG / 17.0)) * 17, 0, 255);
            const int qB = std::clamp(int(std::lround(avgB / 17.0)) * 17, 0, 255);

            const quint32 hVal = mix32(seed ^ (quint32(gx) * 73856093u) ^ (quint32(gy) * 19349663u));
            const int jitter = int(hVal % 13u) - 6;

            const QRgb fillPx = qRgba(std::clamp(qR + jitter, 0, 255),
                                      std::clamp(qG + jitter, 0, 255),
                                      std::clamp(qB + jitter, 0, 255),
                                      255);

            for (int y = ly0; y < ly1; ++y) {
                QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
                std::fill(dst + lx0, dst + lx1, fillPx);
            }
        }
    }
    return out;
}

QImage blurPatch(const QImage &srcArgb32, int radiusPx)
{
    const int w = srcArgb32.width();
    const int h = srcArgb32.height();
    QImage cur = srcArgb32.copy();
    QImage tmp(w, h, QImage::Format_ARGB32_Premultiplied);

    std::vector<int> prefR(size_t(std::max(w, h)) + 1, 0);
    std::vector<int> prefG(size_t(std::max(w, h)) + 1, 0);
    std::vector<int> prefB(size_t(std::max(w, h)) + 1, 0);

    // 3 separable passes approximate a heavy Gaussian blur while clamping all reads strictly inside [0, w) x [0, h).
    for (int pass = 0; pass < 3; ++pass) {
        // Horizontal pass: cur -> tmp
        for (int y = 0; y < h; ++y) {
            const QRgb *src = reinterpret_cast<const QRgb *>(cur.constScanLine(y));
            QRgb *dst = reinterpret_cast<QRgb *>(tmp.scanLine(y));
            prefR[0] = prefG[0] = prefB[0] = 0;
            for (int x = 0; x < w; ++x) {
                prefR[size_t(x) + 1] = prefR[size_t(x)] + qRed(src[x]);
                prefG[size_t(x) + 1] = prefG[size_t(x)] + qGreen(src[x]);
                prefB[size_t(x) + 1] = prefB[size_t(x)] + qBlue(src[x]);
            }
            for (int x = 0; x < w; ++x) {
                const int x0 = std::max(0, x - radiusPx);
                const int x1 = std::min(w - 1, x + radiusPx);
                const int cnt = x1 - x0 + 1;
                const int r = (prefR[size_t(x1) + 1] - prefR[size_t(x0)] + cnt / 2) / cnt;
                const int g = (prefG[size_t(x1) + 1] - prefG[size_t(x0)] + cnt / 2) / cnt;
                const int b = (prefB[size_t(x1) + 1] - prefB[size_t(x0)] + cnt / 2) / cnt;
                dst[x] = qRgba(r, g, b, 255);
            }
        }

        // Vertical pass: tmp -> cur
        for (int x = 0; x < w; ++x) {
            prefR[0] = prefG[0] = prefB[0] = 0;
            for (int y = 0; y < h; ++y) {
                const QRgb px = reinterpret_cast<const QRgb *>(tmp.constScanLine(y))[x];
                prefR[size_t(y) + 1] = prefR[size_t(y)] + qRed(px);
                prefG[size_t(y) + 1] = prefG[size_t(y)] + qGreen(px);
                prefB[size_t(y) + 1] = prefB[size_t(y)] + qBlue(px);
            }
            for (int y = 0; y < h; ++y) {
                const int y0 = std::max(0, y - radiusPx);
                const int y1 = std::min(h - 1, y + radiusPx);
                const int cnt = y1 - y0 + 1;
                const int r = (prefR[size_t(y1) + 1] - prefR[size_t(y0)] + cnt / 2) / cnt;
                const int g = (prefG[size_t(y1) + 1] - prefG[size_t(y0)] + cnt / 2) / cnt;
                const int b = (prefB[size_t(y1) + 1] - prefB[size_t(y0)] + cnt / 2) / cnt;
                reinterpret_cast<QRgb *>(cur.scanLine(y))[x] = qRgba(r, g, b, 255);
            }
        }
    }

    return cur;
}

} // namespace

QRect redactionPhysicalRect(const QRectF &logicalItemRect,
                            const QPointF &logicalOrigin,
                            qreal nativeScale,
                            const QSize &imageSize)
{
    const QRectF norm = logicalItemRect.normalized();
    if (!norm.isValid() || norm.isEmpty() || nativeScale <= 0.0 || imageSize.isEmpty()) {
        return {};
    }

    const qreal scaledL = (norm.left() - logicalOrigin.x()) * nativeScale;
    const qreal scaledT = (norm.top() - logicalOrigin.y()) * nativeScale;
    const qreal scaledR = (norm.right() - logicalOrigin.x()) * nativeScale;
    const qreal scaledB = (norm.bottom() - logicalOrigin.y()) * nativeScale;

    const int x0 = std::clamp(int(std::floor(scaledL + 1e-9)), 0, imageSize.width());
    const int y0 = std::clamp(int(std::floor(scaledT + 1e-9)), 0, imageSize.height());
    const int x1 = std::clamp(int(std::ceil(scaledR - 1e-9)), 0, imageSize.width());
    const int y1 = std::clamp(int(std::ceil(scaledB - 1e-9)), 0, imageSize.height());

    if (x1 <= x0 || y1 <= y0) {
        return {};
    }
    return QRect(x0, y0, x1 - x0, y1 - y0);
}

QImage renderRedactedPatch(const QImage &sourceImage,
                           const QRect &physRect,
                           const QPoint &docGridPhysOrigin,
                           const AnnotationItem &item,
                           qreal nativeScale)
{
    if (sourceImage.isNull() || nativeScale <= 0.0) {
        return {};
    }
    const QRect bounded = physRect.intersected(sourceImage.rect());
    if (bounded.isEmpty()) {
        return {};
    }

    if (item.redactionMode == RedactionMode::Solid) {
        QImage out(bounded.size(), QImage::Format_ARGB32_Premultiplied);
        QColor solid = item.color;
        solid.setAlpha(255);
        out.fill(solid);
        return out;
    }

    const QImage src = sourceImage.copy(bounded).convertToFormat(QImage::Format_ARGB32);
    if (item.redactionMode == RedactionMode::Pixelate) {
        const int logicalBlock = std::clamp(item.redactionStrength, 12, 40);
        const int cellPx = std::max(1, int(std::lround(logicalBlock * nativeScale)));
        const QPoint adjustedOrigin = docGridPhysOrigin + (bounded.topLeft() - physRect.topLeft());
        return pixelatePatch(src, adjustedOrigin, cellPx, item.jitterSeed);
    }

    const int logicalRadius = std::clamp(item.redactionStrength, 14, 48);
    const int radiusPx = std::max(1, int(std::lround(logicalRadius * nativeScale)));
    return blurPatch(src, radiusPx);
}

} // namespace glimply
