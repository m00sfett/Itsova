#include "annotationcontroller.h"

#include "annotationgeometry.h"
#include "annotationtextlayout.h"

#include <QGuiApplication>
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
    if (editingText_) {
        requestCancelActiveText();
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
    if (editingText_ && tool != ToolKind::Text) {
        requestCommitActiveText(false);
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
        && tool_ != ToolKind::Crop && tool_ != ToolKind::Text) {
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

void AnnotationController::applySettings(const AnnotationSettings &settings)
{
    settings_ = settings;
    redactionStrength_ = settings_.redactionMode == RedactionMode::Blur ? settings_.blurRadius
                                                                        : settings_.pixelateBlockSize;
    Q_EMIT styleChanged();
}

void AnnotationController::writeSettings(AnnotationSettings &settings) const
{
    settings = settings_;
}

const AnnotationSettings &AnnotationController::settings() const
{
    return settings_;
}

QColor AnnotationController::strokeColor() const { return settings_.strokeColor; }
void AnnotationController::setStrokeColor(const QColor &color)
{
    QColor opaque = color;
    opaque.setAlpha(255);
    if (settings_.strokeColor != opaque) {
        settings_.strokeColor = opaque;
        Q_EMIT styleChanged();
    }
}

QColor AnnotationController::highlighterColor() const { return settings_.highlighterColor; }
void AnnotationController::setHighlighterColor(const QColor &color)
{
    QColor opaque = color;
    opaque.setAlpha(255);
    if (settings_.highlighterColor != opaque) {
        settings_.highlighterColor = opaque;
        Q_EMIT styleChanged();
    }
}

qreal AnnotationController::strokeWidth() const { return settings_.strokeWidth; }
void AnnotationController::setStrokeWidth(qreal width)
{
    const qreal clamped = std::clamp(width, 2.0, 16.0);
    if (!qFuzzyCompare(settings_.strokeWidth, clamped)) {
        settings_.strokeWidth = clamped;
        Q_EMIT styleChanged();
    }
}

qreal AnnotationController::highlighterWidth() const { return settings_.highlighterWidth; }
void AnnotationController::setHighlighterWidth(qreal width)
{
    const qreal clamped = std::clamp(width, 8.0, 48.0);
    if (!qFuzzyCompare(settings_.highlighterWidth, clamped)) {
        settings_.highlighterWidth = clamped;
        Q_EMIT styleChanged();
    }
}

ShapeFillMode AnnotationController::fillMode() const { return settings_.shapeFillMode; }
void AnnotationController::setFillMode(ShapeFillMode mode)
{
    if (settings_.shapeFillMode != mode) {
        settings_.shapeFillMode = mode;
        Q_EMIT styleChanged();
    }
}

qreal AnnotationController::fillOpacity() const { return settings_.shapeFillOpacity; }
void AnnotationController::setFillOpacity(qreal opacity)
{
    const qreal clamped = std::clamp(opacity, 0.10, 1.0);
    if (!qFuzzyCompare(settings_.shapeFillOpacity, clamped)) {
        settings_.shapeFillOpacity = clamped;
        Q_EMIT styleChanged();
    }
}

SpotlightShape AnnotationController::spotlightShape() const { return settings_.spotlightShape; }
void AnnotationController::setSpotlightShape(SpotlightShape shape)
{
    if (settings_.spotlightShape != shape) {
        settings_.spotlightShape = shape;
        Q_EMIT styleChanged();
    }
}

qreal AnnotationController::dimOpacity() const { return settings_.spotlightDimOpacity; }
void AnnotationController::setDimOpacity(qreal opacity)
{
    const qreal clamped = std::clamp(opacity, 0.35, 0.85);
    if (!qFuzzyCompare(settings_.spotlightDimOpacity, clamped)) {
        settings_.spotlightDimOpacity = clamped;
        Q_EMIT styleChanged();
    }
}

RedactionMode AnnotationController::redactionMode() const { return settings_.redactionMode; }
void AnnotationController::setRedactionMode(RedactionMode mode)
{
    if (settings_.redactionMode != mode) {
        settings_.redactionMode = mode;
        redactionStrength_ = mode == RedactionMode::Blur ? settings_.blurRadius : settings_.pixelateBlockSize;
        Q_EMIT styleChanged();
    }
}

QColor AnnotationController::redactionColor() const { return settings_.redactionColor; }
void AnnotationController::setRedactionColor(const QColor &color)
{
    QColor opaque = color;
    opaque.setAlpha(255);
    if (settings_.redactionColor != opaque) {
        settings_.redactionColor = opaque;
        Q_EMIT styleChanged();
    }
}

int AnnotationController::redactionStrength() const { return redactionStrength_; }
void AnnotationController::setRedactionStrength(int strength)
{
    if (settings_.redactionMode == RedactionMode::Blur) {
        strength = std::clamp(strength, 14, 48);
        settings_.blurRadius = strength;
    } else {
        strength = std::clamp(strength, 12, 40);
        settings_.pixelateBlockSize = strength;
    }
    if (redactionStrength_ != strength) {
        redactionStrength_ = strength;
        Q_EMIT styleChanged();
    }
}

QString AnnotationController::fontFamily() const { return settings_.fontFamily; }
void AnnotationController::setFontFamily(const QString &family)
{
    if (settings_.fontFamily != family) {
        settings_.fontFamily = family;
        if (editingText_) {
            textCurrent_.font.setFamily(family);
            Q_EMIT textEditStyleUpdated(textCurrent_);
        }
        Q_EMIT styleChanged();
    }
}

int AnnotationController::fontPointSize() const { return settings_.fontPointSize; }
void AnnotationController::setFontPointSize(int pt)
{
    const int clamped = std::clamp(pt, 10, 72);
    if (settings_.fontPointSize != clamped) {
        settings_.fontPointSize = clamped;
        if (editingText_) {
            textCurrent_.font.setPointSize(clamped);
            Q_EMIT textEditStyleUpdated(textCurrent_);
        }
        Q_EMIT styleChanged();
    }
}

bool AnnotationController::fontBold() const { return settings_.fontBold; }
void AnnotationController::setFontBold(bool on)
{
    if (settings_.fontBold != on) {
        settings_.fontBold = on;
        if (editingText_) {
            textCurrent_.font.setBold(on);
            Q_EMIT textEditStyleUpdated(textCurrent_);
        }
        Q_EMIT styleChanged();
    }
}

bool AnnotationController::fontItalic() const { return settings_.fontItalic; }
void AnnotationController::setFontItalic(bool on)
{
    if (settings_.fontItalic != on) {
        settings_.fontItalic = on;
        if (editingText_) {
            textCurrent_.font.setItalic(on);
            Q_EMIT textEditStyleUpdated(textCurrent_);
        }
        Q_EMIT styleChanged();
    }
}

Qt::Alignment AnnotationController::textAlignment() const { return settings_.textAlignment; }
void AnnotationController::setTextAlignment(Qt::Alignment alignment)
{
    if (settings_.textAlignment != alignment) {
        settings_.textAlignment = alignment;
        if (editingText_) {
            textCurrent_.textAlignment = alignment;
            Q_EMIT textEditStyleUpdated(textCurrent_);
        }
        Q_EMIT styleChanged();
    }
}

TextLegibility AnnotationController::textLegibility() const { return settings_.textLegibility; }
void AnnotationController::setTextLegibility(TextLegibility legibility)
{
    if (settings_.textLegibility != legibility) {
        settings_.textLegibility = legibility;
        if (editingText_) {
            textCurrent_.legibility = legibility;
            Q_EMIT textEditStyleUpdated(textCurrent_);
        }
        Q_EMIT styleChanged();
    }
}

QColor AnnotationController::textColor() const { return settings_.textColor; }
void AnnotationController::setTextColor(const QColor &color)
{
    QColor opaque = color;
    opaque.setAlpha(255);
    if (settings_.textColor != opaque) {
        settings_.textColor = opaque;
        if (editingText_) {
            textCurrent_.color = opaque;
            Q_EMIT textEditStyleUpdated(textCurrent_);
        }
        Q_EMIT styleChanged();
    }
}

bool AnnotationController::isEditingText() const { return editingText_; }
int AnnotationController::editingTextItemId() const { return (editingText_ && editingExistingText_) ? textBefore_.id : 0; }
const AnnotationItem *AnnotationController::activeTextDraft() const { return editingText_ ? &textCurrent_ : nullptr; }

AnnotationItem AnnotationController::buildTextItemTemplate(const QPointF &logicalTopLeft, qreal paragraphWidth) const
{
    AnnotationItem item;
    item.kind = ItemKind::Text;
    item.paragraphMode = paragraphWidth >= 60.0;
    item.rect = QRectF(logicalTopLeft, QSizeF(item.paragraphMode ? paragraphWidth : 80.0, 28.0));
    item.color = settings_.textColor;
    QFont font = settings_.fontFamily.isEmpty() && QGuiApplication::instance()
        ? QGuiApplication::font()
        : QFont(settings_.fontFamily);
    font.setPointSize(settings_.fontPointSize);
    font.setBold(settings_.fontBold);
    font.setItalic(settings_.fontItalic);
    item.font = font;
    item.textAlignment = settings_.textAlignment;
    item.legibility = settings_.textLegibility;
    return item;
}

void AnnotationController::beginTextEdit(const AnnotationItem &item, bool isExisting)
{
    if (editingText_) {
        commitTextEdit(false);
    }
    editingText_ = true;
    editingExistingText_ = isExisting;
    textBefore_ = item;
    textCurrent_ = item;
    if (doc_ && isExisting) {
        doc_->select(item.id);
    } else if (doc_) {
        doc_->clearSelection();
    }
    Q_EMIT textEditStarted(textCurrent_, editingExistingText_);
    Q_EMIT previewChanged();
    Q_EMIT styleChanged();
}

void AnnotationController::updateActiveTextDraft(const AnnotationItem &item)
{
    if (!editingText_) {
        return;
    }
    textCurrent_ = item;
    settings_.fontFamily = item.font.family();
    if (item.font.pointSize() > 0) {
        settings_.fontPointSize = std::clamp(item.font.pointSize(), 10, 72);
    }
    settings_.fontBold = item.font.bold();
    settings_.fontItalic = item.font.italic();
    settings_.textAlignment = item.textAlignment;
    settings_.textLegibility = item.legibility;
    settings_.textColor = item.color;
    Q_EMIT textEditStyleUpdated(textCurrent_);
    Q_EMIT styleChanged();
}

void AnnotationController::commitTextEdit(bool switchToSelect)
{
    if (!editingText_) {
        return;
    }
    editingText_ = false;
    AnnotationItem finalized = textCurrent_;
    const bool existing = editingExistingText_;
    editingExistingText_ = false;

    if (doc_) {
        if (finalized.text.trimmed().isEmpty()) {
            if (existing) {
                doc_->removeItem(textBefore_.id);
            }
        } else {
            finalized.rect.setSize(measureAnnotationText(finalized));
            if (existing) {
                if (!itemsEquivalent(textBefore_, finalized)) {
                    doc_->commitModification(textBefore_, finalized);
                }
                doc_->select(textBefore_.id);
            } else {
                finalized.id = 0;
                doc_->addItem(std::move(finalized));
            }
        }
    }

    if (switchToSelect && tool_ != ToolKind::Select) {
        tool_ = ToolKind::Select;
        Q_EMIT toolChanged(tool_);
    }
    Q_EMIT textEditFinished();
    Q_EMIT previewChanged();
    Q_EMIT styleChanged();
    Q_EMIT cursorChanged();
}

void AnnotationController::cancelTextEdit()
{
    if (!editingText_) {
        return;
    }
    editingText_ = false;
    editingExistingText_ = false;
    Q_EMIT textEditFinished();
    Q_EMIT previewChanged();
    Q_EMIT styleChanged();
    Q_EMIT cursorChanged();
}

void AnnotationController::requestCommitActiveText(bool switchToSelect)
{
    if (editingText_) {
        Q_EMIT textEditCommitRequested(switchToSelect);
    }
}

void AnnotationController::requestCancelActiveText()
{
    if (editingText_) {
        Q_EMIT textEditCancelRequested();
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
        if (editingText_) {
            requestCommitActiveText(false);
        }
        const int hitId = doc_->topmostHitAt(logicalPos);
        if (const AnnotationItem *hit = doc_->findItem(hitId); hit && hit->kind == ItemKind::Text) {
            beginTextEdit(*hit, true);
            return true;
        }
        doc_->clearSelection();
        const QPointF startPos = clampPoint(logicalPos);
        anchor_ = startPos;
        lastPos_ = startPos;
        dragMode_ = DragMode::CreateTextBox;
        hasDraft_ = true;
        draftItem_ = AnnotationItem{};
        draftItem_.kind = ItemKind::Text;
        draftItem_.rect = QRectF(startPos, QSizeF(0.0, 0.0));
        Q_EMIT previewChanged();
        Q_EMIT cursorChanged();
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
        draftItem_.color = settings_.strokeColor;
        draftItem_.strokeWidth = settings_.strokeWidth;
        break;
    case ToolKind::Rectangle:
    case ToolKind::Ellipse:
        draftItem_.rect = QRectF(startPos, QSizeF(0.0, 0.0));
        draftItem_.color = settings_.strokeColor;
        draftItem_.strokeWidth = settings_.strokeWidth;
        draftItem_.fillMode = settings_.shapeFillMode;
        draftItem_.fillOpacity = settings_.shapeFillOpacity;
        break;
    case ToolKind::Pen:
        draftItem_.points = {startPos};
        draftItem_.color = settings_.strokeColor;
        draftItem_.strokeWidth = settings_.strokeWidth;
        break;
    case ToolKind::Highlighter:
        draftItem_.points = {startPos};
        draftItem_.color = settings_.highlighterColor;
        draftItem_.strokeWidth = settings_.highlighterWidth;
        break;
    case ToolKind::Step:
        draftItem_.start = startPos;
        draftItem_.end = startPos;
        draftItem_.hasPointer = false;
        draftItem_.stepNumber = doc_->nextStepNumber();
        draftItem_.color = settings_.strokeColor;
        draftItem_.strokeWidth = settings_.strokeWidth;
        break;
    case ToolKind::Spotlight:
        draftItem_.rect = QRectF(startPos, QSizeF(0.0, 0.0));
        draftItem_.spotlightShape = settings_.spotlightShape;
        draftItem_.dimOpacity = settings_.spotlightDimOpacity;
        break;
    case ToolKind::Redact:
        draftItem_.rect = QRectF(startPos, QSizeF(0.0, 0.0));
        draftItem_.redactionMode = settings_.redactionMode;
        draftItem_.color = settings_.redactionMode == RedactionMode::Solid ? settings_.redactionColor
                                                                           : QColor(0, 0, 0);
        draftItem_.color.setAlpha(255);
        draftItem_.redactionStrength = settings_.redactionMode == RedactionMode::Blur ? settings_.blurRadius
                                                                                      : settings_.pixelateBlockSize;
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

    if (dragMode_ == DragMode::CreateTextBox) {
        const QPointF cur = clampPoint(logicalPos);
        draftItem_.rect = constrainedBox(anchor_, cur, DrawModifiers{false, drawMods.fromCenter}, bounds_);
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

    if (mode == DragMode::CreateTextBox) {
        const QRectF box = draftItem_.rect.normalized();
        hasDraft_ = false;
        draftItem_ = {};
        const qreal dragDist = std::hypot(logicalPos.x() - anchor_.x(), logicalPos.y() - anchor_.y());
        const bool isParagraph = dragDist >= 8.0 && box.width() >= 12.0;
        const AnnotationItem tpl = buildTextItemTemplate(isParagraph ? box.topLeft() : anchor_,
                                                           isParagraph ? std::max(60.0, box.width()) : 0.0);
        beginTextEdit(tpl, false);
        return true;
    }

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

bool AnnotationController::mouseDoubleClick(const QPointF &logicalPos, Qt::MouseButton button,
                                            Qt::KeyboardModifiers mods)
{
    if (!doc_ || button != Qt::LeftButton || effectiveTool(mods) != ToolKind::Select) {
        return false;
    }
    const int hitId = doc_->topmostHitAt(logicalPos);
    if (const AnnotationItem *hit = doc_->findItem(hitId); hit && hit->kind == ItemKind::Text) {
        beginTextEdit(*hit, true);
        return true;
    }
    return false;
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
    if (targetKind == ItemKind::Text) {
        const int basePt = sel && sel->font.pointSize() > 0 ? sel->font.pointSize() : settings_.fontPointSize;
        const int nextPt = std::clamp(basePt + direction * 2, 10, 72);
        setFontPointSize(nextPt);
        if (sel && sel->kind == ItemKind::Text) {
            AnnotationItem after = *sel;
            after.font.setPointSize(nextPt);
            after.rect.setSize(measureAnnotationText(after));
            doc_->commitModification(*sel, after, /*mergeKey=*/2);
        }
    } else if (targetKind == ItemKind::Highlighter) {
        const qreal next = std::clamp((sel ? sel->strokeWidth : settings_.highlighterWidth) + direction * 4.0,
                                      8.0, 48.0);
        setHighlighterWidth(next);
        if (sel && sel->kind == ItemKind::Highlighter) {
            AnnotationItem after = *sel;
            after.strokeWidth = next;
            doc_->commitModification(*sel, after, /*mergeKey=*/2);
        }
    } else if (targetKind == ItemKind::Redact) {
        const RedactionMode mode = sel ? sel->redactionMode : settings_.redactionMode;
        if (mode != RedactionMode::Solid) {
            const int minV = mode == RedactionMode::Blur ? 14 : 12;
            const int maxV = mode == RedactionMode::Blur ? 48 : 40;
            const int curV = sel ? sel->redactionStrength
                                 : (mode == RedactionMode::Blur ? settings_.blurRadius : settings_.pixelateBlockSize);
            const int next = std::clamp(curV + direction * 2, minV, maxV);
            if (mode == RedactionMode::Blur) {
                settings_.blurRadius = next;
            } else {
                settings_.pixelateBlockSize = next;
            }
            if (mode == settings_.redactionMode) {
                redactionStrength_ = next;
            }
            Q_EMIT styleChanged();
            if (sel && sel->kind == ItemKind::Redact) {
                AnnotationItem after = *sel;
                after.redactionStrength = next;
                doc_->commitModification(*sel, after, /*mergeKey=*/2);
            }
        }
    } else {
        const qreal next = std::clamp((sel ? sel->strokeWidth : settings_.strokeWidth) + direction * 1.0,
                                      2.0, 16.0);
        setStrokeWidth(next);
        if (sel && (sel->kind == ItemKind::Arrow || sel->kind == ItemKind::Rectangle
                    || sel->kind == ItemKind::Ellipse || sel->kind == ItemKind::Pen
                    || sel->kind == ItemKind::Step)) {
            AnnotationItem after = *sel;
            after.strokeWidth = next;
            doc_->commitModification(*sel, after, /*mergeKey=*/2);
        }
    }
}

bool AnnotationController::keyPress(QKeyEvent *event)
{
    if (!event) {
        return false;
    }
    if (editingText_) {
        return true;
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
    if (editingText_) {
        return true;
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
