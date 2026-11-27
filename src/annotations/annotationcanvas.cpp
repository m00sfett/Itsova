#include "annotationcanvas.h"

#include "annotationrenderer.h"
#include "inlinetexteditor.h"
#include "imageoutput.h"
#include "pixelloupe.h"
#include "selectiongeometry.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QIcon>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QResizeEvent>
#include <QUndoCommand>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace itsova
{

class CropImageCommand : public QUndoCommand
{
public:
    CropImageCommand(AnnotationCanvas *canvas, AnnotationDocument *doc, QImage beforeImage, qreal scale,
                     QImage afterImage, QPoint cropTopLeft)
        : QUndoCommand(QCoreApplication::translate("AnnotationCanvas", "Crop image"))
        , canvas_(canvas)
        , doc_(doc)
        , beforeImage_(std::move(beforeImage))
        , scale_(scale)
        , afterImage_(std::move(afterImage))
        , offset_(QPointF(cropTopLeft))
    {
    }

    void undo() override
    {
        if (canvas_) {
            canvas_->setCroppedImageInternal(beforeImage_, scale_);
        }
        if (doc_) {
            doc_->translateAll(offset_);
        }
    }

    void redo() override
    {
        if (canvas_) {
            canvas_->setCroppedImageInternal(afterImage_, scale_);
        }
        if (doc_) {
            doc_->translateAll(-offset_);
        }
    }

private:
    QPointer<AnnotationCanvas> canvas_;
    QPointer<AnnotationDocument> doc_;
    QImage beforeImage_;
    qreal scale_ = 1.0;
    QImage afterImage_;
    QPointF offset_;
};

namespace
{

constexpr int minimumCropSize = 4;
constexpr qreal workspaceMargin = 24.0;
constexpr qreal cropHandleRadius = 5.0;

QRect clampCropRect(QRect rect, const QSize &bounds)
{
    rect = rect.normalized();
    if (bounds.isEmpty()) {
        return {};
    }
    rect.setWidth(std::min(rect.width(), bounds.width()));
    rect.setHeight(std::min(rect.height(), bounds.height()));
    rect.moveLeft(std::clamp(rect.left(), 0, bounds.width() - rect.width()));
    rect.moveTop(std::clamp(rect.top(), 0, bounds.height() - rect.height()));
    return rect;
}

} // namespace

AnnotationCanvas::AnnotationCanvas(QWidget *parent)
    : QWidget(parent)
    , ownedDoc_(new AnnotationDocument(this))
    , doc_(ownedDoc_)
    , controller_(new AnnotationController(doc_, this))
    , textEditor_(new InlineTextEditor(this))
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setProperty("_kde_no_window_grab", true);
    connect(doc_, &AnnotationDocument::changed, this, qOverload<>(&QWidget::update));
    connect(controller_, &AnnotationController::previewChanged, this, qOverload<>(&QWidget::update));
    connect(controller_, &AnnotationController::cursorChanged, this, &AnnotationCanvas::refreshCursor);
    connect(controller_, &AnnotationController::toolChanged, this, [this](ToolKind tool) {
        if (tool != ToolKind::Crop && !cropRect_.isEmpty()) {
            cropRect_ = QRect();
            cropDrag_ = CropDrag::None;
            cropSpaceHeld_ = false;
            Q_EMIT cropRectChanged(QRect());
            update();
        }
    });

    connect(controller_, &AnnotationController::textEditStarted, this,
            [this](const AnnotationItem &item, bool isExisting) {
                textEditor_->startEditing(item, isExisting, zoom_, panOffset_);
            });
    connect(controller_, &AnnotationController::textEditStyleUpdated, textEditor_, &InlineTextEditor::updateStyle);
    connect(controller_, &AnnotationController::textEditCommitRequested, textEditor_, &InlineTextEditor::commit);
    connect(controller_, &AnnotationController::textEditCancelRequested, textEditor_, &InlineTextEditor::cancel);

    connect(textEditor_, &InlineTextEditor::draftChanged, controller_, &AnnotationController::updateActiveTextDraft);
    connect(textEditor_, &InlineTextEditor::styleToggled, this, [this](const AnnotationItem &item) {
        controller_->updateActiveTextDraft(item);
    });
    connect(textEditor_, &InlineTextEditor::committed, this,
            [this](const AnnotationItem &item, bool, bool switchToSelect) {
                controller_->updateActiveTextDraft(item);
                controller_->commitTextEdit(switchToSelect);
                setFocus(Qt::OtherFocusReason);
            });
    connect(textEditor_, &InlineTextEditor::cancelled, this, [this](int) {
        controller_->cancelTextEdit();
        setFocus(Qt::OtherFocusReason);
    });
}

