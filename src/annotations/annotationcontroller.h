#pragma once

#include "annotationdocument.h"
#include "annotationitem.h"

#include <QColor>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <Qt>

class QKeyEvent;

namespace glimply
{

// Widget-agnostic interaction state machine for selecting, drawing, moving, resizing,
// duplicating, and undoing vector annotations in logical coordinates.
class AnnotationController : public QObject
{
    Q_OBJECT

public:
    explicit AnnotationController(AnnotationDocument *document = nullptr, QObject *parent = nullptr);

    void setDocument(AnnotationDocument *document);
    [[nodiscard]] AnnotationDocument *document() const;

    [[nodiscard]] ToolKind tool() const;
    void setTool(ToolKind tool);
    [[nodiscard]] ToolKind effectiveTool(Qt::KeyboardModifiers mods) const;

    void setCropToolEnabled(bool enabled);
    [[nodiscard]] bool cropToolEnabled() const;

    void setBounds(const QRectF &logicalBounds);
    [[nodiscard]] QRectF bounds() const;

    // Active tool style defaults.
    [[nodiscard]] QColor strokeColor() const;
    void setStrokeColor(const QColor &color);
    [[nodiscard]] QColor highlighterColor() const;
    void setHighlighterColor(const QColor &color);
    [[nodiscard]] qreal strokeWidth() const;
    void setStrokeWidth(qreal width);
    [[nodiscard]] qreal highlighterWidth() const;
    void setHighlighterWidth(qreal width);
    [[nodiscard]] ShapeFillMode fillMode() const;
    void setFillMode(ShapeFillMode mode);
    [[nodiscard]] qreal fillOpacity() const;
    void setFillOpacity(qreal opacity);
    [[nodiscard]] SpotlightShape spotlightShape() const;
    void setSpotlightShape(SpotlightShape shape);
    [[nodiscard]] qreal dimOpacity() const;
    void setDimOpacity(qreal opacity);
    [[nodiscard]] RedactionMode redactionMode() const;
    void setRedactionMode(RedactionMode mode);
    [[nodiscard]] QColor redactionColor() const;
    void setRedactionColor(const QColor &color);
    [[nodiscard]] int redactionStrength() const;
    void setRedactionStrength(int strength);

    // Live UI state queries.
    [[nodiscard]] int hoveredId() const;
    [[nodiscard]] bool isDragging() const;
    [[nodiscard]] const AnnotationItem *draftItem() const;
    [[nodiscard]] Qt::CursorShape cursorFor(const QPointF &logicalPos, Qt::KeyboardModifiers mods,
                                            qreal grabTolerance = 8.0) const;

    // Input entry points (logical coordinates). Return true when the event was handled.
    bool mousePress(const QPointF &logicalPos, Qt::MouseButton button, Qt::KeyboardModifiers mods,
                    qreal grabTolerance = 8.0);
    bool mouseMove(const QPointF &logicalPos, Qt::KeyboardModifiers mods);
    bool mouseRelease(const QPointF &logicalPos, Qt::MouseButton button, Qt::KeyboardModifiers mods);
    bool keyPress(QKeyEvent *event);
    bool keyRelease(QKeyEvent *event);
    void cancelActiveGesture();
    void clearHover();

Q_SIGNALS:
    void toolChanged(glimply::ToolKind tool);
    void styleChanged();
    void previewChanged();
    void cursorChanged();
    void textActivationRequested(const QPointF &logicalPos);

private:
    enum class DragMode {
        None,
        Create,
        Move,
        ResizeHandle,
        DuplicateMove,
    };

    void applyDrag(const QPointF &logicalPos, Qt::KeyboardModifiers mods);
    bool reapplyModifiers(QKeyEvent *event);
    [[nodiscard]] QPointF clampPoint(const QPointF &pos) const;
    void stepActiveSize(int direction);

    AnnotationDocument *doc_ = nullptr;
    ToolKind tool_ = ToolKind::Select;
    bool cropToolEnabled_ = true;
    QRectF bounds_;

    // Tool defaults.
    QColor strokeColor_ = QColor(0xE5, 0x39, 0x35);
    QColor highlighterColor_ = QColor(0xFD, 0xD8, 0x35);
    qreal strokeWidth_ = 4.0;
    qreal highlighterWidth_ = 20.0;
    ShapeFillMode fillMode_ = ShapeFillMode::Outline;
    qreal fillOpacity_ = 0.25;
    SpotlightShape spotlightShape_ = SpotlightShape::Rectangle;
    qreal dimOpacity_ = 0.65;
    RedactionMode redactionMode_ = RedactionMode::Solid;
    QColor redactionColor_ = QColor(0x00, 0x00, 0x00);
    int redactionStrength_ = 16;

    // Active gesture state.
    DragMode dragMode_ = DragMode::None;
    AnnotationHandle dragHandle_ = AnnotationHandle::None;
    AnnotationItem dragUndoBefore_;
    AnnotationItem dragBaseline_;
    AnnotationItem draftItem_;
    bool hasDraft_ = false;
    bool spaceHeld_ = false;
    QPointF anchor_;
    QPointF lastPos_;
    Qt::KeyboardModifiers lastMods_;
    int hoveredId_ = 0;
};

} // namespace glimply
