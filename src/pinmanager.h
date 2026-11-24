#pragma once

#include "pinstore.h"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QRect>

#include <functional>
#include <memory>

class QTimer;

namespace glimply
{

class PinWindow;

// Owns all pinned screenshots (#28): persistence, KWin placement, hide/show, removal with undo,
// and keeping pins out of captures.
class PinManager : public QObject
{
    Q_OBJECT

public:
    explicit PinManager(QObject *parent = nullptr);
    ~PinManager() override;

    // Restores the stored pins (call once the D-Bus service is registered).
    void restore();

    // Pins a native-resolution image. logicalRect is where the content was on screen (global,
    // logical); when empty the pin is centred on the primary monitor.
    void pinImage(const QImage &image, const QRect &logicalRect = {});
    [[nodiscard]] int count() const;
    [[nodiscard]] bool pinsShown() const;
    void setPinsShown(bool shown);
    // Asks for confirmation, then removes every pin permanently.
    void removeAllWithConfirmation();
    // Called by the KWin script when the user moved a pin.
    void pinMoved(const QString &id, int x, int y);

    // Captures: beginCapture() makes the pins transparent and calls ready once the compositor shows
    // the transparent frames of all visible pins (Wayland frame callback), at the latest after a
    // fallback timeout; synchronously when nothing had to be hidden or the pins already are. ready
    // is dropped when context is destroyed first. Every call needs a matching endCapture() (#67).
    void beginCapture(QObject *context, std::function<void()> ready);
    void endCapture();

Q_SIGNALS:
    void changed();
    void copyRequested(const QImage &image);
    void editRequested(const QImage &image);
    void saveAsRequested(const QImage &image);

private:
    void createWindow(const PinRecord &record, const QImage &image);
    void placeWindow(PinWindow *window, const PinRecord &record);
    void requestRemove(const QString &id);
    void finalizeRemove(const QString &id);
    void undoRemove(const QString &id);
    void scheduleSave();
    [[nodiscard]] QList<PinOutput> outputs() const;

    PinStore store_;
    QHash<QString, QPointer<PinWindow>> windows_;
    QHash<QString, QTimer *> pendingRemovals_;
    QTimer *saveTimer_;
    QTimer *captureWatchdog_;
    int captureDepth_ = 0;
};

} // namespace glimply
