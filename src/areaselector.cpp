#include "areaselector.h"

#include "captureservice.h"
#include "screens.h"
#include "selectiongeometry.h"
#include "triggerplan.h"

#include <QCursor>
#include <QGuiApplication>
#include <QHash>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QWindow>
#include <QtConcurrent>

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <utility>

namespace glimply
{
namespace
{

constexpr int minimumAreaSize = 4;
constexpr qreal handleRadius = 5.0;
// Grab zone around an edge: generous on large selections, smaller on tiny ones so the inside
// stays reachable for moving.
constexpr int maxGrabDistance = 10;
constexpr int minGrabDistance = 4;
// Corners grab a little further than sides because two edges move at once.
constexpr int cornerBonus = 4;
// Below this side length the midpoint handles and the dotted guides would crowd the selection.
constexpr int detailMinimumSide = 40;

} // namespace

AreaSelector::AreaSelector(QObject *parent)
    : QObject(parent)
{
}

AreaSelector::~AreaSelector()
{
    for (const QPointer<AreaOverlay> &overlay : std::as_const(overlays_)) {
        delete overlay.data();
    }
}

void AreaSelector::start(const QRect &initialGlobalRect)
{
    screens_ = currentScreens();
    if (screens_.isEmpty()) {
        Q_EMIT cancelled();
        return;
    }

    desktopBounds_ = QRect();
    screenRects_.clear();
    QStringList connectors;
    for (const ScreenInfo &screen : std::as_const(screens_)) {
        desktopBounds_ = desktopBounds_.united(screen.geometry);
        screenRects_.append(screen.geometry);
        connectors.append(screen.connector);
    }

    if (initialGlobalRect.isValid()) {
        selection_ = clampToBounds(initialGlobalRect, desktopBounds_);
    } else {
        selection_ = QRect();
    }

    QHash<QString, CaptureResult> frames;
    std::mutex mutex;
    QtConcurrent::blockingMap(connectors, [&](const QString &connector) {
        CaptureResult frame = CaptureService::captureScreen(connector);
        const std::lock_guard lock(mutex);
        frames.insert(connector, std::move(frame));
    });

    for (const ScreenInfo &screen : std::as_const(screens_)) {
        const QImage bg = frames.value(screen.connector).image;
        auto *overlay = new AreaOverlay(this, screen, desktopBounds_, screenRects_, bg);
        overlays_.append(overlay);
        connect(overlay, &AreaOverlay::accepted, this, [this] { finish(true); });
        connect(overlay, &AreaOverlay::rejected, this, [this] { finish(false); });

        overlay->setAttribute(Qt::WA_DeleteOnClose);
        overlay->winId();
        if (QScreen *qscreen = findScreen(screen.connector)) {
            overlay->windowHandle()->setScreen(qscreen);
            overlay->setGeometry(qscreen->geometry());
        }
        overlay->showFullScreen();
    }

    const QPoint focusPoint = selection_.isValid() ? selection_.center() : QCursor::pos();
    AreaOverlay *toActivate = overlays_.first().data();
    for (const QPointer<AreaOverlay> &overlay : std::as_const(overlays_)) {
        if (overlay && overlay->geometry().contains(focusPoint)) {
            toActivate = overlay.data();
            break;
        }
    }
    if (toActivate) {
        toActivate->activateWindow();
    }
}

QRect AreaSelector::selection() const
{
    return selection_;
}

void AreaSelector::setSelection(const QRect &globalRect)
{
    selection_ = globalRect.isValid() ? clampToBounds(globalRect, desktopBounds_) : QRect();
    for (const QPointer<AreaOverlay> &overlay : std::as_const(overlays_)) {
        if (overlay) {
            overlay->update();
        }
    }
}

void AreaSelector::clearSelection()
{
    setSelection(QRect());
}

QString AreaSelector::pillConnector() const
{
    if (screens_.isEmpty()) {
        return {};
    }
    if (!selection_.isValid()) {
        return screens_.first().connector;
    }
    const QPoint anchor = selection_.center();
    for (const ScreenInfo &screen : screens_) {
        if (screen.geometry.contains(anchor)) {
            return screen.connector;
        }
    }
    const QList<ScreenInfo> hit = intersectingScreens(screens_, selection_);
    return hit.isEmpty() ? screens_.first().connector : hit.first().connector;
}

QList<ScreenInfo> AreaSelector::screens() const
{
    return screens_;
}

void AreaSelector::finish(bool accepted)
{
    const QRect rect = selection_;
    const QList<QPointer<AreaOverlay>> overlays = std::exchange(overlays_, {});
    for (const QPointer<AreaOverlay> &overlay : overlays) {
        if (overlay) {
            overlay->close();
        }
    }
    if (accepted && rect.width() >= minimumAreaSize && rect.height() >= minimumAreaSize) {
        Q_EMIT selected(rect);
    } else {
        Q_EMIT cancelled();
    }
}

AreaOverlay::AreaOverlay(const QString &connector, const QSize &logicalSize, const QImage &background,
                         const QRect &initial)
    : connector_(connector)
    , screenGeometry_(QPoint(), logicalSize)
    , desktopBounds_(QPoint(), logicalSize)
    , screenRects_{desktopBounds_}
    , background_(background)
    , selection_(initial.isValid() ? clampToBounds(initial, desktopBounds_) : QRect())
    , lastPosition_(screenGeometry_.topLeft())
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setWindowTitle(tr("Select an area on %1").arg(connector));
    if (background_.isNull()) {
        setAttribute(Qt::WA_TranslucentBackground);
    }
    setCursor(Qt::CrossCursor);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

AreaOverlay::AreaOverlay(AreaSelector *selector, const ScreenInfo &screen, const QRect &desktopBounds,
                         const QList<QRect> &screenRects, const QImage &background)
    : selector_(selector)
    , connector_(screen.connector)
    , screenGeometry_(screen.geometry)
    , desktopBounds_(desktopBounds)
    , screenRects_(screenRects)
    , background_(background)
    , lastPosition_(screen.geometry.topLeft())
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setWindowTitle(tr("Select an area on %1").arg(screen.connector));
    if (background_.isNull()) {
        setAttribute(Qt::WA_TranslucentBackground);
    }
    setCursor(Qt::CrossCursor);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

QString AreaOverlay::connector() const
{
    return connector_;
}

QRect AreaOverlay::selection() const
{
    return selector_ ? selector_->selection() : selection_;
}

void AreaOverlay::clearSelection()
{
    if (selector_) {
        selector_->clearSelection();
    } else {
        selection_ = QRect();
        update();
    }
}

void AreaOverlay::setSelection(const QRect &rect)
{
    if (selector_) {
        selector_->setSelection(rect);
    } else {
        selection_ = rect.isValid() ? clampToBounds(rect, desktopBounds_) : QRect();
        update();
    }
}

QPoint AreaOverlay::toGlobalPoint(const QPoint &localPos) const
{
    return localPos + screenGeometry_.topLeft();
}

QRect AreaOverlay::toLocalRect(const QRect &globalRect) const
{
    return globalRect.translated(-screenGeometry_.topLeft());
}

void AreaOverlay::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const QRect area = rect();
    if (!background_.isNull()) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(area, background_);
    }

    const QRect sel = selection();
    const QRect localSel = sel.isValid() ? toLocalRect(sel) : QRect();
    const QRect visibleHole = localSel.isValid() ? localSel.intersected(area) : QRect();

    // Dim everything outside the selection.
    QPainterPath shade;
    shade.addRect(area);
    if (!visibleHole.isEmpty()) {
        shade.addRect(visibleHole);
    }
    painter.fillPath(shade, QColor(0, 0, 0, background_.isNull() ? 110 : 140));

    const QPalette pal = palette();
    const QColor accent = pal.color(QPalette::Highlight);
    if (sel.isValid() && localSel.intersects(area)) {
        painter.setPen(QPen(accent, 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(localSel.adjusted(0, 0, -1, -1));

        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF frame(localSel);
        const QPointF centre(frame.x() + frame.width() / 2.0, frame.y() + frame.height() / 2.0);
        const bool roomy = sel.width() >= detailMinimumSide && sel.height() >= detailMinimumSide;

        // Centre crosshair: a short solid cross, continued as dotted guides to the selection's edges.
        // A dark halo under the light lines keeps them visible on bright and dark content alike.
        constexpr qreal arm = 8.0;
        const auto drawLines = [&](const QList<QLineF> &lines, const QColor &light, Qt::PenStyle style) {
            QPen halo(QColor(0, 0, 0, 90), 3);
            QPen line(light, 1);
            if (style == Qt::CustomDashLine) {
                halo.setDashPattern({1.0, 1.5});
                line.setDashPattern({3.0, 4.0});
            }
            painter.setPen(halo);
            painter.drawLines(lines);
            painter.setPen(line);
            painter.drawLines(lines);
        };
        if (roomy) {
            drawLines({QLineF(frame.left(), centre.y(), centre.x() - arm, centre.y()),
                       QLineF(centre.x() + arm, centre.y(), frame.right(), centre.y()),
                       QLineF(centre.x(), frame.top(), centre.x(), centre.y() - arm),
                       QLineF(centre.x(), centre.y() + arm, centre.x(), frame.bottom())},
                      QColor(255, 255, 255, 110), Qt::CustomDashLine);
        }
        drawLines({QLineF(centre.x() - arm, centre.y(), centre.x() + arm, centre.y()),
                   QLineF(centre.x(), centre.y() - arm, centre.x(), centre.y() + arm)},
                  QColor(255, 255, 255, 180), Qt::SolidLine);

        // Grab handles: corners always, side midpoints when there is room.
        QList<QPointF> handles{frame.topLeft(), frame.topRight(), frame.bottomLeft(), frame.bottomRight()};
        if (roomy) {
            handles << QPointF(centre.x(), frame.top()) << QPointF(centre.x(), frame.bottom())
                    << QPointF(frame.left(), centre.y()) << QPointF(frame.right(), centre.y());
        }
        painter.setPen(QPen(QColor(255, 255, 255, 235), 1.5));
        painter.setBrush(accent);
        for (const QPointF &handle : std::as_const(handles)) {
            painter.drawEllipse(handle, handleRadius, handleRadius);
        }
        painter.setRenderHint(QPainter::Antialiasing, false);
    }

    // Labels sit on window-coloured pills so they stay readable on any screen content.
    QFont font = painter.font();
    const auto pill = [&](const QString &text, QPoint anchor, bool below) {
        const QFontMetrics metrics(font);
        const QSize textSize = metrics.size(Qt::TextSingleLine, text);
        QRect box(QPoint(), textSize + QSize(20, 10));
        box.moveCenter(anchor);
        if (below) {
            box.moveTop(anchor.y());
        } else {
            box.moveBottom(anchor.y());
        }
        box = clampToBounds(box, area);
        painter.setPen(Qt::NoPen);
        painter.setBrush(pal.color(QPalette::Window));
        painter.drawRoundedRect(box, 5, 5);
        painter.setPen(pal.color(QPalette::WindowText));
        painter.drawText(box, Qt::AlignCenter, text);
    };

    painter.setFont(font);
    pill(sel.isValid()
             ? tr("Drag inside to move, edges or corners to resize. Shift: square, Alt: from centre, Space while "
                  "dragging: reposition. Enter confirms, Escape cancels.")
             : tr("Drag to select an area across one or more monitors. Shift: square, Alt: from centre, Space while "
                  "dragging: reposition. Escape cancels."),
         QPoint(area.center().x(), 24), true);

    if (sel.isValid() && (!selector_ || selector_->pillConnector() == connector_)) {
        QRect shown = sel;
        if (selector_) {
            const QList<ScreenInfo> hit = intersectingScreens(selector_->screens(), sel);
            if (hit.size() == 1 && hit.first().geometry.contains(sel)) {
                shown = sel.translated(-hit.first().geometry.topLeft());
            }
        }
        const QString size =
            tr("%1 × %2 at %3, %4").arg(shown.width()).arg(shown.height()).arg(shown.x()).arg(shown.y());
        const QRect anchor = visibleHole.isEmpty() ? localSel : visibleHole;
        const bool roomBelow = anchor.bottom() + 40 < area.height();
        pill(size, roomBelow ? QPoint(anchor.center().x(), anchor.bottom() + 8)
                             : QPoint(anchor.center().x(), anchor.top() - 8),
             roomBelow);
    }
}

void AreaOverlay::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton) {
        Q_EMIT rejected();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const QPoint globalPos = toGlobalPoint(event->position().toPoint());
    anchor_ = globalPos;
    lastPosition_ = globalPos;
    spaceHeld_ = false;
    const QRect sel = selection();
    dragStartSelection_ = sel;
    if (const Qt::Edges edges = edgesAt(globalPos); edges) {
        drag_ = Drag::Resize;
        dragEdges_ = edges;
    } else if (sel.contains(globalPos)) {
        drag_ = Drag::Move;
    } else {
        drag_ = Drag::Create;
        Q_EMIT selectionStarted();
        setSelection(QRect(anchor_, QSize(1, 1)));
    }
}

