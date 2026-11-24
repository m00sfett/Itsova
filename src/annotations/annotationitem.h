#pragma once

#include <QColor>
#include <QFont>
#include <QList>
#include <QPair>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QtGlobal>

namespace glimply
{

enum class ToolKind {
    Select,
    Crop,
    Arrow,
    Rectangle,
    Ellipse,
    Pen,
    Highlighter,
    Text,
    Step,
    Redact,
    Spotlight,
};

enum class ItemKind {
    Arrow,
    Rectangle,
    Ellipse,
    Pen,
    Highlighter,
    Text,
    Step,
    Redact,
    Spotlight,
};

enum class ShapeFillMode {
    Outline,
    TranslucentFill,
};

enum class SpotlightShape {
    Rectangle,
    Ellipse,
};

enum class TextLegibility {
    None,
    Shadow,
    Outline,
    Pill,
};

enum class RedactionMode {
    Solid,
    Pixelate,
    Blur,
};

enum class AnnotationHandle {
    None,
    TopLeft,
    Top,
    TopRight,
    Right,
    BottomRight,
    Bottom,
    BottomLeft,
    Left,
    StartPoint,
    EndPoint,
};

// Non-destructive vector annotation item stored in logical coordinates.
struct AnnotationItem {
    int id = 0;
    ItemKind kind = ItemKind::Rectangle;

    // Logical geometry: rect for box-shaped items (Rectangle, Ellipse, Text, Redact, Spotlight),
    // start/end for Arrow and Step (start = badge center, end = optional pointer tip),
    // points for freehand Pen and Highlighter.
    QRectF rect;
    QPointF start;
    QPointF end;
    QList<QPointF> points;

    // Shared visual styling.
    QColor color = QColor(0xE5, 0x39, 0x35);
    qreal strokeWidth = 4.0;
    ShapeFillMode fillMode = ShapeFillMode::Outline;
    qreal fillOpacity = 0.25;

    // Step counter fields.
    int stepNumber = 1;
    bool hasPointer = false;

    // Spotlight fields.
    SpotlightShape spotlightShape = SpotlightShape::Rectangle;
    qreal dimOpacity = 0.65;

    // Text fields (#14).
    QString text;
    bool paragraphMode = false;
    QFont font;
    Qt::Alignment textAlignment = Qt::AlignLeft;
    TextLegibility legibility = TextLegibility::None;

    // Redaction fields (#15).
    RedactionMode redactionMode = RedactionMode::Solid;
    int redactionStrength = 16;
    quint32 jitterSeed = 0;

    [[nodiscard]] qreal badgeRadius() const;
    [[nodiscard]] QRectF rawBounds() const;
    [[nodiscard]] QRectF boundingRect() const;
    [[nodiscard]] bool intersectsLogicalRect(const QRectF &cropRect) const;

    void translate(const QPointF &delta);

    [[nodiscard]] QList<QPair<AnnotationHandle, QPointF>> handles() const;
    [[nodiscard]] AnnotationHandle handleAt(const QPointF &logicalPos, qreal grabRadius = 8.0) const;
    [[nodiscard]] bool hitTest(const QPointF &logicalPos, qreal tolerance = 6.0) const;
};

[[nodiscard]] QColor contrastingTextColor(const QColor &background);

} // namespace glimply
