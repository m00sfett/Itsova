#pragma once

#include <QImage>
#include <QPoint>
#include <QString>
#include <QWidget>

class QTimer;

namespace glimply
{

// Borderless reference window showing one pinned screenshot (#28). It paints the image at native
// device pixels times the zoom, with its own opacity (Wayland has no window opacity for clients).
// Placement and keep-above are done by KWin (see kwinpinscript.h).
class PinWindow : public QWidget
{
    Q_OBJECT

public:
    PinWindow(QString pinId, QImage image, double zoom, double opacity, QWidget *parent = nullptr);

    [[nodiscard]] QString pinId() const;
    [[nodiscard]] QString kwinCaption() const;
    [[nodiscard]] const QImage &image() const;
    [[nodiscard]] double zoom() const;
    [[nodiscard]] double opacity() const;
    void setZoom(double zoom);
    void setOpacityLevel(double opacity);
    // Transparent while a capture runs, so the pin never ends up in a screenshot (Q11).
    void setCaptureTransparent(bool transparent);
    // Logical size for the current zoom on a screen with the given device pixel ratio.
    [[nodiscard]] QSize logicalSizeFor(qreal devicePixelRatio) const;

Q_SIGNALS:
    void zoomChanged(double zoom);
    void opacityChanged(double opacity);
    void copyRequested();
    void editRequested();
    void saveAsRequested();
    void removeRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void applySize();
    void showReadout(const QString &text);

    QString pinId_;
    QImage image_;
    double zoom_;
    double opacity_;
    bool hovered_ = false;
    bool captureTransparent_ = false;
    QPoint pressPos_;
    bool pressed_ = false;
    QString readout_;
    QTimer *readoutTimer_;
};

} // namespace glimply
