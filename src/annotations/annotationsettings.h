#pragma once

#include "annotationitem.h"

#include <QColor>
#include <QList>
#include <QString>
#include <Qt>

class KConfig;

namespace itsova
{

// Returns the curated 7-color quick palette used by markup tools.
[[nodiscard]] QList<QColor> defaultAnnotationSwatches();

// Persistent user defaults for markup, text (#14), and redaction (#15) tools, stored under
// [Annotations] in itsovarc.
struct AnnotationSettings {
    QColor strokeColor = QColor(0xE5, 0x39, 0x35);
    QColor highlighterColor = QColor(0xFD, 0xD8, 0x35);
    qreal strokeWidth = 4.0;
    qreal highlighterWidth = 20.0;
    ShapeFillMode shapeFillMode = ShapeFillMode::Outline;
    qreal shapeFillOpacity = 0.25;
    SpotlightShape spotlightShape = SpotlightShape::Rectangle;
    qreal spotlightDimOpacity = 0.65;

    // Text defaults (#14).
    QString fontFamily;
    int fontPointSize = 16;
    bool fontBold = false;
    bool fontItalic = false;
    Qt::Alignment textAlignment = Qt::AlignLeft;
    TextLegibility textLegibility = TextLegibility::None;
    QColor textColor = QColor(0xE5, 0x39, 0x35);

    // Redaction defaults (#15).
    RedactionMode redactionMode = RedactionMode::Solid;
    QColor redactionColor = QColor(0x00, 0x00, 0x00);
    int pixelateBlockSize = 16;
    int blurRadius = 22;

    void load(const KConfig &config);
    void save(KConfig &config) const;
};

} // namespace itsova
