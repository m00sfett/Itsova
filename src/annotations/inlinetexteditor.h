#pragma once

#include "annotationitem.h"

#include <QPointF>
#include <QWidget>

class QPlainTextEdit;

namespace itsova
{

// Frameless inline text editor hosted on top of AnnotationCanvas or CaptureOverlay.
// Supports Point Text (auto-growing single/multi-line) and Paragraph Text (fixed wrap width),
// exterior-margin drag-to-move without leaving edit mode, and PhotoCraft/Photoshop Esc-to-commit.
class InlineTextEditor : public QWidget
{
    Q_OBJECT

public:
    static constexpr int outerMoveMargin = 12;

    explicit InlineTextEditor(QWidget *parent = nullptr);

    void startEditing(const AnnotationItem &item, bool isExisting, qreal zoom = 1.0,
                      const QPointF &panOffset = {});
    void updateStyle(const AnnotationItem &styledItem);
    void setViewTransform(qreal zoom, const QPointF &panOffset);

    [[nodiscard]] AnnotationItem currentItem() const;
    [[nodiscard]] bool isExistingItem() const;
    [[nodiscard]] QPlainTextEdit *textEdit() const;

    void commit(bool switchToSelect = true);
    void cancel();

Q_SIGNALS:
    void draftChanged(const itsova::AnnotationItem &item);
    void styleToggled(const itsova::AnnotationItem &item);
    void committed(const itsova::AnnotationItem &item, bool isExisting, bool switchToSelect);
    void cancelled(int existingItemId);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void syncGeometry();
    void applyVisualAppearance();

    QPlainTextEdit *edit_ = nullptr;
    AnnotationItem item_;
    bool isExisting_ = false;
    bool active_ = false;
    bool updating_ = false;
    qreal zoom_ = 1.0;
    QPointF panOffset_;

    bool draggingMove_ = false;
    QPointF moveAnchorGlobal_;
    QPointF startLogicalTopLeft_;
};

} // namespace itsova
