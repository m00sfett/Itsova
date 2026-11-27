#include "inlinetexteditor.h"

#include "annotationtextlayout.h"

#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QTextBlockFormat>
#include <QTextCursor>

#include <algorithm>
#include <cmath>

namespace itsova
{

InlineTextEditor::InlineTextEditor(QWidget *parent)
    : QWidget(parent)
    , edit_(new QPlainTextEdit(this))
{
    setMouseTracking(true);
    setAttribute(Qt::WA_TranslucentBackground);

    edit_->setFrameShape(QFrame::NoFrame);
    edit_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    edit_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    edit_->setTabChangesFocus(false);
    edit_->viewport()->setAutoFillBackground(false);
    edit_->installEventFilter(this);

    connect(edit_, &QPlainTextEdit::textChanged, this, [this] {
        if (updating_ || !active_) {
            return;
        }
        item_.text = edit_->toPlainText();
        syncGeometry();
        Q_EMIT draftChanged(currentItem());
    });

    hide();
}

void InlineTextEditor::startEditing(const AnnotationItem &item, bool isExisting, qreal zoom,
                                    const QPointF &panOffset)
{
    updating_ = true;
    active_ = true;
    isExisting_ = isExisting;
    item_ = item;
    item_.kind = ItemKind::Text;
    zoom_ = zoom > 0.0 ? zoom : 1.0;
    panOffset_ = panOffset;
    draggingMove_ = false;

    edit_->setLineWrapMode(item_.paragraphMode ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    edit_->setPlainText(item_.text);
    applyVisualAppearance();
    syncGeometry();
    show();
    raise();

    QTextCursor cursor = edit_->textCursor();
    cursor.movePosition(QTextCursor::End);
    edit_->setTextCursor(cursor);
    edit_->setFocus(Qt::MouseFocusReason);
    updating_ = false;
}

void InlineTextEditor::updateStyle(const AnnotationItem &styledItem)
{
    if (!active_) {
        return;
    }
    updating_ = true;
    item_.color = styledItem.color;
    item_.font = styledItem.font;
    item_.textAlignment = styledItem.textAlignment;
    item_.legibility = styledItem.legibility;
    applyVisualAppearance();
    syncGeometry();
    update();
    updating_ = false;
}

void InlineTextEditor::setViewTransform(qreal zoom, const QPointF &panOffset)
{
    zoom_ = zoom > 0.0 ? zoom : 1.0;
    panOffset_ = panOffset;
    if (active_) {
        applyVisualAppearance();
        syncGeometry();
    }
}

AnnotationItem InlineTextEditor::currentItem() const
{
    AnnotationItem out = item_;
    out.text = edit_->toPlainText();
    const QSizeF measured = measureAnnotationText(out);
    out.rect = QRectF(out.rect.topLeft(), measured);
    return out;
}

bool InlineTextEditor::isExistingItem() const
{
    return isExisting_;
}

QPlainTextEdit *InlineTextEditor::textEdit() const
{
    return edit_;
}

void InlineTextEditor::commit(bool switchToSelect)
{
    if (!active_) {
        return;
    }
    active_ = false;
    draggingMove_ = false;
    const AnnotationItem result = currentItem();
    const bool existing = isExisting_;
    hide();
    Q_EMIT committed(result, existing, switchToSelect);
}

void InlineTextEditor::cancel()
{
    if (!active_) {
        return;
    }
    active_ = false;
    draggingMove_ = false;
    const int existingId = isExisting_ ? item_.id : 0;
    hide();
    Q_EMIT cancelled(existingId);
}

void InlineTextEditor::applyVisualAppearance()
{
    QFont widgetFont = resolvedAnnotationFont(item_);
    widgetFont.setPointSizeF(std::max(6.0, widgetFont.pointSizeF() * zoom_));
    edit_->setFont(widgetFont);

    QPalette pal = edit_->palette();
    pal.setColor(QPalette::Text, item_.color);
    pal.setColor(QPalette::Base, Qt::transparent);
    pal.setColor(QPalette::Window, Qt::transparent);
    edit_->setPalette(pal);

    // Apply alignment to all blocks cleanly while preserving cursor position.
    QTextCursor cur = edit_->textCursor();
    const int pos = cur.position();
    QTextCursor all(edit_->document());
    all.select(QTextCursor::Document);
    QTextBlockFormat fmt;
    fmt.setAlignment(item_.textAlignment);
    all.mergeBlockFormat(fmt);
    cur.setPosition(std::min(pos, edit_->document()->characterCount() - 1));
    edit_->setTextCursor(cur);
}

void InlineTextEditor::syncGeometry()
{
    AnnotationItem temp = item_;
    temp.text = edit_->toPlainText();
    const QSizeF logicalMeasured = measureAnnotationText(temp);
    const QFontMetricsF metrics(resolvedAnnotationFont(temp));
    const qreal minLogicalH = std::max(metrics.lineSpacing() + 4.0, 26.0);
    const qreal minLogicalW = temp.paragraphMode ? std::max(60.0, item_.rect.width()) : 72.0;

    const qreal logicalW = temp.paragraphMode ? minLogicalW : std::max(minLogicalW, logicalMeasured.width() + 14.0);
    const qreal logicalH = std::max(minLogicalH, logicalMeasured.height() + 8.0);

    const int innerW = std::max(36, int(std::ceil(logicalW * zoom_)));
    const int innerH = std::max(24, int(std::ceil(logicalH * zoom_)));

    const QPointF widgetTopLeft = item_.rect.topLeft() * zoom_ + panOffset_;
    setGeometry(int(std::lround(widgetTopLeft.x())) - outerMoveMargin,
                int(std::lround(widgetTopLeft.y())) - outerMoveMargin,
                innerW + 2 * outerMoveMargin,
                innerH + 2 * outerMoveMargin);
    edit_->setGeometry(outerMoveMargin, outerMoveMargin, innerW, innerH);
}

bool InlineTextEditor::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == edit_ && event->type() == QEvent::KeyPress && active_) {
        auto *ke = static_cast<QKeyEvent *>(event);
        const Qt::KeyboardModifiers mods = ke->modifiers();
        const int key = ke->key();

        // Escape commits text (PhotoCraft / Photoshop useEscToCommit).
        if (key == Qt::Key_Escape) {
            commit(true);
            return true;
        }
        // Ctrl+Enter or Keypad Enter commits text; plain Return inserts a newline.
        if ((key == Qt::Key_Return || key == Qt::Key_Enter)
            && (mods.testFlag(Qt::ControlModifier) || mods.testFlag(Qt::KeypadModifier))) {
            commit(true);
            return true;
        }
        if (mods.testFlag(Qt::ControlModifier) && !mods.testFlag(Qt::AltModifier)) {
            if (key == Qt::Key_B) {
                item_.font.setBold(!resolvedAnnotationFont(item_).bold());
                applyVisualAppearance();
                syncGeometry();
                Q_EMIT styleToggled(currentItem());
                return true;
            }
            if (key == Qt::Key_I) {
                item_.font.setItalic(!resolvedAnnotationFont(item_).italic());
                applyVisualAppearance();
                syncGeometry();
                Q_EMIT styleToggled(currentItem());
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void InlineTextEditor::paintEvent(QPaintEvent *)
{
    if (!active_) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF inner = QRectF(edit_->geometry());
    if (item_.legibility == TextLegibility::Pill) {
        const QColor contrast = contrastingTextColor(item_.color);
        const QColor backing = contrast.lightness() < 128 ? QColor(18, 18, 20, 225) : QColor(250, 250, 252, 235);
        painter.setPen(Qt::NoPen);
        painter.setBrush(backing);
        painter.drawRoundedRect(inner.adjusted(-6.0, -3.0, 6.0, 3.0), 6.0, 6.0);
    }

    const QColor accent = palette().color(QPalette::Highlight);
    QPen pen(accent, 1.25, Qt::DashLine);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(inner.adjusted(-1.0, -1.0, 1.0, 1.0), 3.0, 3.0);
}

void InlineTextEditor::mousePressEvent(QMouseEvent *event)
{
    if (active_ && event->button() == Qt::LeftButton && !edit_->geometry().contains(event->position().toPoint())) {
        draggingMove_ = true;
        moveAnchorGlobal_ = event->globalPosition();
        startLogicalTopLeft_ = item_.rect.topLeft();
        setCursor(Qt::SizeAllCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void InlineTextEditor::mouseMoveEvent(QMouseEvent *event)
{
    if (draggingMove_) {
        const QPointF logicalDelta = (event->globalPosition() - moveAnchorGlobal_) / zoom_;
        item_.rect.moveTopLeft(startLogicalTopLeft_ + logicalDelta);
        syncGeometry();
        Q_EMIT draftChanged(currentItem());
        event->accept();
        return;
    }
    if (active_ && !edit_->geometry().contains(event->position().toPoint())) {
        setCursor(Qt::SizeAllCursor);
    } else {
        setCursor(Qt::IBeamCursor);
    }
    QWidget::mouseMoveEvent(event);
}

void InlineTextEditor::mouseReleaseEvent(QMouseEvent *event)
{
    if (draggingMove_ && event->button() == Qt::LeftButton) {
        draggingMove_ = false;
        edit_->setFocus(Qt::MouseFocusReason);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void InlineTextEditor::keyPressEvent(QKeyEvent *event)
{
    // Swallow keys while active so overlay/canvas shortcuts never trigger mid-editing.
    event->accept();
}

} // namespace itsova
