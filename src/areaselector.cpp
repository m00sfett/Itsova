#include "areaselector.h"

#include "captureservice.h"
#include "screens.h"
#include "selectiongeometry.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QWindow>

#include <algorithm>
#include <cstdlib>

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

QRect clampToArea(QRect rect, const QSize &area)
{
    rect = rect.normalized();
    rect.setWidth(std::min(rect.width(), area.width()));
    rect.setHeight(std::min(rect.height(), area.height()));
    rect.moveLeft(std::clamp(rect.left(), 0, area.width() - rect.width()));
    rect.moveTop(std::clamp(rect.top(), 0, area.height() - rect.height()));
    return rect;
}

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

void AreaSelector::start(const QString &connector, const QRect &initial)
{
    for (const ScreenInfo &screen : currentScreens()) {
        if (!connector.isEmpty() && screen.connector != connector) {
            continue;
        }
        // A frozen picture keeps menus and tooltips from vanishing while the user aims.
        const CaptureResult frame = CaptureService::captureScreen(screen.connector);
        auto *overlay = new AreaOverlay(screen.connector, screen.geometry.size(), frame.image,
                                          connector.isEmpty() ? QRect() : initial);
        overlays_.append(overlay);
        connect(overlay, &AreaOverlay::selectionStarted, this, [this, overlay] {
            // Only one monitor holds the selection at a time.
            for (const QPointer<AreaOverlay> &other : std::as_const(overlays_)) {
                if (other && other != overlay) {
                    other->clearSelection();
                }
            }
        });
        connect(overlay, &AreaOverlay::accepted, this, [this, overlay] { finish(overlay, true); });
        connect(overlay, &AreaOverlay::rejected, this, [this, overlay] { finish(overlay, false); });

        overlay->setAttribute(Qt::WA_DeleteOnClose);
        overlay->winId();
        if (QScreen *qscreen = findScreen(screen.connector)) {
            overlay->windowHandle()->setScreen(qscreen);
            overlay->setGeometry(qscreen->geometry());
        }
        overlay->showFullScreen();
    }
    if (overlays_.isEmpty()) {
        Q_EMIT cancelled();
    } else {
        overlays_.first()->activateWindow();
    }
}

void AreaSelector::finish(AreaOverlay *source, bool accepted)
{
    const QString connector = source->connector();
    const QRect rect = source->selection();
    const QList<QPointer<AreaOverlay>> overlays = std::exchange(overlays_, {});
    for (const QPointer<AreaOverlay> &overlay : overlays) {
        if (overlay) {
            overlay->close();
        }
    }
    if (accepted && rect.width() >= minimumAreaSize && rect.height() >= minimumAreaSize) {
        Q_EMIT selected(connector, rect);
    } else {
        Q_EMIT cancelled();
    }
}

AreaOverlay::AreaOverlay(const QString &connector, const QSize &logicalSize, const QImage &background,
                             const QRect &initial)
    : connector_(connector)
    , logicalSize_(logicalSize)
    , background_(background)
    , selection_(initial.isValid() ? clampToArea(initial, logicalSize) : QRect())
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

QString AreaOverlay::connector() const
{
    return connector_;
}

QRect AreaOverlay::selection() const
{
    return selection_;
}

void AreaOverlay::clearSelection()
{
    selection_ = QRect();
    update();
}

void AreaOverlay::setSelection(const QRect &rect)
{
    selection_ = clampToArea(rect, logicalSize_);
    update();
}

