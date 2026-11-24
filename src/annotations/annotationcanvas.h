#pragma once

#include "annotationcontroller.h"
#include "annotationdocument.h"
#include "colorformat.h"

#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QSizeF>
#include <QWidget>

namespace glimply
{

class CropImageCommand;
class InlineTextEditor;

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

    // Editor workspace mode enables dark backdrop, soft shadow, crisp pixel grid (>=800%),
    // PhotoCraft zoom/pan (Space+drag, Middle-drag, Ctrl/Alt+Wheel), right-click context menu,
    // and undoable ToolKind::Crop.
    void setEditorMode(bool enabled);
    [[nodiscard]] bool editorMode() const;

    // Attaches an external document, or passes nullptr to revert to the internal document.
    void setDocument(AnnotationDocument *document);
    [[nodiscard]] AnnotationDocument *document() const;
    [[nodiscard]] AnnotationController *controller() const;

    void setZoom(qreal zoom);
    void setZoomAnchored(qreal zoom, const QPointF &widgetAnchor);
    void zoomByStep(int steps, const QPointF &widgetAnchor = {});
    void fitInView();
    void resetZoom100();
    [[nodiscard]] qreal zoom() const;
    [[nodiscard]] bool isAutoFit() const;
    void setPanOffset(const QPointF &offset);
    [[nodiscard]] QPointF panOffset() const;

    [[nodiscard]] QPointF widgetToLogical(const QPointF &widgetPos) const;
    [[nodiscard]] QPointF logicalToWidget(const QPointF &logicalPos) const;

    // Interactive undoable Crop (ToolKind::Crop).
    void setCropRect(const QRect &logicalRect);
    [[nodiscard]] QRect cropRect() const;
    [[nodiscard]] bool hasCropRect() const;
    bool applyActiveCrop();
    void cancelActiveCrop();

    [[nodiscard]] QImage renderExport() const;
    [[nodiscard]] InlineTextEditor *textEditor() const;

    void setColorPickerActive(bool active);
    [[nodiscard]] bool isColorPickerActive() const;
    void setColorFormat(ColorFormat format);
    [[nodiscard]] ColorFormat colorFormat() const;

Q_SIGNALS:
    void viewChanged();
    void cropRectChanged(const QRect &logicalCropRect);
    void imageDimensionsChanged();
    void colorPickerActiveChanged(bool active);
    void colorFormatChanged(glimply::ColorFormat format);
    void colorPicked(const QColor &color, const QString &formatted, bool applyToTool);

protected:
    bool focusNextPrevChild(bool next) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    friend class CropImageCommand;

    enum class CropDrag { None, Create, Move, Resize };

    void setCroppedImageInternal(const QImage &image, qreal scale);
    void clampAndSyncPan();
    void applyInitialEditorView();
    void refreshCursor();
    void applyCropDrag(const QPoint &logicalPos, Qt::KeyboardModifiers mods);
    [[nodiscard]] Qt::Edges cropEdgesAt(const QPoint &logicalPos) const;

    QImage image_;
    qreal imageScale_ = 1.0;
    bool editorMode_ = false;
    bool autoFit_ = false;
    bool pendingInitialView_ = false;
    AnnotationDocument *ownedDoc_ = nullptr;
    AnnotationDocument *doc_ = nullptr;
    AnnotationController *controller_ = nullptr;
    InlineTextEditor *textEditor_ = nullptr;
    qreal zoom_ = 1.0;
    QPointF panOffset_;
    QPointF lastWidgetPos_;

    // Panning state (Space+drag idle or Middle-button drag in editor mode).
    bool spacePanKeyHeld_ = false;
    bool panning_ = false;
    bool colorPickerActive_ = false;
    ColorFormat colorFormat_ = ColorFormat::Hex;
    QPointF panDragStartWidget_;
    QPointF panDragStartOffset_;

    // Crop interaction state (logical pixels).
    QRect cropRect_;
    CropDrag cropDrag_ = CropDrag::None;
    bool cropSpaceHeld_ = false;
    QPoint cropAnchorLogical_;
    QPoint cropLastLogical_;
    QRect cropStartRect_;
    Qt::Edges cropEdges_;
};

} // namespace glimply
