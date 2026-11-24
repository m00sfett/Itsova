#include "countdowntimer.h"

#include <QCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace glimply
{
namespace
{

constexpr int bannerShadow = 6;
constexpr qreal bannerRadius = 8.0;
constexpr int bannerTopOffset = 18;

} // namespace

CountdownController::CountdownController(QObject *parent)
    : QObject(parent)
    , tickTimer_(new QTimer(this))
    , flushTimer_(new QTimer(this))
{
    tickTimer_->setSingleShot(false);
    flushTimer_->setSingleShot(true);
    connect(tickTimer_, &QTimer::timeout, this, &CountdownController::onTick);
    connect(flushTimer_, &QTimer::timeout, this, &CountdownController::onFlushDone);
}

void CountdownController::setTickIntervalMs(int ms)
{
    tickIntervalMs_ = std::max(1, ms);
}

void CountdownController::setHideFlushDelayMs(int ms)
{
    hideFlushDelayMs_ = std::max(0, ms);
}

void CountdownController::start(int seconds, const QString &label, std::function<void()> onReady,
                                std::function<void()> onCancelled)
{
    if (active_) {
        cancel();
    }
    if (seconds <= 0) {
        if (onReady) {
            onReady();
        }
        return;
    }

    active_ = true;
    flushing_ = false;
    remaining_ = seconds;
    label_ = label;
    onReady_ = std::move(onReady);
    onCancelled_ = std::move(onCancelled);

    Q_EMIT started(remaining_, label_);
    tickTimer_->start(tickIntervalMs_);
}

void CountdownController::cancel()
{
    if (!active_) {
        return;
    }
    tickTimer_->stop();
    flushTimer_->stop();
    active_ = false;
    flushing_ = false;
    remaining_ = 0;
    onReady_ = nullptr;
    auto cancelCb = std::exchange(onCancelled_, nullptr);
    Q_EMIT cancelled();
    if (cancelCb) {
        cancelCb();
    }
}

bool CountdownController::isActive() const
{
    return active_;
}

int CountdownController::remainingSeconds() const
{
    return remaining_;
}

QString CountdownController::label() const
{
    return label_;
}

void CountdownController::onTick()
{
    if (!active_ || flushing_) {
        return;
    }
    --remaining_;
    if (remaining_ > 0) {
        Q_EMIT tick(remaining_);
        return;
    }
    tickTimer_->stop();
    flushing_ = true;
    Q_EMIT hidingForCapture();
    if (hideFlushDelayMs_ <= 0) {
        onFlushDone();
    } else {
        flushTimer_->start(hideFlushDelayMs_);
    }
}

void CountdownController::onFlushDone()
{
    if (!active_) {
        return;
    }
    active_ = false;
    flushing_ = false;
    onCancelled_ = nullptr;
    auto readyCb = std::exchange(onReady_, nullptr);
    Q_EMIT completed();
    if (readyCb) {
        readyCb();
    }
}

CountdownBanner::CountdownBanner(CountdownController *controller, QWidget *parent)
    : QWidget(parent)
    , controller_(controller)
{
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setContentsMargins(bannerShadow + 8, bannerShadow + 4, bannerShadow + 8, bannerShadow + 6);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(8);

    auto *iconLabel = new QLabel(this);
    iconLabel->setPixmap(QIcon::fromTheme(QStringLiteral("chronometer")).pixmap(18, 18));
    layout->addWidget(iconLabel);

    textLabel_ = new QLabel(this);
    QFont bold = textLabel_->font();
    bold.setBold(true);
    textLabel_->setFont(bold);
    layout->addWidget(textLabel_);

    cancelButton_ = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-cancel")), tr("Cancel"), this);
    cancelButton_->setFlat(true);
    cancelButton_->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(cancelButton_);

    connect(cancelButton_, &QPushButton::clicked, controller_, &CountdownController::cancel);
    connect(controller_, &CountdownController::started, this, [this](int secs, const QString &label) {
        updateDisplay(secs, label);
        reposition();
        show();
        raise();
    });
    connect(controller_, &CountdownController::tick, this, [this](int secs) {
        updateDisplay(secs, currentLabel_);
        reposition();
    });
    connect(controller_, &CountdownController::hidingForCapture, this, &QWidget::hide);
    connect(controller_, &CountdownController::cancelled, this, &QWidget::hide);
    connect(controller_, &CountdownController::completed, this, &QWidget::hide);
}

void CountdownBanner::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF body = QRectF(rect()).adjusted(bannerShadow, bannerShadow, -bannerShadow, -bannerShadow - 2);
    for (int i = bannerShadow; i > 0; --i) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 6));
        painter.drawRoundedRect(body.adjusted(-i, -i + 2, i, i + 2), bannerRadius + i, bannerRadius + i);
    }
    painter.setBrush(palette().color(QPalette::Window));
    QColor border = palette().color(QPalette::WindowText);
    border.setAlpha(45);
    painter.setPen(QPen(border, 1));
    painter.drawRoundedRect(body.adjusted(0.5, 0.5, -0.5, -0.5), bannerRadius, bannerRadius);
}

void CountdownBanner::updateDisplay(int seconds, const QString &label)
{
    currentLabel_ = label;
    if (label.trimmed().isEmpty()) {
        textLabel_->setText(tr("Capturing in %1 s…").arg(seconds));
    } else {
        textLabel_->setText(tr("Capturing %1 in %2 s…").arg(label, QString::number(seconds)));
    }
    adjustSize();
}

void CountdownBanner::reposition()
{
    const QPoint cursorPos = QCursor::pos();
    QScreen *screen = QGuiApplication::screenAt(cursorPos);
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    if (!screen) {
        return;
    }
    const QRect geom = screen->availableGeometry();
    const int x = geom.center().x() - width() / 2;
    const int y = geom.top() + bannerTopOffset;
    move(std::clamp(x, geom.left() + 8, std::max(geom.left() + 8, geom.right() - width() - 8)), y);
}

} // namespace glimply
