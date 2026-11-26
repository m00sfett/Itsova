#include "pinmanager.h"

#include "notifyconfig.h"
#include "kwinpinscript.h"
#include "pinwindow.h"

#include <KNotification>

#include <QGuiApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QWindow>

#include <utility>

namespace glimply
{
namespace
{

constexpr int saveDelayMs = 500;
constexpr int undoWindowMs = 10'000;
// Upper bound for the compositor to confirm the transparent pin frames (the former fixed wait).
constexpr int captureHideTimeoutMs = 250;
// Safety net: pins never stay invisible because a capture path forgot endCapture().
constexpr int captureWatchdogMs = 60'000;

const QString controlService = QStringLiteral("io.github.m00sfett.glimply");
const QString controlPath = QStringLiteral("/Control");
const QString controlInterface = QStringLiteral("io.github.m00sfett.glimply.Control");

// Waits until every given window has had its last commit consumed by the compositor: a requested
// update is delivered on the Wayland frame callback of the frame that showed the transparent pin.
class FrameWaiter : public QObject
{
public:
    FrameWaiter(const QList<QWindow *> &windows, QObject *context, std::function<void()> ready)
        : QObject(context)
        , ready_(std::move(ready))
    {
        for (QWindow *window : windows) {
            pending_.append(window);
            window->installEventFilter(this);
            window->requestUpdate();
        }
        QTimer::singleShot(captureHideTimeoutMs, this, [this] { fire(); });
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::UpdateRequest) {
            pending_.removeAll(watched);
            pending_.removeAll(nullptr);
            if (pending_.isEmpty()) {
                // After the window has handled this update itself.
                QTimer::singleShot(0, this, [this] { fire(); });
            }
        }
        return false;
    }

private:
    void fire()
    {
        if (!ready_) {
            return;
        }
        for (const QPointer<QObject> &window : std::as_const(pending_)) {
            if (window) {
                window->removeEventFilter(this);
            }
        }
        auto ready = std::exchange(ready_, {});
        deleteLater();
        ready();
    }

    QList<QPointer<QObject>> pending_;
    std::function<void()> ready_;
};

KNotification *makeNotification(const QString &eventId)
{
    const bool haveEvents = haveNotifyEvents();
    auto *notification = new KNotification(haveEvents ? eventId : QStringLiteral("notification"));
    if (haveEvents) {
        notification->setComponentName(notifyComponentName());
    }
    return notification;
}

} // namespace

PinManager::PinManager(QObject *parent)
    : QObject(parent)
    , saveTimer_(new QTimer(this))
    , captureWatchdog_(new QTimer(this))
{
    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(saveDelayMs);
    connect(saveTimer_, &QTimer::timeout, this, [this] {
        QString error;
        if (!store_.save(&error)) {
            qWarning("glimply: could not save pins: %s", qPrintable(error));
        }
    });
    captureWatchdog_->setSingleShot(true);
    captureWatchdog_->setInterval(captureWatchdogMs);
    connect(captureWatchdog_, &QTimer::timeout, this, [this] {
        if (captureDepth_ > 0) {
            qWarning("glimply: pins were hidden for a capture for too long; showing them again");
            captureDepth_ = 1;
            endCapture();
        }
    });
}

PinManager::~PinManager()
{
    // The owner is being destroyed; nobody may react to signals from here on.
    blockSignals(true);
    // Pending removals become final; the KWin scripts go with the windows.
    const QStringList pending = pendingRemovals_.keys();
    for (const QString &id : pending) {
        finalizeRemove(id);
    }
    if (saveTimer_->isActive()) {
        store_.save();
    }
    for (auto it = windows_.cbegin(); it != windows_.cend(); ++it) {
        unloadKWinPinScript(it.key());
        delete it.value().data();
    }
}

void PinManager::restore()
{
    QString error;
    if (!store_.load(&error)) {
        qWarning("glimply: ignoring unreadable pin list: %s", qPrintable(error));
        return;
    }
    for (const PinRecord &record : store_.pins()) {
        const QImage image(store_.imagePath(record.id));
        if (!image.isNull()) {
            createWindow(record, image);
        }
    }
    Q_EMIT changed();
}