void AnnotationCanvas::setImage(const QImage &image, qreal scale)
{
    image_ = image;
    imageScale_ = scale > 0.0 ? scale : 1.0;
    const QSizeF logical = logicalImageSize();
    if (logical.isValid() && !logical.isEmpty()) {
        controller_->setBounds(QRectF(QPointF(0.0, 0.0), logical));
    } else {
        controller_->setBounds({});
    }
    cropRect_ = QRect();
    cropDrag_ = CropDrag::None;
    if (editorMode_) {
        pendingInitialView_ = true;
        applyInitialEditorView();
    }
    Q_EMIT cropRectChanged(QRect());
    Q_EMIT imageDimensionsChanged();
    updateGeometry();
    update();
}

void AnnotationCanvas::setCroppedImageInternal(const QImage &image, qreal scale)
{
    image_ = image;
    imageScale_ = scale > 0.0 ? scale : 1.0;
    const QSizeF logical = logicalImageSize();
    controller_->setBounds(logical.isValid() && !logical.isEmpty() ? QRectF(QPointF(0.0, 0.0), logical) : QRectF());
    cropRect_ = QRect();
    cropDrag_ = CropDrag::None;
    cropSpaceHeld_ = false;
    clampAndSyncPan();
    Q_EMIT cropRectChanged(QRect());
    Q_EMIT imageDimensionsChanged();
    Q_EMIT viewChanged();
    update();
}

const QImage &AnnotationCanvas::image() const
{
    return image_;
}

qreal AnnotationCanvas::imageScale() const
{
    return imageScale_;
}

QSizeF AnnotationCanvas::logicalImageSize() const
{
    if (image_.isNull()) {
        return {};
    }
    return QSizeF(image_.width() / imageScale_, image_.height() / imageScale_);
}

void AnnotationCanvas::setEditorMode(bool enabled)
{
    if (editorMode_ == enabled) {
        return;
    }
    editorMode_ = enabled;
    controller_->setCropToolEnabled(enabled);
    if (editorMode_ && !image_.isNull()) {
        pendingInitialView_ = true;
        applyInitialEditorView();
    }
    update();
}

bool AnnotationCanvas::editorMode() const
{
    return editorMode_;
}

void AnnotationCanvas::setDocument(AnnotationDocument *document)
{
    AnnotationDocument *next = document ? document : ownedDoc_;
    if (doc_ == next) {
        return;
    }
    if (controller_->isEditingText()) {
        textEditor_->cancel();
    }
    disconnect(doc_, &AnnotationDocument::changed, this, qOverload<>(&QWidget::update));
    doc_ = next;
    connect(doc_, &AnnotationDocument::changed, this, qOverload<>(&QWidget::update));
    controller_->setDocument(doc_);
    update();
}

AnnotationDocument *AnnotationCanvas::document() const
{
    return doc_;
}

AnnotationController *AnnotationCanvas::controller() const
{
    return controller_;
}

InlineTextEditor *AnnotationCanvas::textEditor() const
{
    return textEditor_;
}

void AnnotationCanvas::setColorPickerActive(bool active)
{
    if (active && image_.isNull()) {
        return;
    }
    if (colorPickerActive_ == active) {
        return;
    }
    if (active) {
        if (controller_->isEditingText()) {
            controller_->requestCommitActiveText(false);
        }
        controller_->cancelActiveGesture();
        panning_ = false;
        if (!rect().contains(lastWidgetPos_.toPoint())) {
            const QPoint mapped = mapFromGlobal(QCursor::pos());
            if (rect().contains(mapped)) {
                lastWidgetPos_ = mapped;
            }
        }
    }
    colorPickerActive_ = active;
    refreshCursor();
    update();
    Q_EMIT colorPickerActiveChanged(active);
}

bool AnnotationCanvas::isColorPickerActive() const
{
    return colorPickerActive_;
}

void AnnotationCanvas::setColorFormat(ColorFormat format)
{
    if (colorFormat_ != format) {
        colorFormat_ = format;
        if (colorPickerActive_) {
            update();
        }
    }
}

ColorFormat AnnotationCanvas::colorFormat() const
{
    return colorFormat_;
}

bool AnnotationCanvas::focusNextPrevChild(bool next)
{
    if (colorPickerActive_) {
        return false;
    }
    return QWidget::focusNextPrevChild(next);
}

void AnnotationCanvas::applyInitialEditorView()
{
    const QSizeF logical = logicalImageSize();
    if (!editorMode_ || logical.isEmpty() || width() <= 64 || height() <= 64) {
        return;
    }
    pendingInitialView_ = false;
    const qreal availW = std::max(1.0, width() - 2.0 * workspaceMargin);
    const qreal availH = std::max(1.0, height() - 2.0 * workspaceMargin);
    const qreal fitScale = std::min(availW / logical.width(), availH / logical.height());
    if (fitScale >= 1.0) {
        autoFit_ = false;
        zoom_ = 1.0;
    } else {
        autoFit_ = true;
        zoom_ = std::clamp(fitScale, 0.1, 16.0);
    }
    panOffset_ = QPointF((width() - logical.width() * zoom_) * 0.5,
                         (height() - logical.height() * zoom_) * 0.5);
    textEditor_->setViewTransform(zoom_, panOffset_);
    Q_EMIT viewChanged();
}

