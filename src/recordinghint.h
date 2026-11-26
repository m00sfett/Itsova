#pragma once

#include <QRect>
#include <QString>
#include <QWidget>

class QScreen;
class QTimer;

namespace glimply
{

// A short hint for a recording state change (#125): started, stopped, paused, resumed, failed. A
// layer-shell surface placed exactly where planRecordingHint() says, so it stays out of the
// recorded picture; it takes no keyboard focus. A click emits clicked() (the stop hint opens the
// file); hovering keeps it.
class RecordingHint : public QWidget
{
    Q_OBJECT

public:
    explicit RecordingHint(QWidget *parent = nullptr);

    void setContent(const QString &iconName, const QString &title, const QString &detail, bool clickable);
    // Shows it at rect (global logical coordinates) on screen for durationMs.
    void showAt(QScreen *screen, const QRect &rect, int durationMs);
    void dismiss();

Q_SIGNALS:
    void clicked();
    void dismissed();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QString iconName_;
    QString title_;
    QString detail_;
    bool clickable_ = false;
    QTimer *timer_;
};

} // namespace glimply