QList<PinOutput> PinManager::outputs() const
{
    QList<PinOutput> result;
    if (QScreen *primary = QGuiApplication::primaryScreen()) {
        result.append({primary->name(), primary->geometry()});
    }
    for (QScreen *screen : QGuiApplication::screens()) {
        if (screen != QGuiApplication::primaryScreen()) {
            result.append({screen->name(), screen->geometry()});
        }
    }
    return result;
}

void PinManager::pinImage(const QImage &image, const QRect &logicalRect)
{
    if (image.isNull()) {
        return;
    }
    const QList<PinOutput> outs = outputs();
    QPoint topLeft = logicalRect.topLeft();
    if (!logicalRect.isValid() && !outs.isEmpty()) {
        const qreal dpr = QGuiApplication::primaryScreen() ? QGuiApplication::primaryScreen()->devicePixelRatio() : 1.0;
        const QSize size(int(image.width() / dpr), int(image.height() / dpr));
        topLeft = outs.first().geometry.center() - QPoint(size.width() / 2, size.height() / 2);
    }
    const auto placement = pinOutputAt(topLeft, outs);
    QString error;
    const std::optional<PinRecord> record =
        store_.add(image, placement ? placement->first : QString(), placement ? placement->second : topLeft, &error);
    if (!record) {
        qWarning("glimply: could not store pin: %s", qPrintable(error));
        return;
    }
    // A new pin is meant to be seen, even when the user hid the others before.
    if (store_.hidden()) {
        setPinsShown(true);
    }
    scheduleSave();
    createWindow(*record, image);
    Q_EMIT changed();
}

void PinManager::createWindow(const PinRecord &record, const QImage &image)
{
    auto *window = new PinWindow(record.id, image, record.zoom, record.opacity);
    windows_.insert(record.id, window);
    const QString id = record.id;
    connect(window, &PinWindow::zoomChanged, this, [this, id](double zoom) {
        if (std::optional<PinRecord> rec = store_.pin(id)) {
            rec->zoom = zoom;
            store_.update(*rec);
            scheduleSave();
        }
    });
    connect(window, &PinWindow::opacityChanged, this, [this, id](double opacity) {
        if (std::optional<PinRecord> rec = store_.pin(id)) {
            rec->opacity = opacity;
            store_.update(*rec);
            scheduleSave();
        }
    });
    connect(window, &PinWindow::copyRequested, this, [this, window] { Q_EMIT copyRequested(window->image()); });
    connect(window, &PinWindow::editRequested, this, [this, window] { Q_EMIT editRequested(window->image()); });
    connect(window, &PinWindow::saveAsRequested, this, [this, window] { Q_EMIT saveAsRequested(window->image()); });
    connect(window, &PinWindow::removeRequested, this, [this, id] { requestRemove(id); });
    window->setCaptureTransparent(captureDepth_ > 0);
    if (!store_.hidden()) {
        window->show();
        placeWindow(window, record);
    }
}

void PinManager::placeWindow(PinWindow *window, const PinRecord &record)
{
    const QPoint pos = resolvePinPosition(record, outputs(), window->size());
    applyKWinPinScript({record.id, window->kwinCaption(), QRect(pos, window->size()), controlService, controlPath,
                        controlInterface});
}

int PinManager::count() const
{
    return int(store_.pins().size() - pendingRemovals_.size());
}

bool PinManager::pinsShown() const
{
    return !store_.hidden();
}

void PinManager::setPinsShown(bool shown)
{
    if (shown == pinsShown()) {
        return;
    }
    store_.setHidden(!shown);
    for (auto it = windows_.cbegin(); it != windows_.cend(); ++it) {
        PinWindow *window = it.value();
        if (!window || pendingRemovals_.contains(it.key())) {
            continue;
        }
        if (shown) {
            window->show();
            if (std::optional<PinRecord> rec = store_.pin(it.key())) {
                placeWindow(window, *rec); // a re-mapped window is new to KWin
            }
        } else {
            window->hide();
        }
    }
    scheduleSave();
    Q_EMIT changed();
}

