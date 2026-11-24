#include "annotationcontroller.h"

#include "annotationgeometry.h"

#include <QKeyEvent>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>
#include <utility>

namespace glimply
{
namespace
{

bool itemsEquivalent(const AnnotationItem &a, const AnnotationItem &b)
{
    return a.id == b.id && a.kind == b.kind && a.rect == b.rect && a.start == b.start && a.end == b.end
        && a.points == b.points && a.color == b.color && a.strokeWidth == b.strokeWidth
        && a.fillMode == b.fillMode && a.fillOpacity == b.fillOpacity && a.stepNumber == b.stepNumber
        && a.hasPointer == b.hasPointer && a.spotlightShape == b.spotlightShape
        && a.dimOpacity == b.dimOpacity && a.text == b.text && a.paragraphMode == b.paragraphMode
        && a.font == b.font && a.textAlignment == b.textAlignment && a.legibility == b.legibility
        && a.redactionMode == b.redactionMode && a.redactionStrength == b.redactionStrength
        && a.jitterSeed == b.jitterSeed;
}

Qt::CursorShape cursorForHandle(AnnotationHandle handle)
{
    switch (handle) {
    case AnnotationHandle::TopLeft:
    case AnnotationHandle::BottomRight:
        return Qt::SizeFDiagCursor;
    case AnnotationHandle::TopRight:
    case AnnotationHandle::BottomLeft:
        return Qt::SizeBDiagCursor;
    case AnnotationHandle::Left:
    case AnnotationHandle::Right:
        return Qt::SizeHorCursor;
    case AnnotationHandle::Top:
    case AnnotationHandle::Bottom:
        return Qt::SizeVerCursor;
    case AnnotationHandle::StartPoint:
    case AnnotationHandle::EndPoint:
        return Qt::SizeAllCursor;
    case AnnotationHandle::None:
        return Qt::ArrowCursor;
    }
    return Qt::ArrowCursor;
}

ItemKind itemKindForTool(ToolKind tool)
{
    switch (tool) {
    case ToolKind::Arrow:
        return ItemKind::Arrow;
    case ToolKind::Rectangle:
        return ItemKind::Rectangle;
    case ToolKind::Ellipse:
        return ItemKind::Ellipse;
    case ToolKind::Pen:
        return ItemKind::Pen;
    case ToolKind::Highlighter:
        return ItemKind::Highlighter;
    case ToolKind::Text:
        return ItemKind::Text;
    case ToolKind::Step:
        return ItemKind::Step;
    case ToolKind::Redact:
        return ItemKind::Redact;
    case ToolKind::Spotlight:
        return ItemKind::Spotlight;
    case ToolKind::Select:
    case ToolKind::Crop:
        break;
    }
    return ItemKind::Rectangle;
}

} // namespace

AnnotationController::AnnotationController(AnnotationDocument *document, QObject *parent)
    : QObject(parent)
    , doc_(document)
{
}

void AnnotationController::setDocument(AnnotationDocument *document)
{
    if (doc_ == document) {
        return;
    }
    cancelActiveGesture();
    doc_ = document;
    hoveredId_ = 0;
    Q_EMIT previewChanged();
}

AnnotationDocument *AnnotationController::document() const
{
    return doc_;
}

ToolKind AnnotationController::tool() const
{
    return tool_;
}

void AnnotationController::setTool(ToolKind tool)
{
    if (tool == ToolKind::Crop && !cropToolEnabled_) {
        tool = ToolKind::Select;
    }
    if (tool_ == tool) {
        return;
    }
    cancelActiveGesture();
    tool_ = tool;
    if (tool_ != ToolKind::Select && doc_) {
        hoveredId_ = 0;
    }
    Q_EMIT toolChanged(tool_);
    Q_EMIT previewChanged();
    Q_EMIT cursorChanged();
}

ToolKind AnnotationController::effectiveTool(Qt::KeyboardModifiers mods) const
{
    if (dragMode_ == DragMode::None && mods.testFlag(Qt::ControlModifier) && tool_ != ToolKind::Select
        && tool_ != ToolKind::Crop) {
        return ToolKind::Select;
    }
    return tool_;
}

void AnnotationController::setCropToolEnabled(bool enabled)
{
    cropToolEnabled_ = enabled;
    if (!cropToolEnabled_ && tool_ == ToolKind::Crop) {
        setTool(ToolKind::Select);
    }
}

bool AnnotationController::cropToolEnabled() const
{
    return cropToolEnabled_;
}

void AnnotationController::setBounds(const QRectF &logicalBounds)
{
    bounds_ = logicalBounds;
}

QRectF AnnotationController::bounds() const
{
    return bounds_;
}

QColor AnnotationController::strokeColor() const { return strokeColor_; }
void AnnotationController::setStrokeColor(const QColor &color)
{
    if (strokeColor_ != color) {
        strokeColor_ = color;
        Q_EMIT styleChanged();
    }
}

QColor AnnotationController::highlighterColor() const { return highlighterColor_; }
void AnnotationController::setHighlighterColor(const QColor &color)
{
    if (highlighterColor_ != color) {
        highlighterColor_ = color;
        Q_EMIT styleChanged();
    }
}

qreal AnnotationController::strokeWidth() const { return strokeWidth_; }
void AnnotationController::setStrokeWidth(qreal width)
{
    const qreal clamped = std::clamp(width, 2.0, 16.0);
    if (!qFuzzyCompare(strokeWidth_, clamped)) {
        strokeWidth_ = clamped;
        Q_EMIT styleChanged();
    }
}

qreal AnnotationController::highlighterWidth() const { return highlighterWidth_; }
void AnnotationController::setHighlighterWidth(qreal width)
{
    const qreal clamped = std::clamp(width, 8.0, 48.0);
    if (!qFuzzyCompare(highlighterWidth_, clamped)) {
        highlighterWidth_ = clamped;
        Q_EMIT styleChanged();
    }
}

ShapeFillMode AnnotationController::fillMode() const { return fillMode_; }
void AnnotationController::setFillMode(ShapeFillMode mode)
{
    if (fillMode_ != mode) {
        fillMode_ = mode;
        Q_EMIT styleChanged();
    }
}

qreal AnnotationController::fillOpacity() const { return fillOpacity_; }
void AnnotationController::setFillOpacity(qreal opacity)
{
    const qreal clamped = std::clamp(opacity, 0.10, 1.0);
    if (!qFuzzyCompare(fillOpacity_, clamped)) {
        fillOpacity_ = clamped;
        Q_EMIT styleChanged();
    }
}

SpotlightShape AnnotationController::spotlightShape() const { return spotlightShape_; }
void AnnotationController::setSpotlightShape(SpotlightShape shape)
{
    if (spotlightShape_ != shape) {
        spotlightShape_ = shape;
        Q_EMIT styleChanged();
    }
}

qreal AnnotationController::dimOpacity() const { return dimOpacity_; }
void AnnotationController::setDimOpacity(qreal opacity)
{
    const qreal clamped = std::clamp(opacity, 0.35, 0.85);
    if (!qFuzzyCompare(dimOpacity_, clamped)) {
        dimOpacity_ = clamped;
        Q_EMIT styleChanged();
    }
}

RedactionMode AnnotationController::redactionMode() const { return redactionMode_; }
void AnnotationController::setRedactionMode(RedactionMode mode)
{
    if (redactionMode_ != mode) {
        redactionMode_ = mode;
        Q_EMIT styleChanged();
    }
}

QColor AnnotationController::redactionColor() const { return redactionColor_; }
void AnnotationController::setRedactionColor(const QColor &color)
{
    QColor opaque = color;
    opaque.setAlpha(255);
    if (redactionColor_ != opaque) {
        redactionColor_ = opaque;
        Q_EMIT styleChanged();
    }
}

int AnnotationController::redactionStrength() const { return redactionStrength_; }
void AnnotationController::setRedactionStrength(int strength)
{
    if (redactionStrength_ != strength) {
        redactionStrength_ = strength;
        Q_EMIT styleChanged();
    }
}

int AnnotationController::hoveredId() const { return hoveredId_; }
bool AnnotationController::isDragging() const { return dragMode_ != DragMode::None; }
const AnnotationItem *AnnotationController::draftItem() const { return hasDraft_ ? &draftItem_ : nullptr; }

Qt::CursorShape AnnotationController::cursorFor(const QPointF &logicalPos, Qt::KeyboardModifiers mods,
                                                qreal grabTolerance) const
{
    if (dragMode_ == DragMode::Move || dragMode_ == DragMode::DuplicateMove) {
        return Qt::SizeAllCursor;
    }
    if (dragMode_ == DragMode::ResizeHandle) {
        return cursorForHandle(dragHandle_);
    }
    const ToolKind active = effectiveTool(mods);
    if (active == ToolKind::Select) {
        if (doc_) {
            if (const AnnotationItem *sel = doc_->selectedItem()) {
                const AnnotationHandle handle = sel->handleAt(logicalPos, grabTolerance);
                if (handle != AnnotationHandle::None) {
                    return cursorForHandle(handle);
                }
                if (sel->hitTest(logicalPos)) {
                    return Qt::SizeAllCursor;
                }
            }
            if (doc_->topmostHitAt(logicalPos) > 0) {
                return Qt::PointingHandCursor;
            }
        }
        return Qt::ArrowCursor;
    }
    if (active == ToolKind::Text) {
        return Qt::IBeamCursor;
    }
    return Qt::CrossCursor;
}

QPointF AnnotationController::clampPoint(const QPointF &pos) const
{
    if (!bounds_.isValid()) {
        return pos;
    }
    return QPointF(std::clamp(pos.x(), bounds_.left(), bounds_.right()),
                   std::clamp(pos.y(), bounds_.top(), bounds_.bottom()));
}

bool AnnotationController::mousePress(const QPointF &logicalPos, Qt::MouseButton button,
                                      Qt::KeyboardModifiers mods, qreal grabTolerance)
{
    if (!doc_ || button != Qt::LeftButton) {
        return false;
    }
    const ToolKind active = effectiveTool(mods);
    if (active == ToolKind::Crop) {
        return false;
    }

    lastPos_ = logicalPos;
    lastMods_ = mods;
    spaceHeld_ = false;

    if (active == ToolKind::Select) {
        if (const AnnotationItem *sel = doc_->selectedItem()) {
            const AnnotationHandle handle = sel->handleAt(logicalPos, grabTolerance);
            if (handle != AnnotationHandle::None) {
                dragMode_ = DragMode::ResizeHandle;
                dragHandle_ = handle;
                dragUndoBefore_ = *sel;
                dragBaseline_ = *sel;
                anchor_ = logicalPos;
                Q_EMIT cursorChanged();
                return true;
            }
        }
        const int hitId = doc_->topmostHitAt(logicalPos);
        if (hitId > 0) {
            doc_->select(hitId);
            const AnnotationItem *hit = doc_->findItem(hitId);
            if (!hit) {
                return false;
            }
            dragUndoBefore_ = *hit;
            dragBaseline_ = *hit;
            anchor_ = logicalPos;
            if (mods.testFlag(Qt::AltModifier)) {
                dragMode_ = DragMode::DuplicateMove;
                draftItem_ = *hit;
                draftItem_.id = 0;
                hasDraft_ = true;
            } else {
                dragMode_ = DragMode::Move;
            }
            Q_EMIT previewChanged();
            Q_EMIT cursorChanged();
            return true;
        }
        doc_->clearSelection();
        Q_EMIT cursorChanged();
        return false;
    }

    if (active == ToolKind::Text) {
        Q_EMIT textActivationRequested(clampPoint(logicalPos));
        return true;
    }

    doc_->clearSelection();
    const QPointF startPos = clampPoint(logicalPos);
    anchor_ = startPos;
    lastPos_ = startPos;
    dragMode_ = DragMode::Create;
    hasDraft_ = true;
    draftItem_ = AnnotationItem{};
    draftItem_.kind = itemKindForTool(active);

    switch (active) {
    case ToolKind::Arrow:
        draftItem_.start = startPos;
        draftItem_.end = startPos;
        draftItem_.color = strokeColor_;
        draftItem_.strokeWidth = strokeWidth_;
        break;
    case ToolKind::Rectangle:
    case ToolKind::Ellipse:
        draftItem_.rect = QRectF(startPos, QSizeF(0.0, 0.0));
        draftItem_.color = strokeColor_;
        draftItem_.strokeWidth = strokeWidth_;
        draftItem_.fillMode = fillMode_;
        draftItem_.fillOpacity = fillOpacity_;
        break;
    case ToolKind::Pen:
        draftItem_.points = {startPos};
        draftItem_.color = strokeColor_;
        draftItem_.strokeWidth = strokeWidth_;
        break;
    case ToolKind::Highlighter:
        draftItem_.points = {startPos};
        draftItem_.color = highlighterColor_;
        draftItem_.strokeWidth = highlighterWidth_;
        break;
    case ToolKind::Step:
        draftItem_.start = startPos;
        draftItem_.end = startPos;
        draftItem_.hasPointer = false;
        draftItem_.stepNumber = doc_->nextStepNumber();
        draftItem_.color = strokeColor_;
        draftItem_.strokeWidth = strokeWidth_;
        break;
    case ToolKind::Spotlight:
        draftItem_.rect = QRectF(startPos, QSizeF(0.0, 0.0));
        draftItem_.spotlightShape = spotlightShape_;
        draftItem_.dimOpacity = dimOpacity_;
        break;
    case ToolKind::Redact:
        draftItem_.rect = QRectF(startPos, QSizeF(0.0, 0.0));
        draftItem_.redactionMode = redactionMode_;
        draftItem_.color = redactionMode_ == RedactionMode::Solid ? redactionColor_ : QColor(0, 0, 0);
        draftItem_.color.setAlpha(255);
        draftItem_.redactionStrength = redactionStrength_;
        draftItem_.jitterSeed = QRandomGenerator::global()->generate();
        break;
    case ToolKind::Select:
    case ToolKind::Crop:
    case ToolKind::Text:
        break;
    }

    Q_EMIT previewChanged();
    Q_EMIT cursorChanged();
    return true;
}

void AnnotationController::applyDrag(const QPointF &logicalPos, Qt::KeyboardModifiers mods)
{
    const DrawModifiers drawMods{mods.testFlag(Qt::ShiftModifier), mods.testFlag(Qt::AltModifier)};

    if (dragMode_ == DragMode::Move) {
        AnnotationItem moved = dragBaseline_;
        moved.translate(logicalPos - anchor_);
        doc_->setItemPreview(moved);
        return;
    }

    if (dragMode_ == DragMode::DuplicateMove) {
        draftItem_ = dragBaseline_;
        draftItem_.id = 0;
        draftItem_.translate(logicalPos - anchor_);
        return;
    }

    if (dragMode_ == DragMode::ResizeHandle) {
        if (spaceHeld_) {
            const QPointF step = logicalPos - lastPos_;
            anchor_ += step;
            dragBaseline_.translate(step);
        }
        const AnnotationItem resized = applyHandleDrag(dragBaseline_, dragHandle_, logicalPos - anchor_, drawMods);
        doc_->setItemPreview(resized);
        return;
    }

    if (dragMode_ == DragMode::Create) {
        const QPointF cur = clampPoint(logicalPos);
        if (spaceHeld_) {
            const QPointF step = cur - lastPos_;
            anchor_ += step;
            draftItem_.translate(step);
            return;
        }
        switch (draftItem_.kind) {
        case ItemKind::Arrow:
            draftItem_.start = anchor_;
            draftItem_.end = drawMods.constrain ? snapAngle15(anchor_, cur) : cur;
            break;
        case ItemKind::Step: {
            draftItem_.start = anchor_;
            const QPointF tip = drawMods.constrain ? snapAngle15(anchor_, cur) : cur;
            const qreal dist = std::hypot(tip.x() - anchor_.x(), tip.y() - anchor_.y());
            draftItem_.hasPointer = dist >= 8.0;
            draftItem_.end = draftItem_.hasPointer ? tip : anchor_;
            break;
        }
        case ItemKind::Pen:
        case ItemKind::Highlighter: {
            if (draftItem_.points.isEmpty()) {
                draftItem_.points.append(cur);
            } else {
                const QPointF &prev = draftItem_.points.last();
                if (std::hypot(cur.x() - prev.x(), cur.y() - prev.y()) >= 1.5) {
                    draftItem_.points.append(cur);
                }
            }
            break;
        }
        case ItemKind::Rectangle:
        case ItemKind::Ellipse:
        case ItemKind::Spotlight:
        case ItemKind::Redact:
            draftItem_.rect = constrainedBox(anchor_, cur, drawMods, bounds_);
            break;
        case ItemKind::Text:
            break;
        }
    }
}

bool AnnotationController::mouseMove(const QPointF &logicalPos, Qt::KeyboardModifiers mods)
{
    lastMods_ = mods;
    if (dragMode_ == DragMode::None) {
        lastPos_ = logicalPos;
        const int nextHover = (doc_ && effectiveTool(mods) == ToolKind::Select) ? doc_->topmostHitAt(logicalPos) : 0;
        if (nextHover != hoveredId_) {
            hoveredId_ = nextHover;
            Q_EMIT previewChanged();
        }
        Q_EMIT cursorChanged();
        return hoveredId_ > 0;
    }

    applyDrag(logicalPos, mods);
    lastPos_ = clampPoint(logicalPos);
    Q_EMIT previewChanged();
    return true;
}

bool AnnotationController::mouseRelease(const QPointF &logicalPos, Qt::MouseButton button,
                                        Qt::KeyboardModifiers mods)
{
    if (!doc_ || button != Qt::LeftButton || dragMode_ == DragMode::None) {
        return false;
    }
    applyDrag(logicalPos, mods);
    const DragMode mode = std::exchange(dragMode_, DragMode::None);
    spaceHeld_ = false;

    if (mode == DragMode::Create) {
        AnnotationItem finalized = std::move(draftItem_);
        hasDraft_ = false;
        draftItem_ = {};

        bool valid = true;
        switch (finalized.kind) {
        case ItemKind::Arrow:
            valid = std::hypot(finalized.end.x() - finalized.start.x(),
                               finalized.end.y() - finalized.start.y()) >= 4.0;
            break;
        case ItemKind::Rectangle:
        case ItemKind::Ellipse:
        case ItemKind::Spotlight:
        case ItemKind::Redact:
            valid = finalized.rect.width() >= 4.0 && finalized.rect.height() >= 4.0;
            break;
        case ItemKind::Pen:
        case ItemKind::Highlighter:
            if (finalized.points.size() > 2) {
                finalized.points = simplifyPolyline(finalized.points, 1.0);
            }
            valid = !finalized.points.isEmpty();
            break;
        case ItemKind::Step:
            valid = true;
            break;
        case ItemKind::Text:
            valid = false;
            break;
        }
        if (valid) {
            doc_->addItem(std::move(finalized));
        }
    } else if (mode == DragMode::Move || mode == DragMode::ResizeHandle) {
        if (const AnnotationItem *current = doc_->findItem(dragUndoBefore_.id)) {
            if (!itemsEquivalent(*current, dragUndoBefore_)) {
                AnnotationItem after = *current;
                doc_->setItemPreview(dragUndoBefore_);
                doc_->commitModification(dragUndoBefore_, std::move(after));
            }
        }
    } else if (mode == DragMode::DuplicateMove) {
        AnnotationItem copy = std::move(draftItem_);
        hasDraft_ = false;
        draftItem_ = {};
        const QPointF delta = logicalPos - anchor_;
        if (std::hypot(delta.x(), delta.y()) >= 2.0) {
            copy.id = 0;
            if (copy.kind == ItemKind::Redact) {
                copy.jitterSeed = QRandomGenerator::global()->generate();
            }
            doc_->addItem(std::move(copy));
        }
    }

    Q_EMIT previewChanged();
    Q_EMIT cursorChanged();
    return true;
}

bool AnnotationController::reapplyModifiers(QKeyEvent *event)
{
    const int key = event->key();
    if (key != Qt::Key_Shift && key != Qt::Key_Alt && key != Qt::Key_Control) {
        return false;
    }
    lastMods_ = event->modifiers();
    if (dragMode_ != DragMode::None) {
        applyDrag(lastPos_, lastMods_);
        Q_EMIT previewChanged();
    }
    Q_EMIT cursorChanged();
    return false;
}

void AnnotationController::stepActiveSize(int direction)
{
    const AnnotationItem *sel = doc_ ? doc_->selectedItem() : nullptr;
    const ItemKind targetKind = sel ? sel->kind : itemKindForTool(tool_);
    if (targetKind == ItemKind::Highlighter) {
        setHighlighterWidth(highlighterWidth_ + direction * 4.0);
        if (sel && sel->kind == ItemKind::Highlighter) {
            AnnotationItem after = *sel;
            after.strokeWidth = std::clamp(sel->strokeWidth + direction * 4.0, 8.0, 48.0);
            doc_->commitModification(*sel, after, /*mergeKey=*/2);
        }
    } else if (targetKind == ItemKind::Redact) {
        const int next = std::clamp(redactionStrength_ + direction * 2, 12, 48);
        setRedactionStrength(next);
        if (sel && sel->kind == ItemKind::Redact) {
            AnnotationItem after = *sel;
            after.redactionStrength = next;
            doc_->commitModification(*sel, after, /*mergeKey=*/2);
        }
    } else {
        setStrokeWidth(strokeWidth_ + direction * 1.0);
        if (sel && (sel->kind == ItemKind::Arrow || sel->kind == ItemKind::Rectangle
                    || sel->kind == ItemKind::Ellipse || sel->kind == ItemKind::Pen
                    || sel->kind == ItemKind::Step)) {
            AnnotationItem after = *sel;
            after.strokeWidth = std::clamp(sel->strokeWidth + direction * 1.0, 2.0, 16.0);
            doc_->commitModification(*sel, after, /*mergeKey=*/2);
        }
    }
}

bool AnnotationController::keyPress(QKeyEvent *event)
{
    if (!event) {
        return false;
    }
    reapplyModifiers(event);

    if (event->key() == Qt::Key_Space && dragMode_ != DragMode::None) {
        if (!event->isAutoRepeat()) {
            spaceHeld_ = true;
        }
        return true;
    }

    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool hasCtrl = mods.testFlag(Qt::ControlModifier);
    const bool hasShift = mods.testFlag(Qt::ShiftModifier);
    const bool hasAlt = mods.testFlag(Qt::AltModifier);

    if (hasCtrl && !hasAlt) {
        if (event->key() == Qt::Key_Z) {
            if (doc_) {
                if (hasShift && doc_->undoStack().canRedo()) {
                    doc_->undoStack().redo();
                } else if (!hasShift && doc_->undoStack().canUndo()) {
                    doc_->undoStack().undo();
                }
            }
            return true;
        }
        if (event->key() == Qt::Key_Y && !hasShift) {
            if (doc_ && doc_->undoStack().canRedo()) {
                doc_->undoStack().redo();
            }
            return true;
        }
        if (event->key() == Qt::Key_D && !hasShift) {
            if (doc_ && doc_->selectedId() > 0) {
                doc_->clearSelection();
                return true;
            }
            return false;
        }
    }

    if (event->key() == Qt::Key_Escape) {
        if (dragMode_ != DragMode::None) {
            cancelActiveGesture();
            return true;
        }
        if (doc_ && doc_->selectedId() > 0) {
            doc_->clearSelection();
            return true;
        }
        if (tool_ != ToolKind::Select) {
            setTool(ToolKind::Select);
            return true;
        }
        return false;
    }

    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && !hasCtrl && !hasAlt) {
        if (doc_ && doc_->selectedId() > 0) {
            doc_->removeItem(doc_->selectedId());
            return true;
        }
        return false;
    }

