#include "recordinghint.h"

#include <LayerShellQt/Window>

#include <QEnterEvent>
#include <QFontMetrics>
#include <QIcon>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QTimer>
#include <QWindow>

namespace itsova
{

namespace
{
constexpr int padding = 14;
constexpr int iconSize = 22;
constexpr int gap = 10;
constexpr int radius = 14;
constexpr int maxTextWidth = 420;

QColor withAlpha(QColor color, int alpha)
{
    color.setAlpha(alpha);
    return color;
}
} // namespace

RecordingHint::RecordingHint(QWidget *parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool | Qt::WindowDoesNotAcceptFocus)
    , timer_(new QTimer(this))
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, &RecordingHint::dismiss);
}

void RecordingHint::setContent(const QString &iconName, const QString &title, const QString &detail, bool clickable)
{
    iconName_ = iconName;
    title_ = title;
    detail_ = detail;
    clickable_ = clickable;
    setCursor(clickable ? Qt::PointingHandCursor : Qt::ArrowCursor);
    setAccessibleName(detail.isEmpty() ? title : title + QStringLiteral(": ") + detail);
    QFont bold = font();
    bold.setBold(true);
    const QFontMetrics titleMetrics(bold);
    const QFontMetrics detailMetrics(font());
    const int textWidth =
        std::min(maxTextWidth, std::max(titleMetrics.horizontalAdvance(title), detailMetrics.horizontalAdvance(detail)));
    const int textHeight = titleMetrics.height() + (detail.isEmpty() ? 0 : detailMetrics.height() + 2);
    setFixedSize(padding * 2 + iconSize + gap + textWidth, padding * 2 + std::max(iconSize, textHeight));
    update();
}

void RecordingHint::showAt(QScreen *screen, const QRect &rect, int durationMs)
{
    winId(); // the platform window must exist before the layer-shell role is set
    QWindow *window = windowHandle();
    if (screen) {
        window->setScreen(screen);
    }
    if (auto *layer = LayerShellQt::Window::get(window)) {
        layer->setLayer(LayerShellQt::Window::LayerOverlay);
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorLeft));
        layer->setExclusiveZone(-1); // ignore panels: the position is exact
        layer->setScope(QStringLiteral("itsova-recording-hint"));
        if (screen) {
            layer->setScreen(screen);
        }
        const QPoint origin = screen ? screen->geometry().topLeft() : QPoint();
        layer->setMargins(QMargins(rect.x() - origin.x(), rect.y() - origin.y(), 0, 0));
    }
    resize(rect.size());
    show();
    raise();
    timer_->start(durationMs);
}

void RecordingHint::dismiss()
{
    timer_->stop();
    if (isVisible()) {
        hide();
        Q_EMIT dismissed();
    }
}

void RecordingHint::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF body = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setBrush(withAlpha(palette().color(QPalette::Window), 245));
    painter.setPen(QPen(withAlpha(palette().color(QPalette::WindowText), 40), 1));
    painter.drawRoundedRect(body, radius, radius);

    const int textX = padding + iconSize + gap;
    const int textWidth = width() - textX - padding;
    QFont bold = font();
    bold.setBold(true);
    const QFontMetrics titleMetrics(bold);
    const QFontMetrics detailMetrics(font());
    const int textHeight = titleMetrics.height() + (detail_.isEmpty() ? 0 : detailMetrics.height() + 2);
    const int top = (height() - textHeight) / 2;
    QIcon::fromTheme(iconName_).paint(&painter, QRect(padding, (height() - iconSize) / 2, iconSize, iconSize));
    painter.setPen(palette().color(QPalette::WindowText));
    painter.setFont(bold);
    painter.drawText(QRect(textX, top, textWidth, titleMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                     titleMetrics.elidedText(title_, Qt::ElideRight, textWidth));
    if (!detail_.isEmpty()) {
        painter.setFont(font());
        QColor muted = palette().color(QPalette::WindowText);
        muted.setAlphaF(clickable_ ? 0.9 : 0.7);
        painter.setPen(muted);
        painter.drawText(QRect(textX, top + titleMetrics.height() + 2, textWidth, detailMetrics.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, detailMetrics.elidedText(detail_, Qt::ElideMiddle, textWidth));
    }
}

void RecordingHint::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && clickable_) {
        Q_EMIT clicked();
    }
    dismiss();
    event->accept();
}

void RecordingHint::enterEvent(QEnterEvent *event)
{
    timer_->stop(); // stays while the pointer is on it
    QWidget::enterEvent(event);
}

void RecordingHint::leaveEvent(QEvent *event)
{
    if (isVisible()) {
        timer_->start(1000);
    }
    QWidget::leaveEvent(event);
}

} // namespace itsova
