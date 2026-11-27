#include "colorpicker.h"

#include "app.h"
#include "colorformat.h"
#include "pixelloupe.h"
#include "screens.h"

#include <QClipboard>
#include <QCursor>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QWindow>
#include <QtConcurrent>

#include <algorithm>
#include <mutex>
#include <utility>

namespace itsova
{

ColorPickerSession::ColorPickerSession(App *app, QObject *parent)
    : QObject(parent)
    , app_(app)
{
}

ColorPickerSession::~ColorPickerSession()
{
    for (const QPointer<ColorPickerOverlay> &overlay : std::as_const(overlays_)) {
        delete overlay.data();
    }
}

void ColorPickerSession::start()
{
    screens_ = currentScreens();
    if (screens_.isEmpty()) {
        finished_ = true;
        Q_EMIT cancelled();
        Q_EMIT finished();
        return;
    }

    QStringList connectors;
    for (const ScreenInfo &screen : std::as_const(screens_)) {
        desktopBounds_ = desktopBounds_.united(screen.geometry);
        connectors.append(screen.connector);
    }

    QHash<QString, CaptureResult> captured;
    std::mutex mutex;
    QtConcurrent::blockingMap(connectors, [&](const QString &connector) {
        CaptureResult frame = CaptureService::captureScreen(connector, false);
        const std::lock_guard lock(mutex);
        captured.insert(connector, std::move(frame));
    });
    frames_ = std::move(captured);

    cursorGlobal_ = QCursor::pos();
    if (!desktopBounds_.isEmpty()) {
        cursorGlobal_.setX(std::clamp(cursorGlobal_.x(), desktopBounds_.left(), desktopBounds_.right()));
        cursorGlobal_.setY(std::clamp(cursorGlobal_.y(), desktopBounds_.top(), desktopBounds_.bottom()));
    }

    ColorPickerOverlay *activeOverlay = nullptr;
    for (const ScreenInfo &screen : std::as_const(screens_)) {
        auto *overlay = new ColorPickerOverlay(this, screen);
        overlays_.append(overlay);
        overlay->setAttribute(Qt::WA_DeleteOnClose);
        overlay->winId();
        if (QScreen *qscreen = findScreen(screen.connector)) {
            overlay->windowHandle()->setScreen(qscreen);
            overlay->setGeometry(qscreen->geometry());
        }
        overlay->showFullScreen();
        if (screen.geometry.contains(cursorGlobal_)) {
            activeOverlay = overlay;
        }
    }
    if (!activeOverlay && !overlays_.isEmpty()) {
        activeOverlay = overlays_.first().data();
    }
    if (activeOverlay) {
        activeOverlay->activateWindow();
        activeOverlay->setFocus();
    }
}

App *ColorPickerSession::app() const
{
    return app_;
}

QPoint ColorPickerSession::cursorGlobal() const
{
    return cursorGlobal_;
}

QRect ColorPickerSession::desktopBounds() const
{
    return desktopBounds_;
}

CaptureResult ColorPickerSession::frameFor(const QString &connector) const
{
    return frames_.value(connector);
}

void ColorPickerSession::setCursorGlobal(const QPoint &globalPos)
{
    QPoint clamped = globalPos;
    if (!desktopBounds_.isEmpty()) {
        clamped.setX(std::clamp(clamped.x(), desktopBounds_.left(), desktopBounds_.right()));
        clamped.setY(std::clamp(clamped.y(), desktopBounds_.top(), desktopBounds_.bottom()));
    }
    if (cursorGlobal_ == clamped) {
        return;
    }
    cursorGlobal_ = clamped;
    for (const QPointer<ColorPickerOverlay> &overlay : std::as_const(overlays_)) {
        if (overlay) {
            overlay->update();
        }
    }
}

void ColorPickerSession::cycleFormat()
{
    if (!app_) {
        return;
    }
    app_->store().colorPickerFormat = nextColorFormat(app_->store().colorPickerFormat);
    app_->saveSettings();
    for (const QPointer<ColorPickerOverlay> &overlay : std::as_const(overlays_)) {
        if (overlay) {
            overlay->update();
        }
    }
}

void ColorPickerSession::acceptAt(const QPoint &globalPos)
{
    if (finished_) {
        return;
    }
    setCursorGlobal(globalPos);
    const ScreenInfo *target = nullptr;
    for (const ScreenInfo &screen : std::as_const(screens_)) {
        if (screen.geometry.contains(cursorGlobal_)) {
            target = &screen;
            break;
        }
    }
    if (!target && !screens_.isEmpty()) {
        target = &screens_.first();
    }

    QColor color;
    if (target) {
        const CaptureResult frame = frames_.value(target->connector);
        if (!frame.image.isNull()) {
            const qreal scale = frame.scale > 0.0 ? frame.scale : 1.0;
            const QPoint nativePx =
                logicalToNativePixel(QPointF(cursorGlobal_), QPointF(target->geometry.topLeft()), scale,
                                     frame.image.size());
            color = sampleNativePixel(frame.image, nativePx);
        }
    }

    const ColorFormat fmt = app_ ? app_->store().colorPickerFormat : ColorFormat::Hex;
    const QString formatted = color.isValid() ? formatColor(color, fmt) : QString();
    closeAll();
    if (color.isValid()) {
        if (QGuiApplication::clipboard()) {
            QGuiApplication::clipboard()->setText(formatted);
        }
        Q_EMIT picked(color, formatted);
    } else {
        Q_EMIT cancelled();
    }
    Q_EMIT finished();
}

void ColorPickerSession::cancel()
{
    if (finished_) {
        return;
    }
    closeAll();
    Q_EMIT cancelled();
    Q_EMIT finished();
}

void ColorPickerSession::closeAll()
{
    finished_ = true;
    const QList<QPointer<ColorPickerOverlay>> overlays = std::exchange(overlays_, {});
    for (const QPointer<ColorPickerOverlay> &overlay : overlays) {
        if (overlay) {
            overlay->close();
        }
    }
}

ColorPickerOverlay::ColorPickerOverlay(ColorPickerSession *session, const ScreenInfo &screen)
    : session_(session)
    , screen_(screen)
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setWindowTitle(tr("Pick color on %1").arg(screen.connector));
    setCursor(Qt::CrossCursor);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

bool ColorPickerOverlay::focusNextPrevChild(bool)
{
    // Keep Tab from shifting focus so keyPressEvent can cycle HEX/RGB/HSL/HSV reliably.
    return false;
}

void ColorPickerOverlay::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const QRect area = rect();
    const CaptureResult frame = session_->frameFor(screen_.connector);
    if (!frame.image.isNull()) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(area, frame.image);
    } else {
        painter.fillRect(area, Qt::black);
    }

    const QPoint globalPos = session_->cursorGlobal();
    if (!screen_.geometry.contains(globalPos) || frame.image.isNull()) {
        return;
    }
    const QPoint localCursor = globalPos - screen_.geometry.topLeft();
    const qreal scale = frame.scale > 0.0 ? frame.scale : 1.0;
    const QPoint nativePx =
        logicalToNativePixel(QPointF(localCursor), QPointF(0.0, 0.0), scale, frame.image.size());
    const ColorFormat fmt = session_->app() ? session_->app()->store().colorPickerFormat : ColorFormat::Hex;
    PixelLoupe::paintColorPickerLoupe(painter, localCursor, area, palette(), frame.image, nativePx, fmt, false);
}

void ColorPickerOverlay::mouseMoveEvent(QMouseEvent *event)
{
    session_->setCursorGlobal(event->position().toPoint() + screen_.geometry.topLeft());
}

void ColorPickerOverlay::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton) {
        session_->cancel();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        session_->acceptAt(event->position().toPoint() + screen_.geometry.topLeft());
    }
}

void ColorPickerOverlay::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Tab) {
        session_->cycleFormat();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        session_->cancel();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        session_->acceptAt(session_->cursorGlobal());
        event->accept();
        return;
    }
    const int step = event->modifiers().testFlag(Qt::ShiftModifier) ? 10 : 1;
    QPoint delta;
    switch (event->key()) {
    case Qt::Key_Left:
        delta = {-step, 0};
        break;
    case Qt::Key_Right:
        delta = {step, 0};
        break;
    case Qt::Key_Up:
        delta = {0, -step};
        break;
    case Qt::Key_Down:
        delta = {0, step};
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    session_->setCursorGlobal(session_->cursorGlobal() + delta);
    event->accept();
}

} // namespace itsova