void AnnotationCanvas::fitInView()
{
    const QSizeF logical = logicalImageSize();
    if (logical.isEmpty() || width() <= 32 || height() <= 32) {
        return;
    }
    autoFit_ = true;
    const qreal availW = std::max(1.0, width() - 2.0 * workspaceMargin);
    const qreal availH = std::max(1.0, height() - 2.0 * workspaceMargin);
    zoom_ = std::clamp(std::min(availW / logical.width(), availH / logical.height()), 0.1, 16.0);
    panOffset_ = QPointF((width() - logical.width() * zoom_) * 0.5,
                         (height() - logical.height() * zoom_) * 0.5);
    textEditor_->setViewTransform(zoom_, panOffset_);
    refreshCursor();
    Q_EMIT viewChanged();
    update();
}

void AnnotationCanvas::resetZoom100()
{
    autoFit_ = false;
    setZoomAnchored(1.0, rect().center());
}

void AnnotationCanvas::zoomByStep(int steps, const QPointF &widgetAnchor)
{
    if (steps == 0) {
        return;
    }
    const QPointF anchor = widgetAnchor.isNull() ? rect().center() : widgetAnchor;
    const qreal factor = std::pow(1.2, steps);
    setZoomAnchored(zoom_ * factor, anchor);
}

void AnnotationCanvas::setZoomAnchored(qreal zoom, const QPointF &widgetAnchor)
{
    const qreal clamped = std::clamp(zoom, 0.1, 16.0);
    autoFit_ = false;
    const QPointF anchor = widgetAnchor.isNull() ? rect().center() : widgetAnchor;
    const QPointF logicalAtAnchor = widgetToLogical(anchor);
    zoom_ = clamped;
    panOffset_ = anchor - logicalAtAnchor * zoom_;
    clampAndSyncPan();
    textEditor_->setViewTransform(zoom_, panOffset_);
    refreshCursor();
    Q_EMIT viewChanged();
    update();
}

void AnnotationCanvas::setZoom(qreal zoom)
{
    const qreal clamped = std::clamp(zoom, 0.1, 16.0);
    autoFit_ = false;
    if (!qFuzzyCompare(zoom_, clamped)) {
        zoom_ = clamped;
        clampAndSyncPan();
        textEditor_->setViewTransform(zoom_, panOffset_);
        refreshCursor();
        Q_EMIT viewChanged();
        update();
    }
}

qreal AnnotationCanvas::zoom() const
{
    return zoom_;
}

bool AnnotationCanvas::isAutoFit() const
{
    return autoFit_;
}

void AnnotationCanvas::clampAndSyncPan()
{
    if (!editorMode_ || image_.isNull()) {
        return;
    }
    const QSizeF logical = logicalImageSize();
    const qreal scaledW = logical.width() * zoom_;
    const qreal scaledH = logical.height() * zoom_;
    if (scaledW + 2.0 * workspaceMargin <= width()) {
        panOffset_.setX((width() - scaledW) * 0.5);
    } else {
        const qreal minX = width() - scaledW - workspaceMargin;
        const qreal maxX = workspaceMargin;
        panOffset_.setX(std::clamp(panOffset_.x(), minX, maxX));
    }
    if (scaledH + 2.0 * workspaceMargin <= height()) {
        panOffset_.setY((height() - scaledH) * 0.5);
    } else {
        const qreal minY = height() - scaledH - workspaceMargin;
        const qreal maxY = workspaceMargin;
        panOffset_.setY(std::clamp(panOffset_.y(), minY, maxY));
    }
}

void AnnotationCanvas::setPanOffset(const QPointF &offset)
{
    QPointF target = offset;
    if (editorMode_ && !image_.isNull()) {
        panOffset_ = target;
        clampAndSyncPan();
        target = panOffset_;
    } else {
        panOffset_ = target;
    }
    textEditor_->setViewTransform(zoom_, panOffset_);
    Q_EMIT viewChanged();
    update();
}

QPointF AnnotationCanvas::panOffset() const
{
    return panOffset_;
}

QPointF AnnotationCanvas::widgetToLogical(const QPointF &widgetPos) const
{
    return (widgetPos - panOffset_) / zoom_;
}

QPointF AnnotationCanvas::logicalToWidget(const QPointF &logicalPos) const
{
    return logicalPos * zoom_ + panOffset_;
}

void AnnotationCanvas::setCropRect(const QRect &logicalRect)
{
    const QSize logicalBounds = logicalImageSize().toSize();
    const QRect clamped = logicalRect.isValid() && !logicalBounds.isEmpty() ? clampCropRect(logicalRect, logicalBounds)
                                                                            : QRect();
    if (cropRect_ != clamped) {
        cropRect_ = clamped;
        Q_EMIT cropRectChanged(cropRect_);
        update();
    }
}

QRect AnnotationCanvas::cropRect() const
{
    return cropRect_;
}

