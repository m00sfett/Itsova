#include "regionselector.h"

#include "captureservice.h"
#include "screens.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QWindow>

#include <algorithm>

namespace glimply
{
namespace
{

constexpr int minimumRegionSize = 4;

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

RegionSelector::RegionSelector(QObject *parent)
    : QObject(parent)
{
}

RegionSelector::~RegionSelector()
{
    for (const QPointer<RegionOverlay> &overlay : std::as_const(overlays_)) {
        delete overlay.data();
    }
}

void RegionSelector::start(const QString &connector, const QRect &initial)
{
    for (const ScreenInfo &screen : currentScreens()) {
        if (!connector.isEmpty() && screen.connector != connector) {
            continue;
        }
        // A frozen picture keeps menus and tooltips from vanishing while the user aims.
        const CaptureResult frame = CaptureService::captureScreen(screen.connector);
        auto *overlay = new RegionOverlay(screen.connector, screen.geometry.size(), frame.image,
                                          connector.isEmpty() ? QRect() : initial);
        overlays_.append(overlay);
        connect(overlay, &RegionOverlay::selectionStarted, this, [this, overlay] {
            // Only one monitor holds the selection at a time.
            for (const QPointer<RegionOverlay> &other : std::as_const(overlays_)) {
                if (other && other != overlay) {
                    other->clearSelection();
                }
            }
        });
        connect(overlay, &RegionOverlay::accepted, this, [this, overlay] { finish(overlay, true); });
        connect(overlay, &RegionOverlay::rejected, this, [this, overlay] { finish(overlay, false); });

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

void RegionSelector::finish(RegionOverlay *source, bool accepted)
{
    const QString connector = source->connector();
    const QRect rect = source->selection();
    const QList<QPointer<RegionOverlay>> overlays = std::exchange(overlays_, {});
    for (const QPointer<RegionOverlay> &overlay : overlays) {
        if (overlay) {
            overlay->close();
        }
    }
    if (accepted && rect.width() >= minimumRegionSize && rect.height() >= minimumRegionSize) {
        Q_EMIT selected(connector, rect);
    } else {
        Q_EMIT cancelled();
    }
}

RegionOverlay::RegionOverlay(const QString &connector, const QSize &logicalSize, const QImage &background,
                             const QRect &initial)
    : connector_(connector)
    , logicalSize_(logicalSize)
    , background_(background)
    , selection_(initial.isValid() ? clampToArea(initial, logicalSize) : QRect())
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setWindowTitle(tr("Select a region on %1").arg(connector));
    if (background_.isNull()) {
        setAttribute(Qt::WA_TranslucentBackground);
    }
    setCursor(Qt::CrossCursor);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

QString RegionOverlay::connector() const
{
    return connector_;
}

QRect RegionOverlay::selection() const
{
    return selection_;
}

void RegionOverlay::clearSelection()
{
    selection_ = QRect();
    update();
}

void RegionOverlay::setSelection(const QRect &rect)
{
    selection_ = clampToArea(rect, logicalSize_);
    update();
}

void RegionOverlay::paintEvent(QPaintEvent *)
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
             ? tr("Press Enter to use this region, Escape to cancel. Arrow keys move it, Shift+arrow keys resize it.")
             : tr("Drag to select a region on %1. Press Escape to cancel.").arg(connector_),
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

void RegionOverlay::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton) {
        Q_EMIT rejected();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }
    anchor_ = event->position().toPoint();
    if (selection_.contains(anchor_)) {
        drag_ = Drag::Move;
        dragStartSelection_ = selection_;
    } else {
        drag_ = Drag::Create;
        Q_EMIT selectionStarted();
        setSelection(QRect(anchor_, QSize(1, 1)));
    }
}

void RegionOverlay::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint position = event->position().toPoint();
    if (drag_ == Drag::Create) {
        setSelection(QRect(anchor_, position));
    } else if (drag_ == Drag::Move) {
        setSelection(dragStartSelection_.translated(position - anchor_));
    } else {
        setCursor(selection_.contains(position) ? Qt::SizeAllCursor : Qt::CrossCursor);
    }
}

void RegionOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        drag_ = Drag::None;
        if (selection_.width() < minimumRegionSize || selection_.height() < minimumRegionSize) {
            clearSelection();
        }
    }
}

void RegionOverlay::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && selection_.contains(event->position().toPoint())) {
        Q_EMIT accepted();
    }
}

void RegionOverlay::keyPressEvent(QKeyEvent *event)
{
    const int step = event->modifiers().testFlag(Qt::ControlModifier) ? 10 : 1;
    const bool resize = event->modifiers().testFlag(Qt::ShiftModifier);
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
    if (resize) {
        QRect resized = selection_;
        resized.setWidth(std::max(minimumRegionSize, resized.width() + delta.x()));
        resized.setHeight(std::max(minimumRegionSize, resized.height() + delta.y()));
        setSelection(resized);
    } else {
        setSelection(selection_.translated(delta));
    }
}

} // namespace glimply
