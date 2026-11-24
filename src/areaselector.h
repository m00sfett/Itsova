#pragma once

#include "targets.h"

#include <QImage>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QWidget>

namespace glimply
{

class AreaOverlay;

// Setup-time selection of an area. Shows one overlay per connected monitor with a frozen, dimmed
// picture of the screen. The user can drag across monitor edges; Enter or a double click
// confirms, Escape cancels.
class AreaSelector : public QObject
{
    Q_OBJECT

public:
    explicit AreaSelector(QObject *parent = nullptr);
    ~AreaSelector() override;

    // initialGlobalRect: global logical rectangle for editing an existing area, or invalid.
    void start(const QRect &initialGlobalRect = QRect(), bool showLoupe = true);

    [[nodiscard]] QRect selection() const;
    void setSelection(const QRect &globalRect);
    void clearSelection();
    [[nodiscard]] bool showLoupe() const;
    [[nodiscard]] bool dragLoupeActive() const;
    [[nodiscard]] QPoint dragCursorGlobal() const;
    void setDragLoupe(bool active, const QPoint &globalPos = {});
    [[nodiscard]] QString pillConnector() const;
    [[nodiscard]] QList<ScreenInfo> screens() const;

Q_SIGNALS:
    void selected(const QRect &globalRect);
    void cancelled();

private:
    void finish(bool accepted);

    QList<ScreenInfo> screens_;
    QRect desktopBounds_;
    QList<QRect> screenRects_;
    QRect selection_;
    bool showLoupe_ = true;
    bool dragLoupeActive_ = false;
    QPoint dragCursorGlobal_;
    QList<QPointer<AreaOverlay>> overlays_;
};

class AreaOverlay : public QWidget
{
    Q_OBJECT

public:
    // Standalone single-monitor overlay constructor (used by unit tests).
    AreaOverlay(const QString &connector, const QSize &logicalSize, const QImage &background, const QRect &initial);
    // Multi-monitor overlay constructor managed by AreaSelector.
    AreaOverlay(AreaSelector *selector, const ScreenInfo &screen, const QRect &desktopBounds,
                const QList<QRect> &screenRects, const QImage &background, qreal backgroundScale = 1.0);

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
    void keyReleaseEvent(QKeyEvent *event) override;

private:
    enum class Drag { None, Create, Move, Resize };

    void setSelection(const QRect &rect);
    // Updates the selection for the current drag; Alt draws from the centre, Shift keeps a square.
    void applyDrag(const QPoint &globalPos, Qt::KeyboardModifiers modifiers);
    // Redraws the drag when Shift, Alt, or Ctrl is pressed or released mid-drag.
    bool reapplyModifiers(QKeyEvent *event);
    // Edges of the selection under globalPos: one edge near a side, two near a corner, none elsewhere.
    [[nodiscard]] Qt::Edges edgesAt(const QPoint &globalPos) const;
    void updateCursor(const QPoint &globalPos);
    [[nodiscard]] QPoint toGlobalPoint(const QPoint &localPos) const;
    [[nodiscard]] QRect toLocalRect(const QRect &globalRect) const;

    AreaSelector *selector_ = nullptr;
    QString connector_;
    QRect screenGeometry_;
    QRect desktopBounds_;
    QList<QRect> screenRects_;
    QImage background_;
    qreal backgroundScale_ = 1.0;
    QRect selection_;
    Drag drag_ = Drag::None;
    bool localDragLoupeActive_ = false;
    bool spaceHeld_ = false;
    QPoint anchor_;
    QPoint lastPosition_;
    QRect dragStartSelection_;
    Qt::Edges dragEdges_;
};

} // namespace glimply