void PinManager::removeAllWithConfirmation()
{
    const int n = count();
    if (n == 0) {
        return;
    }
    QMessageBox box(QMessageBox::Warning, tr("Remove All Pins"),
                    tr("Remove %n pin(s) permanently?", nullptr, n),
                    QMessageBox::NoButton);
    box.setInformativeText(tr("Their images are deleted. Captures saved in the capture folder are not affected."));
    QPushButton *remove = box.addButton(tr("Remove Pins"), QMessageBox::DestructiveRole);
    QPushButton *cancel = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(cancel);
    box.exec();
    if (box.clickedButton() != remove) {
        return;
    }
    const QStringList ids = windows_.keys();
    for (const QString &id : ids) {
        finalizeRemove(id);
    }
    // Records whose image failed to load have no window.
    const QList<PinRecord> rest = store_.pins();
    for (const PinRecord &record : rest) {
        store_.remove(record.id);
    }
    store_.save();
    Q_EMIT changed();
}

void PinManager::pinMoved(const QString &id, int x, int y)
{
    if (pendingRemovals_.contains(id)) {
        return;
    }
    std::optional<PinRecord> record = store_.pin(id);
    if (!record) {
        return;
    }
    const auto placement = pinOutputAt(QPoint(x, y), outputs());
    if (!placement) {
        return;
    }
    record->connector = placement->first;
    record->offset = placement->second;
    store_.update(*record);
    scheduleSave();
}

void PinManager::requestRemove(const QString &id)
{
    PinWindow *window = windows_.value(id);
    if (!window || pendingRemovals_.contains(id)) {
        return;
    }
    window->hide();
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(undoWindowMs);
    connect(timer, &QTimer::timeout, this, [this, id] { finalizeRemove(id); });
    pendingRemovals_.insert(id, timer);
    timer->start();

    KNotification *notification = makeNotification(QStringLiteral("pinRemoved"));
    notification->setTitle(tr("Pin removed"));
    notification->setText(tr("The pinned screenshot was removed."));
    notification->setIconName(QStringLiteral("window-pin"));
    KNotificationAction *undo = notification->addAction(tr("Undo"));
    connect(undo, &KNotificationAction::activated, this, [this, id] { undoRemove(id); });
    // Closing the notification early does not shorten the undo time; the timer decides.
    connect(timer, &QObject::destroyed, notification, [notification] { notification->close(); });
    notification->sendEvent();
    Q_EMIT changed();
}

void PinManager::undoRemove(const QString &id)
{
    QTimer *timer = pendingRemovals_.take(id);
    if (!timer) {
        return;
    }
    timer->stop();
    timer->deleteLater();
    PinWindow *window = windows_.value(id);
    std::optional<PinRecord> record = store_.pin(id);
    if (window && record && !store_.hidden()) {
        window->show();
        placeWindow(window, *record);
    }
    Q_EMIT changed();
}

void PinManager::finalizeRemove(const QString &id)
{
    if (QTimer *timer = pendingRemovals_.take(id)) {
        timer->stop();
        timer->deleteLater();
    }
    unloadKWinPinScript(id);
    if (QPointer<PinWindow> window = windows_.take(id)) {
        window->deleteLater();
    }
    if (store_.remove(id)) {
        scheduleSave();
    }
    Q_EMIT changed();
}

void PinManager::scheduleSave()
{
    saveTimer_->start();
}

void PinManager::beginCapture(QObject *context, std::function<void()> ready)
{
    ++captureDepth_;
    captureWatchdog_->start();
    if (captureDepth_ > 1) {
        ready(); // already transparent; the first capture waited for the compositor
        return;
    }
    QList<QWindow *> visible;
    for (auto it = windows_.cbegin(); it != windows_.cend(); ++it) {
        if (PinWindow *window = it.value()) {
            window->setCaptureTransparent(true);
            if (window->isVisible() && window->windowHandle()) {
                visible.append(window->windowHandle());
            }
        }
    }
    if (visible.isEmpty()) {
        ready();
        return;
    }
    new FrameWaiter(visible, context, std::move(ready));
}

void PinManager::endCapture()
{
    if (captureDepth_ == 0) {
        return;
    }
    if (--captureDepth_ > 0) {
        return;
    }
    captureWatchdog_->stop();
    for (auto it = windows_.cbegin(); it != windows_.cend(); ++it) {
        if (PinWindow *window = it.value()) {
            window->setCaptureTransparent(false);
        }
    }
}

} // namespace glimply