bool AnnotationCanvas::hasCropRect() const
{
    return cropRect_.isValid() && cropRect_.width() >= minimumCropSize && cropRect_.height() >= minimumCropSize;
}

bool AnnotationCanvas::applyActiveCrop()
{
    if (!hasCropRect() || image_.isNull()) {
        return false;
    }
    QString error;
    const QImage cropped = cropLogicalArea(image_, cropRect_, imageScale_, &error);
    if (cropped.isNull()) {
        return false;
    }
    doc_->undoStack().push(new CropImageCommand(this, doc_, image_, imageScale_, cropped, cropRect_.topLeft()));
    controller_->setTool(ToolKind::Select);
    return true;
}

void AnnotationCanvas::cancelActiveCrop()
{
    if (!cropRect_.isEmpty()) {
        cropRect_ = QRect();
        Q_EMIT cropRectChanged(QRect());
    }
    cropDrag_ = CropDrag::None;
    cropSpaceHeld_ = false;
    if (controller_->tool() == ToolKind::Crop) {
        controller_->setTool(ToolKind::Select);
    }
    update();
}

QImage AnnotationCanvas::renderExport() const
{
    const QSizeF logical = logicalImageSize();
    if (image_.isNull() || logical.isEmpty()) {
        return {};
    }
    return renderAnnotatedCrop(image_, QRectF(QPointF(0.0, 0.0), logical), imageScale_, *doc_);
}

Qt::Edges AnnotationCanvas::cropEdgesAt(const QPoint &logicalPos) const
{
    if (!cropRect_.isValid()) {
        return {};
    }
    const int grab = std::max(4, int(std::lround(8.0 / zoom_)));
    const int corner = grab + 4;
    const int left = cropRect_.left();
    const int right = cropRect_.left() + cropRect_.width();
    const int top = cropRect_.top();
    const int bottom = cropRect_.top() + cropRect_.height();
    const int x = logicalPos.x();
    const int y = logicalPos.y();
    const bool nearLeft = std::abs(x - left) <= corner;
    const bool nearRight = std::abs(x - right) <= corner;
    const bool nearTop = std::abs(y - top) <= corner;
    const bool nearBottom = std::abs(y - bottom) <= corner;
    if ((nearLeft || nearRight) && (nearTop || nearBottom)) {
        return Qt::Edges(std::abs(x - left) <= std::abs(x - right) ? Qt::LeftEdge : Qt::RightEdge)
             | (std::abs(y - top) <= std::abs(y - bottom) ? Qt::TopEdge : Qt::BottomEdge);
    }
    const bool withinY = y >= top - grab && y <= bottom + grab;
    const bool withinX = x >= left - grab && x <= right + grab;
    if (withinY && std::abs(x - left) <= grab) {
        return Qt::LeftEdge;
    }
    if (withinY && std::abs(x - right) <= grab) {
        return Qt::RightEdge;
    }
    if (withinX && std::abs(y - top) <= grab) {
        return Qt::TopEdge;
    }
    if (withinX && std::abs(y - bottom) <= grab) {
        return Qt::BottomEdge;
    }
    return {};
}

void AnnotationCanvas::applyCropDrag(const QPoint &logicalPos, Qt::KeyboardModifiers mods)
{
    const QSize logicalBounds = logicalImageSize().toSize();
    if (logicalBounds.isEmpty()) {
        return;
    }
    const SelectionConstraints constraints{mods.testFlag(Qt::AltModifier), mods.testFlag(Qt::ShiftModifier)};
    if (cropDrag_ == CropDrag::Create) {
        cropRect_ = drawnSelection(cropAnchorLogical_, logicalPos, constraints, logicalBounds);
    } else if (cropDrag_ == CropDrag::Move) {
        cropRect_ = clampCropRect(cropStartRect_.translated(logicalPos - cropAnchorLogical_), logicalBounds);
    } else if (cropDrag_ == CropDrag::Resize) {
        cropRect_ = resizedSelection(cropStartRect_, cropEdges_, logicalPos - cropAnchorLogical_, constraints,
                                     logicalBounds);
    } else {
        return;
    }
    Q_EMIT cropRectChanged(cropRect_);
    update();
}