void AreaOverlay::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint globalPos = toGlobalPoint(event->position().toPoint());
    const QPoint step = globalPos - lastPosition_;
    lastPosition_ = globalPos;
    if (drag_ == Drag::None) {
        updateCursor(globalPos);
    } else {
        if (spaceHeld_ && (drag_ == Drag::Create || drag_ == Drag::Resize)) {
            anchor_.setX(std::clamp(anchor_.x() + step.x(), desktopBounds_.left(),
                                    desktopBounds_.left() + desktopBounds_.width()));
            anchor_.setY(std::clamp(anchor_.y() + step.y(), desktopBounds_.top(),
                                    desktopBounds_.top() + desktopBounds_.height()));
            if (drag_ == Drag::Resize) {
                dragStartSelection_ = clampToBounds(dragStartSelection_.translated(step), desktopBounds_);
            }
        }
        applyDrag(globalPos, event->modifiers());
    }
}

void AreaOverlay::applyDrag(const QPoint &globalPos, Qt::KeyboardModifiers modifiers)
{
    const SelectionConstraints constraints{modifiers.testFlag(Qt::AltModifier),
                                           modifiers.testFlag(Qt::ShiftModifier)};
    if (drag_ == Drag::Create) {
        QPoint effectivePos = globalPos;
        if (!spaceHeld_) {
            const QPoint snapped = snapPointToMonitorEdges(globalPos, screenRects_);
            if (std::abs(snapped.x() - anchor_.x()) >= minimumAreaSize) {
                effectivePos.setX(snapped.x());
            }
            if (std::abs(snapped.y() - anchor_.y()) >= minimumAreaSize) {
                effectivePos.setY(snapped.y());
            }
        }
        setSelection(drawnSelection(anchor_, effectivePos, constraints, desktopBounds_));
    } else if (drag_ == Drag::Move) {
        QRect moved = clampToBounds(dragStartSelection_.translated(globalPos - anchor_), desktopBounds_);
        if (!spaceHeld_) {
            moved = snapMovedRectToMonitorEdges(moved, screenRects_, desktopBounds_);
        }
        setSelection(moved);
    } else if (drag_ == Drag::Resize) {
        QPoint delta = globalPos - anchor_;
        if (!spaceHeld_) {
            delta = snapResizeDeltaToMonitorEdges(dragStartSelection_, dragEdges_, delta, screenRects_);
        }
        setSelection(resizedSelection(dragStartSelection_, dragEdges_, delta, constraints, desktopBounds_));
    }
}

