#include "annotationsettings.h"

#include <KConfig>
#include <KConfigGroup>

#include <algorithm>

namespace itsova
{
namespace
{

constexpr auto annotationsGroupName = "Annotations";

QColor readColorEntry(const KConfigGroup &group, const char *key, const QColor &fallback)
{
    const QString raw = group.readEntry(key, QString()).trimmed();
    if (raw.isEmpty()) {
        return fallback;
    }
    const QColor parsed = QColor::fromString(raw);
    return parsed.isValid() ? parsed : fallback;
}

} // namespace

QList<QColor> defaultAnnotationSwatches()
{
    return {
        QColor(0xE5, 0x39, 0x35), // Crimson Red
        QColor(0xFF, 0xB3, 0x00), // Amber
        QColor(0xFD, 0xD8, 0x35), // Highlighter Yellow
        QColor(0x43, 0xA0, 0x47), // Emerald Green
        QColor(0x1E, 0x88, 0xE5), // Breeze Blue
        QColor(0xFF, 0xFF, 0xFF), // White
        QColor(0x11, 0x11, 0x11), // Charcoal Black
    };
}

void AnnotationSettings::load(const KConfig &config)
{
    const AnnotationSettings defaults;
    const KConfigGroup group = config.group(QLatin1String(annotationsGroupName));

    strokeColor = readColorEntry(group, "StrokeColor", defaults.strokeColor);
    strokeColor.setAlpha(255);
    highlighterColor = readColorEntry(group, "HighlighterColor", defaults.highlighterColor);
    highlighterColor.setAlpha(255);

    strokeWidth = std::clamp(group.readEntry("StrokeWidth", defaults.strokeWidth), 2.0, 16.0);
    highlighterWidth = std::clamp(group.readEntry("HighlighterWidth", defaults.highlighterWidth), 8.0, 48.0);

    const QString fillStr = group.readEntry("ShapeFillMode", QStringLiteral("outline")).trimmed().toLower();
    shapeFillMode = fillStr == QLatin1String("fill") ? ShapeFillMode::TranslucentFill : ShapeFillMode::Outline;
    shapeFillOpacity = std::clamp(group.readEntry("ShapeFillOpacity", defaults.shapeFillOpacity), 0.10, 1.0);

    const QString spotStr = group.readEntry("SpotlightShape", QStringLiteral("rectangle")).trimmed().toLower();
    spotlightShape = spotStr == QLatin1String("ellipse") ? SpotlightShape::Ellipse : SpotlightShape::Rectangle;
    spotlightDimOpacity = std::clamp(group.readEntry("SpotlightDimOpacity", defaults.spotlightDimOpacity), 0.35, 0.85);

    fontFamily = group.readEntry("FontFamily", QString());
    fontPointSize = std::clamp(group.readEntry("FontPointSize", defaults.fontPointSize), 10, 72);
    fontBold = group.readEntry("FontBold", defaults.fontBold);
    fontItalic = group.readEntry("FontItalic", defaults.fontItalic);
    const QString alignStr = group.readEntry("TextAlignment", QStringLiteral("left")).trimmed().toLower();
    if (alignStr == QLatin1String("center")) {
        textAlignment = Qt::AlignHCenter;
    } else if (alignStr == QLatin1String("right")) {
        textAlignment = Qt::AlignRight;
    } else {
        textAlignment = Qt::AlignLeft;
    }

    const QString legStr = group.readEntry("TextLegibility", QStringLiteral("none")).trimmed().toLower();
    if (legStr == QLatin1String("shadow")) {
        textLegibility = TextLegibility::Shadow;
    } else if (legStr == QLatin1String("outline")) {
        textLegibility = TextLegibility::Outline;
    } else if (legStr == QLatin1String("pill")) {
        textLegibility = TextLegibility::Pill;
    } else {
        textLegibility = TextLegibility::None;
    }
    textColor = readColorEntry(group, "TextColor", defaults.textColor);
    textColor.setAlpha(255);

    const QString redactStr = group.readEntry("RedactionMode", QStringLiteral("solid")).trimmed().toLower();
    if (redactStr == QLatin1String("pixelate")) {
        redactionMode = RedactionMode::Pixelate;
    } else if (redactStr == QLatin1String("blur")) {
        redactionMode = RedactionMode::Blur;
    } else {
        redactionMode = RedactionMode::Solid;
    }
    redactionColor = readColorEntry(group, "RedactionColor", defaults.redactionColor);
    redactionColor.setAlpha(255);
    pixelateBlockSize = std::clamp(group.readEntry("PixelateBlockSize", defaults.pixelateBlockSize), 12, 40);
    blurRadius = std::clamp(group.readEntry("BlurRadius", defaults.blurRadius), 14, 48);
}

void AnnotationSettings::save(KConfig &config) const
{
    KConfigGroup group = config.group(QLatin1String(annotationsGroupName));
    group.writeEntry("StrokeColor", strokeColor.name(QColor::HexRgb));
    group.writeEntry("HighlighterColor", highlighterColor.name(QColor::HexRgb));
    group.writeEntry("StrokeWidth", std::clamp(strokeWidth, 2.0, 16.0));
    group.writeEntry("HighlighterWidth", std::clamp(highlighterWidth, 8.0, 48.0));
    group.writeEntry("ShapeFillMode",
                     shapeFillMode == ShapeFillMode::TranslucentFill ? QStringLiteral("fill")
                                                                     : QStringLiteral("outline"));
    group.writeEntry("ShapeFillOpacity", std::clamp(shapeFillOpacity, 0.10, 1.0));
    group.writeEntry("SpotlightShape",
                     spotlightShape == SpotlightShape::Ellipse ? QStringLiteral("ellipse")
                                                               : QStringLiteral("rectangle"));
    group.writeEntry("SpotlightDimOpacity", std::clamp(spotlightDimOpacity, 0.35, 0.85));

    group.writeEntry("FontFamily", fontFamily);
    group.writeEntry("FontPointSize", std::clamp(fontPointSize, 10, 72));
    group.writeEntry("FontBold", fontBold);
    group.writeEntry("FontItalic", fontItalic);
    group.writeEntry("TextAlignment",
                     textAlignment.testFlag(Qt::AlignHCenter) ? QStringLiteral("center")
                     : textAlignment.testFlag(Qt::AlignRight) ? QStringLiteral("right")
                                                              : QStringLiteral("left"));
    QString legStr = QStringLiteral("none");
    if (textLegibility == TextLegibility::Shadow) {
        legStr = QStringLiteral("shadow");
    } else if (textLegibility == TextLegibility::Outline) {
        legStr = QStringLiteral("outline");
    } else if (textLegibility == TextLegibility::Pill) {
        legStr = QStringLiteral("pill");
    }
    group.writeEntry("TextLegibility", legStr);
    group.writeEntry("TextColor", textColor.name(QColor::HexRgb));

    QString redactStr = QStringLiteral("solid");
    if (redactionMode == RedactionMode::Pixelate) {
        redactStr = QStringLiteral("pixelate");
    } else if (redactionMode == RedactionMode::Blur) {
        redactStr = QStringLiteral("blur");
    }
    group.writeEntry("RedactionMode", redactStr);
    group.writeEntry("RedactionColor", redactionColor.name(QColor::HexRgb));
    group.writeEntry("PixelateBlockSize", std::clamp(pixelateBlockSize, 12, 40));
    group.writeEntry("BlurRadius", std::clamp(blurRadius, 14, 48));
}

} // namespace itsova
