#pragma once

#include <QObject>
#include <QString>
#include <QWidget>

#include <functional>

class QLabel;
class QPushButton;
class QTimer;

namespace itsova
{

// Coordinates a non-blocking pre-capture or retake countdown. Hides any countdown UI strictly
// before invoking the capture callback so zero indicator UI appears in the screenshot.
class CountdownController : public QObject
{
    Q_OBJECT

public:
    explicit CountdownController(QObject *parent = nullptr);

    void setTickIntervalMs(int ms);
    void setHideFlushDelayMs(int ms);

    void start(int seconds,
               const QString &label,
               std::function<void()> onReady,
               std::function<void()> onCancelled = {});
    void cancel();

    [[nodiscard]] bool isActive() const;
    [[nodiscard]] int remainingSeconds() const;
    [[nodiscard]] QString label() const;

Q_SIGNALS:
    void started(int seconds, const QString &label);
    void tick(int remainingSeconds);
    void hidingForCapture();
    void completed();
    void cancelled();

private:
    void onTick();
    void onFlushDone();

    QTimer *tickTimer_ = nullptr;
    QTimer *flushTimer_ = nullptr;
    int tickIntervalMs_ = 1000;
    int hideFlushDelayMs_ = 250;
    int remaining_ = 0;
    bool active_ = false;
    bool flushing_ = false;
    QString label_;
    std::function<void()> onReady_;
    std::function<void()> onCancelled_;
};

// Compact, non-focus-stealing KDE/Breeze banner showing active countdown + Cancel without blocking
// desktop interaction (allowing users to open menus or tooltips before the capture fires).
class CountdownBanner : public QWidget
{
    Q_OBJECT

public:
    explicit CountdownBanner(CountdownController *controller, QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void updateDisplay(int seconds, const QString &label);
    void reposition();

    CountdownController *controller_ = nullptr;
    QLabel *textLabel_ = nullptr;
    QPushButton *cancelButton_ = nullptr;
    QString currentLabel_;
};

} // namespace itsova