    if (!hasCtrl && !hasAlt && !mods.testFlag(Qt::MetaModifier)) {
        switch (event->key()) {
        case Qt::Key_BracketLeft:
            stepActiveSize(-1);
            return true;
        case Qt::Key_BracketRight:
            stepActiveSize(+1);
            return true;
        default:
            break;
        }
        if (!event->isAutoRepeat()) {
            switch (event->key()) {
            case Qt::Key_V:
                setTool(ToolKind::Select);
                return true;
            case Qt::Key_C:
                setTool(cropToolEnabled_ ? ToolKind::Crop : ToolKind::Select);
                return true;
            case Qt::Key_A:
                setTool(ToolKind::Arrow);
                return true;
            case Qt::Key_R:
                setTool(ToolKind::Rectangle);
                return true;
            case Qt::Key_E:
                setTool(ToolKind::Ellipse);
                return true;
            case Qt::Key_B:
                setTool(ToolKind::Pen);
                return true;
            case Qt::Key_M:
                setTool(ToolKind::Highlighter);
                return true;
            case Qt::Key_T:
                setTool(ToolKind::Text);
                return true;
            case Qt::Key_N:
                setTool(ToolKind::Step);
                return true;
            case Qt::Key_X:
                setTool(ToolKind::Redact);
                return true;
            case Qt::Key_S:
                setTool(ToolKind::Spotlight);
                return true;
            default:
                break;
            }
        }
    }

