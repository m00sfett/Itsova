#include "pinwindow.h"

#include "pinstore.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QStyle>
#include <QTimer>
#include <QWheelEvent>
#include <QWindow>

#include <cmath>

namespace glimply
{
namespace
{

constexpr int readoutMs = 1000;

QString percent(double value)
{
    return QStringLiteral("%1 %").arg(int(std::lround(value * 100.0)));
}

} // namespace

PinWindow::PinWindow(QString pinId, QImage image, double zoom, double opacity, QWidget *parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool)
    , pinId_(std::move(pinId))
    , image_(std::move(image))
    , zoom_(nextPinZoom(zoom, 0))
    , opacity_(clampPinOpacity(opacity))
    , readoutTimer_(new QTimer(this))
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(true);
    // KWin finds the pin by this caption; it never shows anywhere because the window is frameless
    // and skipped from task bar and switcher.
    setWindowTitle(kwinCaption());
    setAccessibleName(tr("Pinned screenshot"));
    readoutTimer_->setSingleShot(true);
    readoutTimer_->setInterval(readoutMs);
    connect(readoutTimer_, &QTimer::timeout, this, [this] {
        readout_.clear();
        update();
    });
    applySize();
}

QString PinWindow::pinId() const
{
    return pinId_;
}

QString PinWindow::kwinCaption() const
{
    return QStringLiteral("glimply-pin-") + pinId_;
}

const QImage &PinWindow::image() const
{
    return image_;
}

double PinWindow::zoom() const
{
    return zoom_;
}

double PinWindow::opacity() const
{
    return opacity_;
}

void PinWindow::setZoom(double zoom)
{
    const double clamped = nextPinZoom(zoom, 0);
    if (std::abs(clamped - zoom_) < 1e-6) {
        return;
    }
    zoom_ = clamped;
    applySize();
    showReadout(percent(zoom_));
    Q_EMIT zoomChanged(zoom_);
}

void PinWindow::setOpacityLevel(double opacity)
{
    const double clamped = clampPinOpacity(opacity);
    if (std::abs(clamped - opacity_) < 1e-6) {
        return;
    }
    opacity_ = clamped;
    showReadout(tr("Opacity %1").arg(percent(opacity_)));
    Q_EMIT opacityChanged(opacity_);
}

void PinWindow::setCaptureTransparent(bool transparent)
{
    if (captureTransparent_ == transparent) {
        return;
    }
    captureTransparent_ = transparent;
    repaint();
}

QSize PinWindow::logicalSizeFor(qreal devicePixelRatio) const
{
    const qreal dpr = devicePixelRatio > 0 ? devicePixelRatio : 1.0;
    return QSize(std::max(16, int(std::lround(image_.width() * zoom_ / dpr))),
                 std::max(16, int(std::lround(image_.height() * zoom_ / dpr))));
}

void PinWindow::applySize()
{
    const qreal dpr = windowHandle() && windowHandle()->screen() ? windowHandle()->screen()->devicePixelRatio()
                                                                 : qApp->devicePixelRatio();
    setFixedSize(logicalSizeFor(dpr));
}

void PinWindow::showReadout(const QString &text)
{
    readout_ = text;
    readoutTimer_->start();
    update();
}

void PinWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (QWindow *window = windowHandle()) {
        connect(window, &QWindow::screenChanged, this, [this] { applySize(); }, Qt::UniqueConnection);
    }
    applySize();
}

void PinWindow::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
    if (captureTransparent_) {
        return;
    }
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setOpacity(opacity_);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, zoom_ < 1.0);
    painter.drawImage(rect(), image_);
    painter.setOpacity(1.0);

    // A thin outline keeps a white screenshot distinguishable on a white page; it switches to the
    // focus colour on hover or focus.
    const QPalette pal = palette();
    QColor outline = hovered_ || hasFocus() ? pal.color(QPalette::Highlight) : pal.color(QPalette::WindowText);
    if (!(hovered_ || hasFocus())) {
        outline.setAlphaF(0.25);
    }
    painter.setPen(QPen(outline, hovered_ || hasFocus() ? 2 : 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(rect().adjusted(0, 0, -1, -1));

    if (!readout_.isEmpty()) {
        const QFontMetrics fm(font());
        const int margin = style()->pixelMetric(QStyle::PM_LayoutHorizontalSpacing) > 0
                               ? style()->pixelMetric(QStyle::PM_LayoutHorizontalSpacing)
                               : 6;
        QRect box = fm.boundingRect(readout_).adjusted(-margin, -margin / 2, margin, margin / 2);
        box.moveBottomRight(rect().bottomRight() - QPoint(margin, margin));
        QColor background = pal.color(QPalette::ToolTipBase);
        background.setAlphaF(0.9);
        painter.setPen(Qt::NoPen);
        painter.setBrush(background);
        painter.drawRoundedRect(box, 4, 4);
        painter.setPen(pal.color(QPalette::ToolTipText));
        painter.drawText(box, Qt::AlignCenter, readout_);
    }
}

void PinWindow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        pressed_ = true;
        pressPos_ = event->position().toPoint();
    }
    QWidget::mousePressEvent(event);
}