Qt::Edges AreaOverlay::edgesAt(const QPoint &globalPos) const
{
    const QRect sel = selection();
    if (!sel.isValid()) {
        return {};
    }
    const int grab = std::clamp(std::min(sel.width(), sel.height()) / 4, minGrabDistance, maxGrabDistance);
    const int left = sel.left();
    const int right = sel.left() + sel.width();
    const int top = sel.top();
    const int bottom = sel.top() + sel.height();
    const int x = globalPos.x();
    const int y = globalPos.y();

    // Corners first: aiming roughly at a corner moves both of its edges.
    const int corner = grab + cornerBonus;
    const bool nearLeft = std::abs(x - left) <= corner;
    const bool nearRight = std::abs(x - right) <= corner;
    const bool nearTop = std::abs(y - top) <= corner;
    const bool nearBottom = std::abs(y - bottom) <= corner;
    if ((nearLeft || nearRight) && (nearTop || nearBottom)) {
        // On tiny selections both sides can be in reach; take the closer one.
        const Qt::Edge horizontal = std::abs(x - left) <= std::abs(x - right) ? Qt::LeftEdge : Qt::RightEdge;
        const Qt::Edge vertical = std::abs(y - top) <= std::abs(y - bottom) ? Qt::TopEdge : Qt::BottomEdge;
        return Qt::Edges(horizontal) | vertical;
    }

    const bool withinY = y >= top - grab && y <= bottom + grab;
    const bool withinX = x >= left - grab && x <= right + grab;
    if (withinY && std::abs(x - left) <= grab) {
        return Qt::LeftEdge;
    }
    if (withinY && std::abs(x - right) <= grab) {
        return Qt::RightEdge;
    }
    if (withinX && std::abs(y - top) <= grab) {
        return Qt::TopEdge;
    }
    if (withinX && std::abs(y - bottom) <= grab) {
        return Qt::BottomEdge;
    }
    return {};
}