    if (doc_ && doc_->selectedId() > 0) {
        const qreal step = hasShift ? 10.0 : 1.0;
        QPointF delta;
        switch (event->key()) {
        case Qt::Key_Left:
            delta = {-step, 0.0};
            break;
        case Qt::Key_Right:
            delta = {step, 0.0};
            break;
        case Qt::Key_Up:
            delta = {0.0, -step};
            break;
        case Qt::Key_Down:
            delta = {0.0, step};
            break;
        default:
            return false;
        }
        if (const AnnotationItem *sel = doc_->selectedItem()) {
            AnnotationItem after = *sel;
            if (!hasAlt && !hasCtrl) {
                after.translate(delta);
            } else if (after.kind == ItemKind::Arrow || after.kind == ItemKind::Step) {
                if (after.kind == ItemKind::Step && !after.hasPointer) {
                    after.translate(delta);
                } else {
                    after.end += delta;
                }
            } else {
                const AnnotationHandle handle = std::abs(delta.x()) > 0.0
                    ? AnnotationHandle::Right
                    : AnnotationHandle::Bottom;
                const QPointF resizeDelta = std::abs(delta.x()) > 0.0
                    ? QPointF(delta.x(), 0.0)
                    : QPointF(0.0, delta.y());
                const DrawModifiers resizeMods{hasCtrl && hasAlt, hasAlt};
                after = applyHandleDrag(*sel, handle, resizeDelta, resizeMods);
            }
            doc_->commitModification(*sel, after, /*mergeKey=*/1);
            return true;
        }
    }

    return false;
}

bool AnnotationController::keyRelease(QKeyEvent *event)
{
    if (!event) {
        return false;
    }
    reapplyModifiers(event);
    if (event->key() == Qt::Key_Space && spaceHeld_) {
        if (!event->isAutoRepeat()) {
            spaceHeld_ = false;
        }
        return true;
    }
    return false;
}

void AnnotationController::cancelActiveGesture()
{
    if (dragMode_ == DragMode::None) {
        return;
    }
    const DragMode mode = std::exchange(dragMode_, DragMode::None);
    spaceHeld_ = false;
    hasDraft_ = false;
    draftItem_ = {};
    if ((mode == DragMode::Move || mode == DragMode::ResizeHandle) && doc_) {
        doc_->setItemPreview(dragUndoBefore_);
    }
    Q_EMIT previewChanged();
    Q_EMIT cursorChanged();
}

void AnnotationController::clearHover()
{
    if (hoveredId_ != 0) {
        hoveredId_ = 0;
        Q_EMIT previewChanged();
        Q_EMIT cursorChanged();
    }
}

} // namespace glimply
