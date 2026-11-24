#pragma once

#include "annotationcontroller.h"
#include "annotationdocument.h"

#include <QImage>
#include <QPointF>
#include <QSizeF>
#include <QWidget>

namespace glimply
{

// Reusable interactive vector canvas widget displaying an image and managing annotations through
// AnnotationController. Shared by CaptureOverlay workflows and QuickEditorWindow.
class AnnotationCanvas : public QWidget
{
    Q_OBJECT

public:
    explicit AnnotationCanvas(QWidget *parent = nullptr);

    void setImage(const QImage &image, qreal scale = 1.0);
    [[nodiscard]] const QImage &image() const;
    [[nodiscard]] qreal imageScale() const;
    [[nodiscard]] QSizeF logicalImageSize() const;

    // Attaches an external document, or passes nullptr to revert to the internal document.
    void setDocument(AnnotationDocument *document);
    [[nodiscard]] AnnotationDocument *document() const;
    [[nodiscard]] AnnotationController *controller() const;

    void setZoom(qreal zoom);
    [[nodiscard]] qreal zoom() const;
    void setPanOffset(const QPointF &offset);
    [[nodiscard]] QPointF panOffset() const;

    [[nodiscard]] QPointF widgetToLogical(const QPointF &widgetPos) const;
    [[nodiscard]] QPointF logicalToWidget(const QPointF &logicalPos) const;

    [[nodiscard]] QImage renderExport() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void refreshCursor();

    QImage image_;
    qreal imageScale_ = 1.0;
    AnnotationDocument *ownedDoc_ = nullptr;
    AnnotationDocument *doc_ = nullptr;
    AnnotationController *controller_ = nullptr;
    qreal zoom_ = 1.0;
    QPointF panOffset_;
    QPointF lastWidgetPos_;
};

} // namespace glimply
