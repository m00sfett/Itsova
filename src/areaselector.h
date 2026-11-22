#pragma once

#include <QImage>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QWidget>

namespace glimply
{

class AreaOverlay;

// Setup-time selection of an area. Shows one overlay per monitor (or only on the monitor of an
// existing area) with a frozen, dimmed picture of the screen. The user drags a rectangle on any
// of them; Enter or a double click confirms, Escape cancels.
class AreaSelector : public QObject
{
    Q_OBJECT

public:
    explicit AreaSelector(QObject *parent = nullptr);
    ~AreaSelector() override;

    // connector empty: every connected monitor. initial: monitor-local logical rectangle.
    void start(const QString &connector = QString(), const QRect &initial = QRect());

Q_SIGNALS:
    void selected(const QString &connector, const QRect &rect);
    void cancelled();

private:
    void finish(AreaOverlay *source, bool accepted);

    QList<QPointer<AreaOverlay>> overlays_;
};

class AreaOverlay : public QWidget
{
    Q_OBJECT

public:
    AreaOverlay(const QString &connector, const QSize &logicalSize, const QImage &background, const QRect &initial);

    [[nodiscard]] QString connector() const;
    [[nodiscard]] QRect selection() const;
    void clearSelection();

Q_SIGNALS:
    void selectionStarted();
    void accepted();
    void rejected();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    enum class Drag { None, Create, Move, Resize };

    void setSelection(const QRect &rect);
    // Edges of the selection under pos: one edge near a side, two near a corner, none elsewhere.
    [[nodiscard]] Qt::Edges edgesAt(const QPoint &pos) const;
    void updateCursor(const QPoint &pos);

    QString connector_;
    QSize logicalSize_;
    QImage background_;
    QRect selection_;
    Drag drag_ = Drag::None;
    QPoint anchor_;
    QRect dragStartSelection_;
    Qt::Edges dragEdges_;
};

} // namespace glimply