void PinWindow::mouseMoveEvent(QMouseEvent *event)
{
    // Hand the drag to the compositor once the pointer really moves, so a double click still works.
    if (pressed_ && (event->position().toPoint() - pressPos_).manhattanLength() >= QApplication::startDragDistance()) {
        pressed_ = false;
        if (QWindow *window = windowHandle()) {
            window->startSystemMove();
        }
    }
    QWidget::mouseMoveEvent(event);
}

void PinWindow::mouseReleaseEvent(QMouseEvent *event)
{
    pressed_ = false;
    QWidget::mouseReleaseEvent(event);
}

void PinWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    // Pins are persistent, so a double click resets the zoom instead of closing (Q20).
    if (event->button() == Qt::LeftButton) {
        setZoom(1.0);
        showReadout(percent(zoom_));
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void PinWindow::wheelEvent(QWheelEvent *event)
{
    const int delta = event->angleDelta().y();
    if (delta == 0) {
        return;
    }
    const int direction = delta > 0 ? 1 : -1;
    if (event->modifiers() & Qt::ControlModifier) {
        setOpacityLevel(nextPinOpacity(opacity_, direction));
    } else {
        setZoom(nextPinZoom(zoom_, direction));
    }
    event->accept();
}

void PinWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        Q_EMIT removeRequested();
        return;
    }
    if (event->matches(QKeySequence::Copy)) {
        Q_EMIT copyRequested();
        return;
    }
    if (event->key() == Qt::Key_0 && (event->modifiers() & Qt::ControlModifier)) {
        setZoom(1.0);
        return;
    }
    QWidget::keyPressEvent(event);
}

void PinWindow::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), tr("Copy"), this, &PinWindow::copyRequested);
    menu.addAction(QIcon::fromTheme(QStringLiteral("document-edit")), tr("Edit in Window"), this,
                   &PinWindow::editRequested);
    menu.addAction(QIcon::fromTheme(QStringLiteral("document-save-as")), tr("Save As…"), this,
                   &PinWindow::saveAsRequested);
    menu.addSeparator();
    QMenu *zoomMenu = menu.addMenu(QIcon::fromTheme(QStringLiteral("zoom")), tr("Zoom (%1)").arg(percent(zoom_)));
    for (double step : pinZoomSteps()) {
        QAction *action = zoomMenu->addAction(percent(step), this, [this, step] { setZoom(step); });
        action->setCheckable(true);
        action->setChecked(std::abs(step - zoom_) < 1e-6);
    }
    QMenu *opacityMenu = menu.addMenu(tr("Opacity (%1)").arg(percent(opacity_)));
    for (int level = 10; level >= 2; --level) {
        const double value = level / 10.0;
        QAction *action = opacityMenu->addAction(percent(value), this, [this, value] { setOpacityLevel(value); });
        action->setCheckable(true);
        action->setChecked(std::abs(value - opacity_) < 1e-6);
    }
    menu.addSeparator();
    menu.addAction(QIcon::fromTheme(QStringLiteral("window-close")), tr("Remove Pin"), this,
                   &PinWindow::removeRequested);
    menu.exec(event->globalPos());
}

void PinWindow::enterEvent(QEnterEvent *event)
{
    hovered_ = true;
    update();
    QWidget::enterEvent(event);
}

void PinWindow::leaveEvent(QEvent *event)
{
    hovered_ = false;
    update();
    QWidget::leaveEvent(event);
}

} // namespace glimply