void AnnotationCanvas::refreshCursor()
{
    if (colorPickerActive_) {
        setCursor(Qt::CrossCursor);
        return;
    }
    if (panning_) {
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (editorMode_ && spacePanKeyHeld_ && !controller_->isDragging() && cropDrag_ == CropDrag::None) {
        setCursor(Qt::OpenHandCursor);
        return;
    }
    if (editorMode_ && controller_->tool() == ToolKind::Crop) {
        const QPoint logical = widgetToLogical(lastWidgetPos_).toPoint();
        const Qt::Edges edges = cropEdgesAt(logical);
        if (edges == (Qt::LeftEdge | Qt::TopEdge) || edges == (Qt::RightEdge | Qt::BottomEdge)) {
            setCursor(Qt::SizeFDiagCursor);
        } else if (edges == (Qt::RightEdge | Qt::TopEdge) || edges == (Qt::LeftEdge | Qt::BottomEdge)) {
            setCursor(Qt::SizeBDiagCursor);
        } else if (edges == Qt::LeftEdge || edges == Qt::RightEdge) {
            setCursor(Qt::SizeHorCursor);
        } else if (edges == Qt::TopEdge || edges == Qt::BottomEdge) {
            setCursor(Qt::SizeVerCursor);
        } else if (cropRect_.contains(logical)) {
            setCursor(Qt::SizeAllCursor);
        } else {
            setCursor(Qt::CrossCursor);
        }
        return;
    }
    const Qt::CursorShape shape = controller_->cursorFor(
        widgetToLogical(lastWidgetPos_), QGuiApplication::keyboardModifiers(), 8.0 / zoom_);
    setCursor(shape);
}

void AnnotationCanvas::paintEvent(QPaintEvent *)
{
    QPainter painter(this);

    const QSizeF logical = logicalImageSize();
    const QRectF logicalRect = logical.isEmpty() ? QRectF(0.0, 0.0, width() / zoom_, height() / zoom_)
                                                 : QRectF(QPointF(0.0, 0.0), logical);

    if (editorMode_) {
        painter.fillRect(rect(), palette().color(QPalette::Dark));
        if (!image_.isNull()) {
            const QRectF target(panOffset_, QSizeF(logical.width() * zoom_, logical.height() * zoom_));
            for (int i = 5; i >= 1; --i) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(0, 0, 0, 12));
                painter.drawRect(target.adjusted(-i, -i + 2, i, i + 2));
            }
            painter.setPen(QPen(QColor(0, 0, 0, 90), 1.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(target.adjusted(-0.5, -0.5, 0.5, 0.5));
        }
    }

    painter.save();
    painter.translate(panOffset_);
    painter.scale(zoom_, zoom_);

    if (!image_.isNull()) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, zoom_ < 4.0);
        painter.drawImage(logicalRect, image_);
    }

    paintAnnotationOverlay(painter, logicalRect, *doc_, palette().color(QPalette::Highlight),
                           controller_->hoveredId(), controller_->draftItem(), &image_, imageScale_,
                           QPointF(0.0, 0.0), controller_->editingTextItemId());

    // Subtle pixel grid at >= 800% zoom in editor mode.
    if (editorMode_ && zoom_ >= 8.0 && !image_.isNull()) {
        const qreal step = 1.0 / imageScale_;
        const QRectF visibleLogical = logicalRect.intersected(
            QRectF(widgetToLogical(QPointF(0.0, 0.0)), widgetToLogical(QPointF(width(), height()))));
        if (!visibleLogical.isEmpty()) {
            QPen gridPen(QColor(128, 128, 128, 45), 1.0);
            gridPen.setCosmetic(true);
            painter.setPen(gridPen);
            const int xStart = std::max(0, int(std::floor(visibleLogical.left() * imageScale_)));
            const int xEnd = std::min(image_.width(), int(std::ceil(visibleLogical.right() * imageScale_)));
            const int yStart = std::max(0, int(std::floor(visibleLogical.top() * imageScale_)));
            const int yEnd = std::min(image_.height(), int(std::ceil(visibleLogical.bottom() * imageScale_)));
            for (int px = xStart; px <= xEnd; ++px) {
                const qreal lx = px * step;
                painter.drawLine(QPointF(lx, visibleLogical.top()), QPointF(lx, visibleLogical.bottom()));
            }
            for (int py = yStart; py <= yEnd; ++py) {
                const qreal ly = py * step;
                painter.drawLine(QPointF(visibleLogical.left(), ly), QPointF(visibleLogical.right(), ly));
            }
        }
    }

    // Crop overlay in ToolKind::Crop.
    if (editorMode_ && controller_->tool() == ToolKind::Crop && cropRect_.isValid()) {
        QPainterPath outside;
        outside.addRect(logicalRect);
        QPainterPath hole;
        hole.addRect(QRectF(cropRect_));
        painter.fillPath(outside.subtracted(hole), QColor(0, 0, 0, 145));

        const QColor accent = palette().color(QPalette::Highlight);
        QPen borderPen(accent, 1.5);
        borderPen.setCosmetic(true);
        painter.setPen(borderPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(cropRect_));

        const qreal r = cropHandleRadius / zoom_;
        const QRectF f(cropRect_);
        const QPointF c = f.center();
        const QList<QPointF> handles{
            f.topLeft(), QPointF(c.x(), f.top()), f.topRight(), QPointF(f.right(), c.y()),
            f.bottomRight(), QPointF(c.x(), f.bottom()), f.bottomLeft(), QPointF(f.left(), c.y()),
        };
        QPen hPen(QColor(255, 255, 255, 235), 1.5);
        hPen.setCosmetic(true);
        painter.setPen(hPen);
        painter.setBrush(accent);
        painter.setRenderHint(QPainter::Antialiasing, true);
        for (const QPointF &hp : handles) {
            painter.drawEllipse(hp, r, r);
        }
    }
    painter.restore();

    if (colorPickerActive_ && !image_.isNull() && rect().contains(lastWidgetPos_.toPoint())) {
        const QPointF logicalPos = widgetToLogical(lastWidgetPos_);
        if (logicalRect.contains(logicalPos)) {
            const QPoint nativePx = logicalToNativePixel(logicalPos, QPointF(0.0, 0.0), imageScale_, image_.size());
            PixelLoupe::paintColorPickerLoupe(painter, lastWidgetPos_.toPoint(), rect(), palette(), image_, nativePx,
                                              colorFormat_, true);
        }
    }
}