void AreaOverlay::updateCursor(const QPoint &globalPos)
{
    const Qt::Edges edges = edgesAt(globalPos);
    if (edges == (Qt::LeftEdge | Qt::TopEdge) || edges == (Qt::RightEdge | Qt::BottomEdge)) {
        setCursor(Qt::SizeFDiagCursor);
    } else if (edges == (Qt::RightEdge | Qt::TopEdge) || edges == (Qt::LeftEdge | Qt::BottomEdge)) {
        setCursor(Qt::SizeBDiagCursor);
    } else if (edges == Qt::LeftEdge || edges == Qt::RightEdge) {
        setCursor(Qt::SizeHorCursor);
    } else if (edges == Qt::TopEdge || edges == Qt::BottomEdge) {
        setCursor(Qt::SizeVerCursor);
    } else {
        setCursor(selection().contains(globalPos) ? Qt::SizeAllCursor : Qt::CrossCursor);
    }
}

void AreaOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const bool resizing = drag_ == Drag::Resize;
        drag_ = Drag::None;
        spaceHeld_ = false;
        const QRect sel = selection();
        if (sel.width() < minimumAreaSize || sel.height() < minimumAreaSize) {
            if (resizing) {
                setSelection(dragStartSelection_); // a resize never deletes the selection
            } else {
                clearSelection();
            }
        }
        updateCursor(toGlobalPoint(event->position().toPoint()));
    }
}

