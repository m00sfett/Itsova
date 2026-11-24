#pragma once

#include "captureservice.h"
#include "targets.h"

#include <QColor>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QWidget>

namespace glimply
{

class App;
class ColorPickerOverlay;

// Standalone color picker session capturing all monitors natively (without dimming) and
// displaying a 15x15 nearest-neighbor loupe. Left-click / Enter copies the formatted color to the
// clipboard; Tab cycles HEX/RGB/HSL/HSV; Escape / right-click cancels.
class ColorPickerSession : public QObject
{
    Q_OBJECT

public:
    explicit ColorPickerSession(App *app, QObject *parent = nullptr);
    ~ColorPickerSession() override;

    void start();

    [[nodiscard]] App *app() const;
    [[nodiscard]] QPoint cursorGlobal() const;
    [[nodiscard]] QRect desktopBounds() const;
    [[nodiscard]] CaptureResult frameFor(const QString &connector) const;

    void setCursorGlobal(const QPoint &globalPos);
    void cycleFormat();
    void acceptAt(const QPoint &globalPos);
    void cancel();

Q_SIGNALS:
    void picked(const QColor &color, const QString &formatted);
    void cancelled();
    void finished();

private:
    void closeAll();

    App *app_ = nullptr;
    QList<ScreenInfo> screens_;
    QRect desktopBounds_;
    QHash<QString, CaptureResult> frames_;
    QPoint cursorGlobal_;
    bool finished_ = false;
    QList<QPointer<ColorPickerOverlay>> overlays_;
};

class ColorPickerOverlay : public QWidget
{
    Q_OBJECT

public:
    ColorPickerOverlay(ColorPickerSession *session, const ScreenInfo &screen);

protected:
    bool focusNextPrevChild(bool next) override;
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    ColorPickerSession *session_ = nullptr;
    ScreenInfo screen_;
};

} // namespace glimply