void AreaOverlay::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const QRect area = rect();
    if (!background_.isNull()) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(area, background_);
    }

    // Dim everything outside the selection.
    QPainterPath shade;
    shade.addRect(area);
    if (selection_.isValid()) {
        shade.addRect(selection_);
    }
    painter.fillPath(shade, QColor(0, 0, 0, background_.isNull() ? 110 : 140));

    const QPalette pal = palette();
    const QColor accent = pal.color(QPalette::Highlight);
    if (selection_.isValid()) {
        painter.setPen(QPen(accent, 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(selection_.adjusted(0, 0, -1, -1));

        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF frame(selection_);
        const QPointF centre(frame.x() + frame.width() / 2.0, frame.y() + frame.height() / 2.0);
        const bool roomy = selection_.width() >= detailMinimumSide && selection_.height() >= detailMinimumSide;

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
        box = clampToArea(box, area.size());
        painter.setPen(Qt::NoPen);
        painter.setBrush(pal.color(QPalette::Window));
        painter.drawRoundedRect(box, 5, 5);
        painter.setPen(pal.color(QPalette::WindowText));
        painter.drawText(box, Qt::AlignCenter, text);
    };

    painter.setFont(font);
    pill(selection_.isValid()
             ? tr("Drag inside to move, edges or corners to resize. Shift: square, Alt: from centre, Space while "
                  "dragging: reposition. Enter confirms, Escape cancels.")
             : tr("Drag to select an area on %1. Shift: square, Alt: from centre, Space while dragging: reposition. "
                  "Escape cancels.")
                   .arg(connector_),
         QPoint(area.center().x(), 24), true);

    if (selection_.isValid()) {
        const QString size = tr("%1 × %2 at %3, %4")
                                 .arg(selection_.width())
                                 .arg(selection_.height())
                                 .arg(selection_.x())
                                 .arg(selection_.y());
        const bool roomBelow = selection_.bottom() + 40 < area.height();
        pill(size, roomBelow ? QPoint(selection_.center().x(), selection_.bottom() + 8)
                             : QPoint(selection_.center().x(), selection_.top() - 8),
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
    anchor_ = event->position().toPoint();
    lastPosition_ = anchor_;
    spaceHeld_ = false;
    dragStartSelection_ = selection_;
    if (const Qt::Edges edges = edgesAt(anchor_); edges) {
        drag_ = Drag::Resize;
        dragEdges_ = edges;
    } else if (selection_.contains(anchor_)) {
        drag_ = Drag::Move;
    } else {
        drag_ = Drag::Create;
        Q_EMIT selectionStarted();
        setSelection(QRect(anchor_, QSize(1, 1)));
    }
}

void AreaOverlay::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint position = event->position().toPoint();
    const QPoint step = position - lastPosition_;
    lastPosition_ = position;
    if (drag_ == Drag::None) {
        updateCursor(position);
    } else {
        if (spaceHeld_ && (drag_ == Drag::Create || drag_ == Drag::Resize)) {
            anchor_.setX(std::clamp(anchor_.x() + step.x(), 0, logicalSize_.width()));
            anchor_.setY(std::clamp(anchor_.y() + step.y(), 0, logicalSize_.height()));
            if (drag_ == Drag::Resize) {
                dragStartSelection_ = clampToArea(dragStartSelection_.translated(step), logicalSize_);
            }
        }
        applyDrag(position, event->modifiers());
    }
}

void AreaOverlay::applyDrag(const QPoint &position, Qt::KeyboardModifiers modifiers)
{
    const SelectionConstraints constraints{modifiers.testFlag(Qt::AltModifier),
                                           modifiers.testFlag(Qt::ShiftModifier)};
    if (drag_ == Drag::Create) {
        selection_ = drawnSelection(anchor_, position, constraints, logicalSize_);
        update();
    } else if (drag_ == Drag::Move) {
        setSelection(dragStartSelection_.translated(position - anchor_));
    } else if (drag_ == Drag::Resize) {
        selection_ = resizedSelection(dragStartSelection_, dragEdges_, position - anchor_, constraints, logicalSize_);
        update();
    }
}

Qt::Edges AreaOverlay::edgesAt(const QPoint &pos) const
{
    if (!selection_.isValid()) {
        return {};
    }
    const int grab = std::clamp(std::min(selection_.width(), selection_.height()) / 4, minGrabDistance, maxGrabDistance);
    const int left = selection_.left();
    const int right = selection_.left() + selection_.width();
    const int top = selection_.top();
    const int bottom = selection_.top() + selection_.height();
    const int x = pos.x();
    const int y = pos.y();

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

void AreaOverlay::updateCursor(const QPoint &pos)
{
    const Qt::Edges edges = edgesAt(pos);
    if (edges == (Qt::LeftEdge | Qt::TopEdge) || edges == (Qt::RightEdge | Qt::BottomEdge)) {
        setCursor(Qt::SizeFDiagCursor);
    } else if (edges == (Qt::RightEdge | Qt::TopEdge) || edges == (Qt::LeftEdge | Qt::BottomEdge)) {
        setCursor(Qt::SizeBDiagCursor);
    } else if (edges == Qt::LeftEdge || edges == Qt::RightEdge) {
        setCursor(Qt::SizeHorCursor);
    } else if (edges == Qt::TopEdge || edges == Qt::BottomEdge) {
        setCursor(Qt::SizeVerCursor);
    } else {
        setCursor(selection_.contains(pos) ? Qt::SizeAllCursor : Qt::CrossCursor);
    }
}

void AreaOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const bool resizing = drag_ == Drag::Resize;
        drag_ = Drag::None;
        spaceHeld_ = false;
        if (selection_.width() < minimumAreaSize || selection_.height() < minimumAreaSize) {
            if (resizing) {
                setSelection(dragStartSelection_); // a resize never deletes the selection
            } else {
                clearSelection();
            }
        }
        updateCursor(event->position().toPoint());
    }
}

void AreaOverlay::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && selection_.contains(event->position().toPoint())) {
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
        if (selection_.isValid()) {
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
    if (!selection_.isValid()) {
        return;
    }
    setSelection(steppedSelection(selection_, delta, constraints, minimumAreaSize, logicalSize_));
}

} // namespace glimply