void AreaOverlay::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && selection().contains(toGlobalPoint(event->position().toPoint()))) {
        Q_EMIT accepted();
    }
}

bool AreaOverlay::reapplyModifiers(QKeyEvent *event)
{
    const int key = event->key();
    if (key != Qt::Key_Shift && key != Qt::Key_Alt && key != Qt::Key_Control) {
        return false;
    }
    if (drag_ != Drag::None) {
        applyDrag(lastPosition_, event->modifiers());
    }
    return true;
}

void AreaOverlay::keyReleaseEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && spaceHeld_) {
        if (!event->isAutoRepeat()) {
            spaceHeld_ = false;
        }
        return;
    }
    if (!reapplyModifiers(event)) {
        QWidget::keyReleaseEvent(event);
    }
}

void AreaOverlay::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && drag_ != Drag::None) {
        if (!event->isAutoRepeat()) {
            spaceHeld_ = true;
        }
        return;
    }
    if (reapplyModifiers(event)) {
        return;
    }
    const Qt::KeyboardModifiers modifiers = event->modifiers();
    const int step = modifiers.testFlag(Qt::ShiftModifier) ? 10 : 1;
    const SelectionConstraints constraints{modifiers.testFlag(Qt::AltModifier),
                                           modifiers.testFlag(Qt::ControlModifier)};
    QPoint delta;
    switch (event->key()) {
    case Qt::Key_Escape:
        Q_EMIT rejected();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (selection().isValid()) {
            Q_EMIT accepted();
        }
        return;
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
    const QRect sel = selection();
    if (!sel.isValid()) {
        return;
    }
    setSelection(steppedSelection(sel, delta, constraints, minimumAreaSize, desktopBounds_));
}

} // namespace glimply
