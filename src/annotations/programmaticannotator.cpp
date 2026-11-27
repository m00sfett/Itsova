#include "programmaticannotator.h"

#include "annotationdocument.h"
#include "annotationitem.h"
#include "annotationrenderer.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QImageReader>
#include <QJsonObject>

#include <algorithm>
#include <cmath>

namespace itsova
{
namespace
{

class AnnotatorText
{
    Q_DECLARE_TR_FUNCTIONS(ProgrammaticAnnotator)
};

QColor parseColor(const QJsonValue &val, const QColor &fallback)
{
    if (!val.isString()) {
        return fallback;
    }
    const QColor c(val.toString().trimmed());
    return c.isValid() ? c : fallback;
}

bool readFiniteDouble(const QJsonObject &obj, const QString &key, double *out)
{
    const QJsonValue v = obj.value(key);
    if (!v.isDouble()) {
        return false;
    }
    const double d = v.toDouble();
    if (!std::isfinite(d)) {
        return false;
    }
    *out = d;
    return true;
}

bool readRect(const QJsonObject &obj, QRectF *outRect)
{
    double x = 0;
    double y = 0;
    double w = 0;
    double h = 0;
    if (!readFiniteDouble(obj, QStringLiteral("x"), &x)
        || !readFiniteDouble(obj, QStringLiteral("y"), &y)
        || !readFiniteDouble(obj, QStringLiteral("width"), &w)
        || !readFiniteDouble(obj, QStringLiteral("height"), &h) || w <= 0.0 || h <= 0.0) {
        return false;
    }
    *outRect = QRectF(x, y, w, h);
    return true;
}

bool readPoints(const QJsonObject &obj, QList<QPointF> *outPoints)
{
    const QJsonValue v = obj.value(QStringLiteral("points"));
    if (!v.isArray()) {
        return false;
    }
    const QJsonArray arr = v.toArray();
    if (arr.size() < 2) {
        return false;
    }
    QList<QPointF> pts;
    pts.reserve(arr.size());
    for (const QJsonValue &ptVal : arr) {
        if (ptVal.isArray()) {
            const QJsonArray pair = ptVal.toArray();
            if (pair.size() < 2 || !pair.at(0).isDouble() || !pair.at(1).isDouble()) {
                return false;
            }
            const double px = pair.at(0).toDouble();
            const double py = pair.at(1).toDouble();
            if (!std::isfinite(px) || !std::isfinite(py)) {
                return false;
            }
            pts.append(QPointF(px, py));
        } else if (ptVal.isObject()) {
            double px = 0;
            double py = 0;
            const QJsonObject ptObj = ptVal.toObject();
            if (!readFiniteDouble(ptObj, QStringLiteral("x"), &px)
                || !readFiniteDouble(ptObj, QStringLiteral("y"), &py)) {
                return false;
            }
            pts.append(QPointF(px, py));
        } else {
            return false;
        }
    }
    *outPoints = std::move(pts);
    return true;
}

} // namespace

AnnotateResult applyProgrammaticAnnotations(const AnnotateRequest &request)
{
    AnnotateResult res;
    const QString inputTrimmed = request.inputPath.trimmed();
    if (inputTrimmed.isEmpty()) {
        res.error = AnnotatorText::tr("Input image path is required.");
        return res;
    }

    const QFileInfo inInfo(inputTrimmed);
    if (!inInfo.exists() || !inInfo.isFile()) {
        res.error = AnnotatorText::tr("Input file does not exist: %1").arg(inputTrimmed);
        return res;
    }
    const QString absInput = inInfo.absoluteFilePath();
    const QString canonicalInput = inInfo.canonicalFilePath();

    QImageReader reader(absInput);
    reader.setAutoTransform(true);
    QImage source = reader.read();
    if (source.isNull()) {
        res.error = AnnotatorText::tr("Could not read input image: %1").arg(reader.errorString());
        return res;
    }
    source.setDevicePixelRatio(1.0);

    ExportOptions opts = request.exportOptions;
    if (!request.formatExplicit) {
        const QString probePath =
            !request.outputPath.trimmed().isEmpty() ? request.outputPath.trimmed() : absInput;
        if (const std::optional<ImageFormat> fmt = imageFormatFromPath(probePath); fmt && isImageFormatSupported(*fmt)) {
            opts.format = *fmt;
        } else {
            opts.format = ImageFormat::Png;
        }
    }
    if (!isImageFormatSupported(opts.format)) {
        res.error = AnnotatorText::tr("Requested export image format is not supported on this system.");
        return res;
    }

    AnnotationDocument doc;
    int applied = 0;
    for (int idx = 0; idx < request.operations.size(); ++idx) {
        const QJsonValue opVal = request.operations.at(idx);
        if (!opVal.isObject()) {
            res.error = AnnotatorText::tr("Operation #%1 must be a JSON object.").arg(idx);
            return res;
        }
        const QJsonObject op = opVal.toObject();
        const QString type = op.value(QStringLiteral("type")).toString().trimmed().toLower();
        AnnotationItem item;
        item.color = parseColor(op.value(QStringLiteral("color")), QColor(0xE5, 0x39, 0x35));
        item.strokeWidth = std::clamp(op.value(QStringLiteral("stroke_width")).toDouble(4.0), 1.0, 48.0);

        if (type == QLatin1String("arrow")) {
            double x1 = 0;
            double y1 = 0;
            double x2 = 0;
            double y2 = 0;
            if (!readFiniteDouble(op, QStringLiteral("x1"), &x1)
                || !readFiniteDouble(op, QStringLiteral("y1"), &y1)
                || !readFiniteDouble(op, QStringLiteral("x2"), &x2)
                || !readFiniteDouble(op, QStringLiteral("y2"), &y2)) {
                res.error = AnnotatorText::tr("Arrow operation #%1 requires x1, y1, x2, and y2.").arg(idx);
                return res;
            }
            item.kind = ItemKind::Arrow;
            item.start = QPointF(x1, y1);
            item.end = QPointF(x2, y2);
            doc.addItem(std::move(item));
        } else if (type == QLatin1String("rect") || type == QLatin1String("ellipse")) {
            QRectF r;
            if (!readRect(op, &r)) {
                res.error = AnnotatorText::tr("Shape operation #%1 requires x, y, width > 0, height > 0.").arg(idx);
                return res;
            }
            item.kind = (type == QLatin1String("rect")) ? ItemKind::Rectangle : ItemKind::Ellipse;
            item.rect = r;
            const QString fillMode = op.value(QStringLiteral("fill_mode")).toString().trimmed().toLower();
            if (fillMode == QLatin1String("translucent")) {
                item.fillMode = ShapeFillMode::TranslucentFill;
            }
            item.fillOpacity = std::clamp(op.value(QStringLiteral("fill_opacity")).toDouble(0.25), 0.05, 0.9);
            doc.addItem(std::move(item));
        } else if (type == QLatin1String("pen") || type == QLatin1String("highlighter")) {
            QList<QPointF> pts;
            if (!readPoints(op, &pts)) {
                res.error = AnnotatorText::tr("Stroke operation #%1 requires at least 2 points.").arg(idx);
                return res;
            }
            item.kind = (type == QLatin1String("pen")) ? ItemKind::Pen : ItemKind::Highlighter;
            if (type == QLatin1String("highlighter")) {
                if (!op.contains(QStringLiteral("color"))) {
                    item.color = QColor(0xFF, 0xEB, 0x3B);
                }
                if (!op.contains(QStringLiteral("stroke_width"))) {
                    item.strokeWidth = 18.0;
                }
            }
            item.points = std::move(pts);
            doc.addItem(std::move(item));
        } else if (type == QLatin1String("text")) {
            double x = 0;
            double y = 0;
            const QString text = op.value(QStringLiteral("text")).toString();
            if (!readFiniteDouble(op, QStringLiteral("x"), &x)
                || !readFiniteDouble(op, QStringLiteral("y"), &y) || text.isEmpty()) {
                res.error = AnnotatorText::tr("Text operation #%1 requires x, y, and non-empty text.").arg(idx);
                return res;
            }
            item.kind = ItemKind::Text;
            item.text = text;
            const double w = op.value(QStringLiteral("width")).toDouble(0.0);
            const double h = op.value(QStringLiteral("height")).toDouble(0.0);
            item.paragraphMode = std::isfinite(w) && w > 0.0;
            item.rect = QRectF(x, y, item.paragraphMode ? w : 100.0, (std::isfinite(h) && h > 0.0) ? h : 28.0);
            QFont font;
            const int ptSize = std::clamp(op.value(QStringLiteral("font_size")).toInt(16), 6, 128);
            font.setPointSize(ptSize);
            font.setBold(op.value(QStringLiteral("bold")).toBool(false));
            font.setItalic(op.value(QStringLiteral("italic")).toBool(false));
            item.font = font;
            const QString leg = op.value(QStringLiteral("legibility")).toString().trimmed().toLower();
            if (leg == QLatin1String("shadow")) {
                item.legibility = TextLegibility::Shadow;
            } else if (leg == QLatin1String("outline")) {
                item.legibility = TextLegibility::Outline;
            } else if (leg == QLatin1String("pill")) {
                item.legibility = TextLegibility::Pill;
            }
            doc.addItem(std::move(item));
        } else if (type == QLatin1String("step")) {
            double x = 0;
            double y = 0;
            if (!readFiniteDouble(op, QStringLiteral("x"), &x)
                || !readFiniteDouble(op, QStringLiteral("y"), &y)) {
                res.error = AnnotatorText::tr("Step operation #%1 requires x and y.").arg(idx);
                return res;
            }
            item.kind = ItemKind::Step;
            item.start = QPointF(x, y);
            double px = 0;
            double py = 0;
            if (readFiniteDouble(op, QStringLiteral("pointer_x"), &px)
                && readFiniteDouble(op, QStringLiteral("pointer_y"), &py)) {
                item.hasPointer = true;
                item.end = QPointF(px, py);
            }
            const int explicitNumber = op.value(QStringLiteral("number")).toInt(0);
            const int assignedId = doc.addItem(std::move(item));
            if (explicitNumber > 0) {
                if (const AnnotationItem *added = doc.findItem(assignedId)) {
                    AnnotationItem updated = *added;
                    updated.stepNumber = explicitNumber;
                    doc.commitModification(*added, updated);
                }
            }
        } else if (type == QLatin1String("spotlight")) {
            QRectF r;
            if (!readRect(op, &r)) {
                res.error = AnnotatorText::tr("Spotlight operation #%1 requires x, y, width > 0, height > 0.").arg(idx);
                return res;
            }
            item.kind = ItemKind::Spotlight;
            item.rect = r;
            const QString shape = op.value(QStringLiteral("shape")).toString().trimmed().toLower();
            item.spotlightShape =
                (shape == QLatin1String("ellipse")) ? SpotlightShape::Ellipse : SpotlightShape::Rectangle;
            item.dimOpacity = std::clamp(op.value(QStringLiteral("dim_opacity")).toDouble(0.65), 0.1, 0.95);
            doc.addItem(std::move(item));
        } else if (type == QLatin1String("redact")) {
            QRectF r;
            if (!readRect(op, &r)) {
                res.error = AnnotatorText::tr("Redact operation #%1 requires x, y, width > 0, height > 0.").arg(idx);
                return res;
            }
            item.kind = ItemKind::Redact;
            item.rect = r;
            item.color = parseColor(op.value(QStringLiteral("color")), QColor(0x00, 0x00, 0x00));
            item.color.setAlpha(255);
            const QString mode = op.value(QStringLiteral("mode")).toString().trimmed().toLower();
            if (mode == QLatin1String("pixelate")) {
                item.redactionMode = RedactionMode::Pixelate;
            } else if (mode == QLatin1String("blur")) {
                item.redactionMode = RedactionMode::Blur;
            } else {
                item.redactionMode = RedactionMode::Solid;
            }
            item.redactionStrength = std::clamp(op.value(QStringLiteral("strength")).toInt(16), 4, 48);
            doc.addItem(std::move(item));
        } else {
            res.error = AnnotatorText::tr("Unsupported annotation type '%1' at operation #%2.").arg(type).arg(idx);
            return res;
        }
        ++applied;
    }

    const QImage rendered = renderAnnotatedCrop(source, QRectF(source.rect()), 1.0, doc);
    if (rendered.isNull()) {
        res.error = AnnotatorText::tr("Failed to render annotated image.");
        return res;
    }

    QString writtenPath;
    QString writeErr;
    const QString requestedOut = request.outputPath.trimmed();
    if (requestedOut.isEmpty()) {
        const QString baseName = inInfo.completeBaseName() + QStringLiteral("-annotated.") + canonicalExtension(opts.format);
        if (!writeUniqueImageFile(inInfo.absolutePath(), baseName, rendered, opts, &writtenPath, &writeErr)) {
            res.error = writeErr;
            return res;
        }
    } else {
        const QFileInfo outInfo(requestedOut);
        const QString absOut = outInfo.absoluteFilePath();
        const QString canonicalOut = outInfo.exists() ? outInfo.canonicalFilePath() : absOut;
        const bool sameFile = (absOut == absInput) || (!canonicalInput.isEmpty() && canonicalOut == canonicalInput);
        if (sameFile && !request.overwrite) {
            res.error = AnnotatorText::tr("Refusing to overwrite input file '%1' without overwrite=true.").arg(absInput);
            return res;
        }
        if (!writeImageToFile(absOut, rendered, opts, &writeErr)) {
            res.error = writeErr;
            return res;
        }
        writtenPath = absOut;
    }

    res.ok = true;
    res.outputPath = writtenPath;
    res.width = rendered.width();
    res.height = rendered.height();
    res.format = formatConfigKey(opts.format);
    res.bytes = QFileInfo(writtenPath).size();
    res.appliedCount = applied;
    return res;
}

} // namespace itsova
