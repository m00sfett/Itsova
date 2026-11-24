#include "annotationcanvas.h"

#include "annotationrenderer.h"

#include <QFocusEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

namespace glimply
{

AnnotationCanvas::AnnotationCanvas(QWidget *parent)
    : QWidget(parent)
    , ownedDoc_(new AnnotationDocument(this))
    , doc_(ownedDoc_)
    , controller_(new AnnotationController(doc_, this))
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    connect(doc_, &AnnotationDocument::changed, this, qOverload<>(&QWidget::update));
    connect(controller_, &AnnotationController::previewChanged, this, qOverload<>(&QWidget::update));
    connect(controller_, &AnnotationController::cursorChanged, this, &AnnotationCanvas::refreshCursor);
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
    updateGeometry();
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

void AnnotationCanvas::setDocument(AnnotationDocument *document)
{
    AnnotationDocument *next = document ? document : ownedDoc_;
    if (doc_ == next) {
        return;
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

void AnnotationCanvas::setZoom(qreal zoom)
{
    const qreal clamped = std::clamp(zoom, 0.1, 16.0);
    if (!qFuzzyCompare(zoom_, clamped)) {
        zoom_ = clamped;
        refreshCursor();
        update();
    }
}

qreal AnnotationCanvas::zoom() const
{
    return zoom_;
}

void AnnotationCanvas::setPanOffset(const QPointF &offset)
{
    if (panOffset_ != offset) {
        panOffset_ = offset;
        update();
    }
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

QImage AnnotationCanvas::renderExport() const
{
    const QSizeF logical = logicalImageSize();
    if (image_.isNull() || logical.isEmpty()) {
        return {};
    }
    return renderAnnotatedCrop(image_, QRectF(QPointF(0.0, 0.0), logical), imageScale_, *doc_);
}

void AnnotationCanvas::refreshCursor()
{
    const Qt::CursorShape shape = controller_->cursorFor(
        widgetToLogical(lastWidgetPos_), QGuiApplication::keyboardModifiers(), 8.0 / zoom_);
    setCursor(shape);
}

void AnnotationCanvas::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.translate(panOffset_);
    painter.scale(zoom_, zoom_);

    const QSizeF logical = logicalImageSize();
    const QRectF logicalRect = logical.isEmpty() ? QRectF(0.0, 0.0, width() / zoom_, height() / zoom_)
                                                 : QRectF(QPointF(0.0, 0.0), logical);

    if (!image_.isNull()) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, zoom_ < 4.0);
        painter.drawImage(logicalRect, image_);
    }

    paintAnnotationOverlay(painter, logicalRect, *doc_, palette().color(QPalette::Highlight),
                           controller_->hoveredId(), controller_->draftItem(), &image_, imageScale_,
                           QPointF(0.0, 0.0));
}

void AnnotationCanvas::mousePressEvent(QMouseEvent *event)
{
    lastWidgetPos_ = event->position();
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
    if (controller_->mouseMove(widgetToLogical(event->position()), event->modifiers())) {
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void AnnotationCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    lastWidgetPos_ = event->position();
    if (controller_->mouseRelease(widgetToLogical(event->position()), event->button(), event->modifiers())) {
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void AnnotationCanvas::keyPressEvent(QKeyEvent *event)
{
    if (controller_->keyPress(event)) {
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void AnnotationCanvas::keyReleaseEvent(QKeyEvent *event)
{
    if (controller_->keyRelease(event)) {
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void AnnotationCanvas::focusOutEvent(QFocusEvent *event)
{
    controller_->cancelActiveGesture();
    QWidget::focusOutEvent(event);
}

void AnnotationCanvas::leaveEvent(QEvent *event)
{
    controller_->clearHover();
    QWidget::leaveEvent(event);
}

} // namespace glimply