void AnnotationCanvas::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (!editorMode_ || image_.isNull()) {
        return;
    }
    if (pendingInitialView_) {
        applyInitialEditorView();
    } else if (autoFit_) {
        fitInView();
    } else {
        clampAndSyncPan();
        textEditor_->setViewTransform(zoom_, panOffset_);
        Q_EMIT viewChanged();
    }
}

void AnnotationCanvas::mousePressEvent(QMouseEvent *event)
{
    lastWidgetPos_ = event->position();

    if (colorPickerActive_) {
        if (event->button() == Qt::RightButton) {
            setColorPickerActive(false);
        } else if (event->button() == Qt::LeftButton && !image_.isNull()) {
            const QPointF logicalPos = widgetToLogical(event->position());
            if (QRectF(QPointF(0.0, 0.0), logicalImageSize()).contains(logicalPos)) {
                const QPoint nativePx = logicalToNativePixel(logicalPos, QPointF(0.0, 0.0), imageScale_, image_.size());
                const QColor sampled = sampleNativePixel(image_, nativePx);
                if (sampled.isValid()) {
                    Q_EMIT colorPicked(sampled, formatColor(sampled, colorFormat_),
                                       event->modifiers().testFlag(Qt::AltModifier));
                }
            }
            setColorPickerActive(false);
        }
        event->accept();
        return;
    }

    if (controller_->isEditingText() && event->button() == Qt::LeftButton) {
        textEditor_->commit(false);
        event->accept();
        return;
    }

    if (editorMode_ && ((event->button() == Qt::MiddleButton)
                        || (event->button() == Qt::LeftButton && spacePanKeyHeld_ && !controller_->isDragging()
                            && cropDrag_ == CropDrag::None))) {
        panning_ = true;
        panDragStartWidget_ = event->position();
        panDragStartOffset_ = panOffset_;
        refreshCursor();
        event->accept();
        return;
    }

    if (editorMode_ && controller_->tool() == ToolKind::Crop && event->button() == Qt::LeftButton) {
        const QPoint logical = widgetToLogical(event->position()).toPoint();
        const QSize logicalBounds = logicalImageSize().toSize();
        cropAnchorLogical_ = logical;
        cropLastLogical_ = logical;
        cropSpaceHeld_ = false;
        cropStartRect_ = cropRect_;
        if (const Qt::Edges edges = cropEdgesAt(logical); edges) {
            cropDrag_ = CropDrag::Resize;
            cropEdges_ = edges;
        } else if (cropRect_.isValid() && cropRect_.contains(logical)) {
            cropDrag_ = CropDrag::Move;
        } else {
            cropDrag_ = CropDrag::Create;
            cropAnchorLogical_ = QPoint(std::clamp(logical.x(), 0, logicalBounds.width()),
                                        std::clamp(logical.y(), 0, logicalBounds.height()));
            cropRect_ = QRect(cropAnchorLogical_, QSize(1, 1));
            Q_EMIT cropRectChanged(cropRect_);
        }
        refreshCursor();
        update();
        event->accept();
        return;
    }

    if (controller_->mousePress(widgetToLogical(event->position()), event->button(), event->modifiers(),
                                8.0 / zoom_)) {
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void AnnotationCanvas::mouseMoveEvent(QMouseEvent *event)
{
    lastWidgetPos_ = event->position();

    if (colorPickerActive_) {
        refreshCursor();
        update();
        event->accept();
        return;
    }

    if (panning_) {
        setPanOffset(panDragStartOffset_ + (event->position() - panDragStartWidget_));
        event->accept();
        return;
    }

    if (editorMode_ && controller_->tool() == ToolKind::Crop) {
        if (cropDrag_ != CropDrag::None) {
            const QPoint logical = widgetToLogical(event->position()).toPoint();
            const QPoint step = logical - cropLastLogical_;
            cropLastLogical_ = logical;
            if (cropSpaceHeld_ && (cropDrag_ == CropDrag::Create || cropDrag_ == CropDrag::Resize)) {
                const QSize bounds = logicalImageSize().toSize();
                cropAnchorLogical_.setX(std::clamp(cropAnchorLogical_.x() + step.x(), 0, bounds.width()));
                cropAnchorLogical_.setY(std::clamp(cropAnchorLogical_.y() + step.y(), 0, bounds.height()));
                if (cropDrag_ == CropDrag::Resize) {
                    cropStartRect_ = clampCropRect(cropStartRect_.translated(step), bounds);
                }
            }
            applyCropDrag(logical, event->modifiers());
        }
        refreshCursor();
        event->accept();
        return;
    }

    if (controller_->mouseMove(widgetToLogical(event->position()), event->modifiers())) {
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void AnnotationCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    lastWidgetPos_ = event->position();

    if (panning_ && (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)) {
        panning_ = false;
        refreshCursor();
        event->accept();
        return;
    }

    if (editorMode_ && controller_->tool() == ToolKind::Crop && cropDrag_ != CropDrag::None
        && event->button() == Qt::LeftButton) {
        const CropDrag mode = std::exchange(cropDrag_, CropDrag::None);
        cropSpaceHeld_ = false;
        if (cropRect_.width() < minimumCropSize || cropRect_.height() < minimumCropSize) {
            if (mode == CropDrag::Resize) {
                cropRect_ = cropStartRect_;
            } else {
                cropRect_ = QRect();
            }
            Q_EMIT cropRectChanged(cropRect_);
        }
        refreshCursor();
        update();
        event->accept();
        return;
    }

    if (controller_->mouseRelease(widgetToLogical(event->position()), event->button(), event->modifiers())) {
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void AnnotationCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    lastWidgetPos_ = event->position();
    if (editorMode_ && controller_->tool() == ToolKind::Crop && event->button() == Qt::LeftButton
        && hasCropRect() && cropRect_.contains(widgetToLogical(event->position()).toPoint())) {
        applyActiveCrop();
        event->accept();
        return;
    }
    if (controller_->mouseDoubleClick(widgetToLogical(event->position()), event->button(), event->modifiers())) {
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void AnnotationCanvas::wheelEvent(QWheelEvent *event)
{
    if (!editorMode_ || image_.isNull()) {
        QWidget::wheelEvent(event);
        return;
    }
    const Qt::KeyboardModifiers mods = event->modifiers();
    const QPoint angleDelta = event->angleDelta();

    // PhotoCraft match (zoomWithScrollWheel: false): Ctrl+Wheel and Alt+Wheel zoom anchored at cursor.
    if (mods.testFlag(Qt::ControlModifier) || mods.testFlag(Qt::AltModifier)) {
        const qreal steps = (angleDelta.y() != 0 ? angleDelta.y() : angleDelta.x()) / 120.0;
        if (!qFuzzyIsNull(steps)) {
            setZoomAnchored(zoom_ * std::pow(1.15, steps), event->position());
        }
        event->accept();
        return;
    }

    // Plain Wheel scrolls vertically (or horizontally if device sends X or Shift is held).
    QPointF delta(angleDelta.x(), angleDelta.y());
    if (mods.testFlag(Qt::ShiftModifier) && qFuzzyIsNull(delta.x())) {
        delta = QPointF(delta.y(), 0.0);
    }
    setPanOffset(panOffset_ + delta * 0.5);
    event->accept();
}

void AnnotationCanvas::contextMenuEvent(QContextMenuEvent *event)
{
    if (!editorMode_ || controller_->isEditingText() || controller_->isDragging() || panning_) {
        QWidget::contextMenuEvent(event);
        return;
    }
    const int hitId = doc_->topmostHitAt(widgetToLogical(event->pos()));
    if (hitId <= 0) {
        return;
    }
    doc_->select(hitId);

    QMenu menu(this);
    QAction *front = menu.addAction(QIcon::fromTheme(QStringLiteral("object-order-front")), tr("Bring to Front"));
    QAction *back = menu.addAction(QIcon::fromTheme(QStringLiteral("object-order-back")), tr("Send to Back"));
    menu.addSeparator();
    QAction *dup = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-duplicate"),
                                                   QIcon::fromTheme(QStringLiteral("edit-copy"))),
                                  tr("Duplicate"));
    QAction *del = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), tr("Delete"));

    connect(front, &QAction::triggered, this, [this, hitId] { doc_->bringToFront(hitId); });
    connect(back, &QAction::triggered, this, [this, hitId] { doc_->sendToBack(hitId); });
    connect(dup, &QAction::triggered, this, [this, hitId] { doc_->duplicateItem(hitId); });
    connect(del, &QAction::triggered, this, [this, hitId] { doc_->removeItem(hitId); });
    menu.exec(event->globalPos());
}

void AnnotationCanvas::keyPressEvent(QKeyEvent *event)
{
    if (controller_->isEditingText()) {
        if (controller_->keyPress(event)) {
            event->accept();
        }
        return;
    }

    if (colorPickerActive_) {
        const Qt::KeyboardModifiers mods = event->modifiers();
        const bool plain = !mods.testFlag(Qt::ControlModifier) && !mods.testFlag(Qt::AltModifier)
                        && !mods.testFlag(Qt::MetaModifier);
        if (event->key() == Qt::Key_Tab) {
            colorFormat_ = nextColorFormat(colorFormat_);
            Q_EMIT colorFormatChanged(colorFormat_);
            update();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Escape || (plain && !event->isAutoRepeat() && event->key() == Qt::Key_I)) {
            setColorPickerActive(false);
            event->accept();
            return;
        }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && !image_.isNull()) {
            const QPointF logicalPos = widgetToLogical(lastWidgetPos_);
            if (QRectF(QPointF(0.0, 0.0), logicalImageSize()).contains(logicalPos)) {
                const QPoint nativePx = logicalToNativePixel(logicalPos, QPointF(0.0, 0.0), imageScale_, image_.size());
                const QColor sampled = sampleNativePixel(image_, nativePx);
                if (sampled.isValid()) {
                    Q_EMIT colorPicked(sampled, formatColor(sampled, colorFormat_), mods.testFlag(Qt::AltModifier));
                }
            }
            setColorPickerActive(false);
            event->accept();
            return;
        }
    }

    if (editorMode_) {
        const Qt::KeyboardModifiers mods = event->modifiers();
        if (!colorPickerActive_ && !event->isAutoRepeat() && event->key() == Qt::Key_I && mods == Qt::NoModifier) {
            setColorPickerActive(true);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
            if (cropDrag_ != CropDrag::None) {
                cropSpaceHeld_ = true;
                event->accept();
                return;
            }
            if (!controller_->isDragging()) {
                spacePanKeyHeld_ = true;
                refreshCursor();
                event->accept();
                return;
            }
        }
        if (mods.testFlag(Qt::ControlModifier) && !mods.testFlag(Qt::AltModifier)) {
            switch (event->key()) {
            case Qt::Key_0:
                fitInView();
                event->accept();
                return;
            case Qt::Key_1:
                resetZoom100();
                event->accept();
                return;
            case Qt::Key_Plus:
            case Qt::Key_Equal:
                zoomByStep(+1);
                event->accept();
                return;
            case Qt::Key_Minus:
                zoomByStep(-1);
                event->accept();
                return;
            default:
                break;
            }
        }
        if (controller_->tool() == ToolKind::Crop) {
            if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
                if (hasCropRect()) {
                    applyActiveCrop();
                }
                event->accept();
                return;
            }
            if (event->key() == Qt::Key_Escape) {
                if (cropDrag_ != CropDrag::None) {
                    const CropDrag aborted = std::exchange(cropDrag_, CropDrag::None);
                    cropSpaceHeld_ = false;
                    cropRect_ = aborted == CropDrag::Create ? QRect() : cropStartRect_;
                    Q_EMIT cropRectChanged(cropRect_);
                    update();
                } else {
                    cancelActiveCrop();
                }
                event->accept();
                return;
            }
            if ((event->key() == Qt::Key_Left || event->key() == Qt::Key_Right
                 || event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)
                && hasCropRect()) {
                const int step = mods.testFlag(Qt::ShiftModifier) ? 10 : 1;
                QPoint delta;
                if (event->key() == Qt::Key_Left) {
                    delta = {-step, 0};
                } else if (event->key() == Qt::Key_Right) {
                    delta = {step, 0};
                } else if (event->key() == Qt::Key_Up) {
                    delta = {0, -step};
                } else {
                    delta = {0, step};
                }
                const SelectionConstraints c{mods.testFlag(Qt::AltModifier), mods.testFlag(Qt::ControlModifier)};
                setCropRect(steppedSelection(cropRect_, delta, c, minimumCropSize, logicalImageSize().toSize()));
                event->accept();
                return;
            }
            if (cropDrag_ != CropDrag::None) {
                applyCropDrag(cropLastLogical_, mods);
            }
        }
    }

    if (controller_->keyPress(event)) {
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void AnnotationCanvas::keyReleaseEvent(QKeyEvent *event)
{
    if (editorMode_ && event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        cropSpaceHeld_ = false;
        if (spacePanKeyHeld_) {
            spacePanKeyHeld_ = false;
            refreshCursor();
        }
    }
    if (editorMode_ && controller_->tool() == ToolKind::Crop && cropDrag_ != CropDrag::None) {
        applyCropDrag(cropLastLogical_, event->modifiers());
    }
    if (controller_->keyRelease(event)) {
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void AnnotationCanvas::focusOutEvent(QFocusEvent *event)
{
    if (QWidget *focused = QApplication::focusWidget(); focused && textEditor_->isAncestorOf(focused)) {
        QWidget::focusOutEvent(event);
        return;
    }
    spacePanKeyHeld_ = false;
    panning_ = false;
    cropDrag_ = CropDrag::None;
    cropSpaceHeld_ = false;
    controller_->cancelActiveGesture();
    QWidget::focusOutEvent(event);
}

void AnnotationCanvas::leaveEvent(QEvent *event)
{
    controller_->clearHover();
    if (colorPickerActive_) {
        lastWidgetPos_ = QPointF(-1.0, -1.0);
        update();
    }
    QWidget::leaveEvent(event);
}

} // namespace itsova
