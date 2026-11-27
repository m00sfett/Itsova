#include "captureoverlay_p.h"
#include "captureoverlay.h"

#include "logging.h"
#include "annotations/annotationrenderer.h"
#include "annotations/annotationtoolbar.h"
#include "annotations/inlinetexteditor.h"
#include "annotations/overlayexport.h"
#include "app.h"
#include "colorformat.h"
#include "exportdialog.h"
#include "imageoutput.h"
#include "latencytrace.h"
#include "overlaypanel.h"
#include "ocrengine.h"
#include "pixelloupe.h"
#include "screens.h"
#include "selectiongeometry.h"
#include "shortcutmanager.h"
#include "triggerplan.h"

#include <KKeySequenceWidget>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImageWriter>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>

namespace itsova
{
namespace
{

constexpr int minimumAreaSize = 4;
// Time the compositor gets to take the hidden overlays off the screen before a live capture.
constexpr int liveHideDelayMs = 250;
constexpr qreal handleRadius = 5.0;
constexpr int maxGrabDistance = 10;
constexpr int minGrabDistance = 4;
constexpr int cornerBonus = 4;
constexpr int detailMinimumSide = 40;
constexpr int closeButtonSize = 22;
// Distance of the panel from the monitor edge.
constexpr int panelMargin = 16;
constexpr int frozenDim = 140;
// Live: lighter, so a video underneath stays watchable.
constexpr int liveDim = 70;

} // namespace

// ------------------------------------------------------------------------------------------ Overlay

CaptureOverlay::CaptureOverlay(CaptureSession *session, const ScreenInfo &screen)
    : session_(session)
    , screen_(screen)
    , lastPosition_(screen.geometry.topLeft())
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setWindowTitle(tr("Itsova capture on %1").arg(session->monitorName(screen.connector)));
    // Always translucent, so the same window can show the frozen picture or the live screen.
    setAttribute(Qt::WA_TranslucentBackground);
    setCursor(Qt::CrossCursor);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    buildPanel();
    if (session_->purpose() == CaptureSession::Purpose::Print) {
        textEditor_ = new InlineTextEditor(this);
        AnnotationController &ctrl = session_->annotationController();
        connect(&ctrl, &AnnotationController::textEditStyleUpdated, textEditor_, &InlineTextEditor::updateStyle);
        connect(&ctrl, &AnnotationController::textEditCommitRequested, textEditor_, &InlineTextEditor::commit);
        connect(&ctrl, &AnnotationController::textEditCancelRequested, textEditor_, &InlineTextEditor::cancel);
        connect(textEditor_, &InlineTextEditor::draftChanged, &ctrl, &AnnotationController::updateActiveTextDraft);
        connect(textEditor_, &InlineTextEditor::styleToggled, &ctrl, &AnnotationController::updateActiveTextDraft);
        connect(textEditor_, &InlineTextEditor::committed, this,
                [this](const AnnotationItem &item, bool, bool switchToSelect) {
                    session_->annotationController().updateActiveTextDraft(item);
                    session_->annotationController().commitTextEdit(switchToSelect);
                    session_->persistAnnotationSettings();
                    setFocus(Qt::OtherFocusReason);
                });
        connect(textEditor_, &InlineTextEditor::cancelled, this, [this](int) {
            session_->annotationController().cancelTextEdit();
            setFocus(Qt::OtherFocusReason);
        });
    }
}

QString CaptureOverlay::connector() const
{
    return screen_.connector;
}

const ScreenInfo &CaptureOverlay::screen() const
{
    return screen_;
}

InlineTextEditor *CaptureOverlay::textEditor() const
{
    return textEditor_;
}

void CaptureOverlay::startInlineTextEdit(const AnnotationItem &item, bool isExisting)
{
    if (textEditor_) {
        textEditor_->startEditing(item, isExisting, 1.0, -QPointF(screen_.geometry.topLeft()));
    }
}

void CaptureOverlay::stopInlineTextEdit()
{
    if (textEditor_ && !textEditor_->isHidden()) {
        textEditor_->hide();
    }
}

QPoint CaptureOverlay::toGlobalPoint(const QPoint &localPos) const
{
    return localPos + screen_.geometry.topLeft();
}

QRect CaptureOverlay::toLocalRect(const QRect &globalRect) const
{
    return globalRect.translated(-screen_.geometry.topLeft());
}

QPointF CaptureOverlay::toGlobalLogical(const QPointF &localPos) const
{
    return localPos + QPointF(screen_.geometry.topLeft());
}


void CaptureOverlay::refresh()
{
    refreshPanel();
    updateCursor(lastPosition_);
    update();
}

QWidget *CaptureOverlay::buildHelpPanel(QWidget *parent) const
{
    const bool print = session_->purpose() == CaptureSession::Purpose::Print;
    auto *panel = new QWidget(parent);
    auto *columns = new QHBoxLayout(panel);
    columns->setContentsMargins(8, 6, 8, 4);
    columns->setSpacing(28);

    const auto section = [&](const QString &title, const QList<QPair<QString, QString>> &rows) {
        auto *grid = new QGridLayout;
        grid->setHorizontalSpacing(14);
        grid->setVerticalSpacing(3);
        auto *heading = new QLabel(title, panel);
        QFont bold = heading->font();
        bold.setBold(true);
        heading->setFont(bold);
        grid->addWidget(heading, 0, 0, 1, 2);
        grid->setRowMinimumHeight(1, 2);
        int line = 2;
        const bool rtl = panel->layoutDirection() == Qt::RightToLeft;
        for (const auto &[keys, meaning] : rows) {
            auto *keyLabel = new QLabel(keys, panel);
            keyLabel->setFont(bold);
            keyLabel->setAlignment((rtl ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignAbsolute | Qt::AlignTop);
            auto *meaningLabel = new QLabel(meaning, panel);
            meaningLabel->setAlignment((rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignAbsolute | Qt::AlignTop);
            grid->addWidget(keyLabel, line, 0);
            grid->addWidget(meaningLabel, line, 1);
            ++line;
        }
        grid->setRowStretch(line, 1);
        columns->addLayout(grid);
    };

    // The F1 overview (#72, Q15): the buttons show their titles and keycaps; this table adds the
    // modifiers and keys no button shows, grouped by what is being worked on.
    QList<QPair<QString, QString>> select{
        {tr("Drag"), print ? tr("New rectangle") : tr("New area")},
        {tr("Shift while dragging"), tr("Keep a square")},
        {tr("Alt while dragging"), tr("Grow from the centre")},
        {tr("Space while dragging"), tr("Reposition live")},
        {tr("Arrows while dragging"), tr("Move the corner by 1 / 10 screen pixels")},
        {tr("Arrows, nothing selected"), tr("Move through the panel and its menus")},
        {tr("Arrows / Shift+Arrows"), tr("Move by 1 px / 10 px")},
        {tr("Alt+Arrows / Ctrl+Arrows"), tr("Resize from the centre / as a square")},
        {tr("Delete"), tr("Delete the active rectangle")},
        {tr("Double-click"), print ? tr("Save the rectangle") : tr("Save as area")},
        {tr("Escape"), print ? tr("Step back, or close") : tr("Cancel")},
    };
    if (print) {
        select.insert(1, {tr("Click, nothing drawn"), tr("Capture the window or monitor under the pointer")});
        select.append({tr("Ctrl+E"), tr("Open in the editor window")});
        select.append({tr("Ctrl+R"), tr("Record the rectangle as a video, or choose what to record")});
    }
    if (session_->recordMode()) {
        select.append({tr("P"), tr("Pin the focused entry or the rectangle as the default of the recording shortcut")});
    }
    section(tr("Select"), select);
    if (print) {
        section(tr("Markup"),
                {
                    {QStringLiteral("V A R E B M T N X S"), tr("Pick a tool")},
                    {tr("Shift while drawing"), tr("Square, circle or 15° steps")},
                    {tr("Alt while drawing"), tr("Grow from the centre")},
                    {tr("Alt+Drag"), tr("Duplicate an annotation")},
                    {tr("Space while drawing"), tr("Reposition live")},
                    {tr("Ctrl"), tr("Select and move for a moment")},
                    {tr("[ / ]"), tr("Smaller / larger brush or font")},
                    {tr("Ctrl+Z / Ctrl+Shift+Z"), tr("Undo / Redo")},
                    {tr("I"), tr("Pick a color; Alt+Click applies it")},
                });
    }
    return panel;
}

bool CaptureOverlay::focusNextPrevChild(bool next)
{
    if (session_->colorPickerActive()) {
        return false;
    }
    return QWidget::focusNextPrevChild(next);
}

QRect CaptureOverlay::closeButtonGlobal(const QRect &globalRect) const
{
    const Qt::LayoutDirection direction = layoutDirection();
    const QPoint corner = direction == Qt::RightToLeft ? globalRect.topLeft() : globalRect.topRight();
    const QList<ScreenInfo> cornerScreens = session_->intersectingScreens(QRect(corner, QSize(1, 1)));
    const QRect bounds = cornerScreens.isEmpty() ? session_->desktopBounds() : cornerScreens.first().geometry;
    return overlayCloseButtonRect(globalRect, bounds, closeButtonSize, direction);
}

int CaptureOverlay::closeButtonAt(const QPoint &globalPos) const
{
    const SelectionModel &model = session_->selections();
    for (const int id : {model.activeId(), session_->hoveredSelectionId()}) {
        const Selection *selection = model.find(id);
        if (selection && closeButtonGlobal(selection->rect).contains(globalPos)) {
            return id;
        }
    }
    return 0;
}

void CaptureOverlay::paintEvent(QPaintEvent *)
{
    // Marks the first overlay frame once it is fully painted (#67).
    struct FirstFrameMark {
        CaptureSession *session;
        ~FirstFrameMark()
        {
            if (session->purpose() == CaptureSession::Purpose::Print
                && !LatencyTrace::reached(LatencyTrace::Stage::Overlay)) {
                LatencyTrace::mark(LatencyTrace::Stage::Overlay);
                QTimer::singleShot(0, session, &CaptureSession::finishLatencyBenchIfDone);
            }
        }
    } firstFrameMark{session_};
    QPainter painter(this);
    const QRect area = rect();
    const QImage background = session_->frame(screen_.connector);
    const bool frozen = !background.isNull();
    if (frozen) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(area, background);
    }

    const QPalette pal = palette();
    const QColor accent = pal.color(QPalette::Highlight);
    const QColor negative(218, 68, 83); // Breeze "negative text"

    if (session_->purpose() == CaptureSession::Purpose::Print
        && (!session_->annotations().isEmpty() || session_->annotationController().draftItem())) {
        painter.save();
        painter.translate(-screen_.geometry.topLeft());
        const qreal frameScale = frozen && width() > 0 ? qreal(background.width()) / width() : 1.0;
        paintAnnotationOverlay(painter, QRectF(screen_.geometry), session_->annotations(), accent,
                               session_->annotationController().hoveredId(),
                               session_->annotationController().draftItem(), frozen ? &background : nullptr,
                               frameScale, QPointF(screen_.geometry.topLeft()),
                               session_->annotationController().editingTextItemId());
        painter.restore();
    }

    const SelectionModel &model = session_->selections();
    const bool suppressBackdropDim = model.isEmpty() && session_->purpose() == CaptureSession::Purpose::Print
                                    && (session_->markupVisible() || !session_->annotations().isEmpty());
    if (!suppressBackdropDim) {
        QPainterPath holes;
        for (const Selection &selection : model.stack()) {
            const QRectF hole = session_->displayRect(selection)
                                    .translated(-QPointF(screen_.geometry.topLeft()))
                                    .intersected(QRectF(area));
            if (!hole.isEmpty()) {
                holes.addRect(hole);
            }
        }
        QPainterPath shade;
        shade.addRect(area);
        painter.fillPath(shade.subtracted(holes), QColor(0, 0, 0, frozen ? frozenDim : liveDim));
    }

    // Outlines of areas or windows hovered in a list; without one, the target under the pointer (#97).
    QList<CaptureSession::Highlight> outlines = session_->highlights();
    if (outlines.isEmpty() && session_->pointerPick().kind != PickTarget::Kind::None && drag_ == Drag::None
        && session_->pickingActive()) {
        outlines.append({session_->pointerPick().rect, QString()});
    }
    for (const CaptureSession::Highlight &highlight : std::as_const(outlines)) {
        const QRect local = highlight.rect.translated(-screen_.geometry.topLeft()).intersected(area);
        if (local.isEmpty()) {
            continue;
        }
        painter.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 40));
        QPen dashed(accent, 2);
        dashed.setDashPattern({4.0, 3.0});
        painter.setPen(dashed);
        painter.drawRect(local.adjusted(1, 1, -1, -1));
    }

    painter.setRenderHint(QPainter::Antialiasing);
    const bool numbered = model.count() > 1;
    const int hoveredId = session_->hoveredSelectionId();
    for (const Selection &selection : model.stack()) {
        const QRect local = toLocalRect(selection.rect);
        if (!local.intersects(area)) {
            continue;
        }
        const bool active = selection.id == model.activeId();
        const QRectF frame = session_->displayRect(selection).translated(-QPointF(screen_.geometry.topLeft()));
        painter.setBrush(Qt::NoBrush);
        if (active) {
            painter.setPen(QPen(accent, 2));
            painter.drawRect(frame.adjusted(1, 1, -1, -1));
        } else {
            // A hovered rectangle shows in the theme's negative colour: a right click deletes it.
            const bool hovered = selection.id == hoveredId && !session_->isDragging();
            painter.setPen(QPen(QColor(0, 0, 0, 120), 3));
            painter.drawRect(frame.adjusted(0.5, 0.5, -0.5, -0.5));
            painter.setPen(QPen(hovered ? negative : QColor(255, 255, 255, 200), hovered ? 2 : 1));
            painter.drawRect(frame.adjusted(0.5, 0.5, -0.5, -0.5));
        }

        if (numbered) {
            const int number = model.number(selection.id);
            QRectF badge(0, 0, 22, 22);
            badge.moveCenter(overlaySelectionBadgeCenter(frame, layoutDirection()));
            painter.setPen(QPen(QColor(255, 255, 255, 230), 1.5));
            painter.setBrush(active ? accent : QColor(60, 60, 60, 220));
            painter.drawEllipse(badge);
            QFont font = painter.font();
            font.setBold(true);
            painter.setFont(font);
            painter.setPen(active ? pal.color(QPalette::HighlightedText) : QColor(Qt::white));
            painter.drawText(badge, Qt::AlignCenter, QString::number(number));
        }

        if (active || selection.id == hoveredId) {
            const QRect button = toLocalRect(closeButtonGlobal(selection.rect));
            if (button.intersects(area)) {
                painter.setPen(QPen(QColor(0, 0, 0, 60), 1));
                painter.setBrush(pal.color(QPalette::Window));
                painter.drawEllipse(QRectF(button).adjusted(0.5, 0.5, -0.5, -0.5));
                const QIcon icon = QIcon::fromTheme(QStringLiteral("window-close"));
                const QRect iconRect(button.center() - QPoint(7, 7), QSize(16, 16));
                icon.paint(&painter, iconRect.adjusted(1, 1, -1, -1));
            }
        }

        if (!active) {
            continue;
        }
        const QPointF centre(frame.x() + frame.width() / 2.0, frame.y() + frame.height() / 2.0);
        const bool roomy = selection.rect.width() >= detailMinimumSide && selection.rect.height() >= detailMinimumSide;
        constexpr qreal arm = 8.0;
        const auto drawLines = [&](const QList<QLineF> &lines, const QColor &light, bool dotted) {
            QPen halo(QColor(0, 0, 0, 90), 3);
            QPen line(light, 1);
            if (dotted) {
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
                      QColor(255, 255, 255, 110), true);
        }
        drawLines({QLineF(centre.x() - arm, centre.y(), centre.x() + arm, centre.y()),
                   QLineF(centre.x(), centre.y() - arm, centre.x(), centre.y() + arm)},
                  QColor(255, 255, 255, 180), false);

        // The number badge sits in the top left corner; that corner still resizes, without a dot.
        QList<QPointF> handles{frame.topRight(), frame.bottomLeft(), frame.bottomRight()};
        if (!numbered) {
            handles << frame.topLeft();
        }
        if (roomy) {
            handles << QPointF(centre.x(), frame.top()) << QPointF(centre.x(), frame.bottom())
                    << QPointF(frame.left(), centre.y()) << QPointF(frame.right(), centre.y());
        }
        painter.setPen(QPen(QColor(255, 255, 255, 235), 1.5));
        painter.setBrush(accent);
        for (const QPointF &handle : std::as_const(handles)) {
            painter.drawEllipse(handle, handleRadius, handleRadius);
        }

        // Size of the active rectangle, above its leading corner (inside when there is no room).
        // Native pixels: what the saved file will measure (#101).
        const QSize nativeSize = session_->nativeSize(selection);
        const QString sizeText = tr("%1 × %2").arg(nativeSize.width()).arg(nativeSize.height());
        QFont sizeFont = font();
        sizeFont.setPointSizeF(std::max(7.0, sizeFont.pointSizeF() * 0.9));
        const QFontMetrics sizeMetrics(sizeFont);
        QRectF tag(0, 0, sizeMetrics.horizontalAdvance(sizeText) + 14, sizeMetrics.height() + 4);
        const bool rtl = layoutDirection() == Qt::RightToLeft;
        const qreal inset = numbered ? 30 : 0;
        tag.moveTop(frame.top() - tag.height() - 6 >= 0 ? frame.top() - tag.height() - 6 : frame.top() + 6);
        if (rtl) {
            tag.moveRight(frame.right() - (tag.top() > frame.top() ? inset : 0));
        } else {
            tag.moveLeft(frame.left() + (tag.top() > frame.top() ? inset : 0));
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(20, 22, 24, 215));
        painter.drawRoundedRect(tag, 4, 4);
        painter.setFont(sizeFont);
        painter.setPen(QColor(252, 252, 252));
        painter.drawText(tag, Qt::AlignCenter, sizeText);
    }

    if (session_->colorPickerActive() && frozen && screen_.geometry.contains(session_->pickerCursorGlobal())) {
        const QPoint localCursor = session_->pickerCursorGlobal() - screen_.geometry.topLeft();
        const QPoint nativePx = logicalToNativePixel(QPointF(localCursor), QPointF(0.0, 0.0),
                                                     session_->frameScale(screen_.connector), background.size());
        PixelLoupe::paintColorPickerLoupe(painter, localCursor, area, pal, background, nativePx,
                                          session_->store().colorPickerFormat, true);
    } else if (session_->dragLoupeActive() && screen_.geometry.contains(session_->dragCursorGlobal())) {
        if (const Selection *sel = model.active()) {
            const QPoint localCursor = session_->dragCursorGlobal() - screen_.geometry.topLeft();
            // Frozen or live (#101): live, the pixels are the Print snapshot.
            bool snapshot = false;
            const QImage loupeImage = session_->loupeFrame(screen_.connector, &snapshot);
            const QSize nativeSize = session_->nativeSize(*sel);
            if (session_->store().showSelectionLoupe && !loupeImage.isNull()) {
                const qreal scale = session_->monitorScale(screen_.connector);
                QPoint nativePx =
                    logicalToNativePixel(QPointF(localCursor), QPointF(0.0, 0.0), scale, loupeImage.size());
                const QRect nativeSel = session_->nativeSelectionOn(*sel, screen_.connector);
                // Centred on the dragged corner or edge, which a nudge moves away from the pointer
                // (#102): the inside pixel next to it.
                const Qt::Edges edges = session_->dragEdges();
                if (edges.testFlag(Qt::LeftEdge)) {
                    nativePx.setX(nativeSel.left());
                } else if (edges.testFlag(Qt::RightEdge)) {
                    nativePx.setX(nativeSel.right());
                }
                if (edges.testFlag(Qt::TopEdge)) {
                    nativePx.setY(nativeSel.top());
                } else if (edges.testFlag(Qt::BottomEdge)) {
                    nativePx.setY(nativeSel.bottom());
                }
                PixelLoupe::paintSelectionLoupe(painter, localCursor, area, pal, loupeImage, nativePx, nativePx,
                                                nativeSize, nativeSel, snapshot, session_->dragKeyNudge());
            } else {
                PixelLoupe::paintDimensionPill(painter, localCursor, area, pal, session_->dragCursorGlobal(),
                                               nativeSize);
            }
        }
    }
}

Qt::Edges CaptureOverlay::edgesAt(const QRect &rect, const QPoint &pos) const
{
    const int grab = std::clamp(std::min(rect.width(), rect.height()) / 4, minGrabDistance, maxGrabDistance);
    const int left = rect.left();
    const int right = rect.left() + rect.width();
    const int top = rect.top();
    const int bottom = rect.top() + rect.height();
    const int x = pos.x();
    const int y = pos.y();
    const int corner = grab + cornerBonus;
    const bool nearLeft = std::abs(x - left) <= corner;
    const bool nearRight = std::abs(x - right) <= corner;
    const bool nearTop = std::abs(y - top) <= corner;
    const bool nearBottom = std::abs(y - bottom) <= corner;
    if ((nearLeft || nearRight) && (nearTop || nearBottom)) {
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

void CaptureOverlay::updateCursor(const QPoint &globalPos)
{
    if (session_->colorPickerActive()) {
        setCursor(Qt::CrossCursor);
        return;
    }
    if (closeButtonAt(globalPos)) {
        setCursor(Qt::PointingHandCursor);
        return;
    }
    if (session_->purpose() == CaptureSession::Purpose::Print) {
        AnnotationController &ctrl = session_->annotationController();
        const Qt::KeyboardModifiers mods = QGuiApplication::keyboardModifiers();
        const QPointF gPos(globalPos);
        if (ctrl.isDragging() || ctrl.effectiveTool(mods) != ToolKind::Select) {
            setCursor(ctrl.cursorFor(gPos, mods, 8.0));
            return;
        }
        const AnnotationItem *selAnn = session_->annotations().selectedItem();
        if (session_->annotations().topmostHitAt(gPos) > 0
            || (selAnn && selAnn->handleAt(gPos, 8.0) != AnnotationHandle::None)) {
            setCursor(ctrl.cursorFor(gPos, mods, 8.0));
            return;
        }
    }
    const Selection *active = session_->selections().active();
    if (active && !session_->forceNew()) {
        const Qt::Edges edges = edgesAt(active->rect, globalPos);
        if (edges == (Qt::LeftEdge | Qt::TopEdge) || edges == (Qt::RightEdge | Qt::BottomEdge)) {
            setCursor(Qt::SizeFDiagCursor);
            return;
        }
        if (edges == (Qt::RightEdge | Qt::TopEdge) || edges == (Qt::LeftEdge | Qt::BottomEdge)) {
            setCursor(Qt::SizeBDiagCursor);
            return;
        }
        if (edges == Qt::LeftEdge || edges == Qt::RightEdge) {
            setCursor(Qt::SizeHorCursor);
            return;
        }
        if (edges == Qt::TopEdge || edges == Qt::BottomEdge) {
            setCursor(Qt::SizeVerCursor);
            return;
        }
        if (active->rect.contains(globalPos)) {
            setCursor(Qt::SizeAllCursor);
            return;
        }
    }
    if (!session_->forceNew() && session_->selections().topmostAt(globalPos)) {
        setCursor(Qt::PointingHandCursor);
        return;
    }
    setCursor(Qt::CrossCursor);
}

void CaptureOverlay::mousePressEvent(QMouseEvent *event)
{
    const QPoint globalPos = toGlobalPoint(event->position().toPoint());
    lastPosition_ = globalPos;
    if (session_->colorPickerActive()) {
        if (event->button() == Qt::RightButton) {
            session_->setColorPickerActive(false);
        } else if (event->button() == Qt::LeftButton) {
            session_->pickColorAt(globalPos, event->modifiers().testFlag(Qt::AltModifier));
        }
        updateCursor(globalPos);
        return;
    }
    if (event->button() == Qt::RightButton) {
        // Right click deletes the topmost annotation under pointer first, else the crop rectangle.
        if (drag_ == Drag::None) {
            if (session_->purpose() == CaptureSession::Purpose::Print
                && !session_->annotationController().isDragging()) {
                if (const int annId = session_->annotations().topmostHitAt(toGlobalLogical(event->position()))) {
                    session_->annotations().removeItem(annId);
                    updateCursor(globalPos);
                    return;
                }
            }
            if (const int id = session_->selections().topmostAt(globalPos)) {
                session_->selections().remove(id);
                session_->setHoveredSelectionId(0);
                session_->changed();
                updateCursor(globalPos);
            }
        }
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }
    session_->dismissAreaPanels();
    if (session_->purpose() == CaptureSession::Purpose::Print
        && session_->annotationController().isEditingText()) {
        session_->annotationController().requestCommitActiveText(false);
        return;
    }

    SelectionModel &model = session_->selections();
    anchor_ = globalPos;
    lastPosition_ = globalPos;
    lastDragPos_ = globalPos;
    spaceHeld_ = false;
    nudgeZone_.reset();
    nudgeTotal_ = {};
    dragStartNudge_ = {};

    const bool selectTool = session_->purpose() != CaptureSession::Purpose::Print
                         || session_->annotationController().effectiveTool(event->modifiers()) == ToolKind::Select;
    if (selectTool) {
        if (const int id = closeButtonAt(globalPos)) {
            model.remove(id);
            session_->setHoveredSelectionId(0);
            drag_ = Drag::None;
            session_->changed();
            updateCursor(globalPos);
            return;
        }
    }

    if (session_->purpose() == CaptureSession::Purpose::Print) {
        if (session_->annotationController().mousePress(toGlobalLogical(event->position()), Qt::LeftButton,
                                                        event->modifiers(), 8.0)) {
            return;
        }
    }

    const Selection *active = model.active();
    if (active && !session_->forceNew()) {
        dragStartRect_ = active->rect;
        dragId_ = active->id;
        dragStartNudge_ = active->nativeNudge; // restored by Escape, kept by a move (#102)
        if (const Qt::Edges edges = edgesAt(active->rect, globalPos)) {
            drag_ = Drag::Resize;
            dragEdges_ = edges;
            return;
        }
        if (active->rect.contains(globalPos)) {
            drag_ = Drag::Move;
            return;
        }
    }
    // Decide on release (click) or on the first movement (drag).
    drag_ = Drag::Pending;
    pendingId_ = session_->forceNew() ? 0 : model.topmostAt(globalPos);
}

void CaptureOverlay::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint globalPos = toGlobalPoint(event->position().toPoint());
    const QPoint step = globalPos - lastPosition_;
    lastPosition_ = globalPos;
    session_->notePointerPosition(globalPos);
    session_->notePointerOn(screen_.connector); // the panel follows the pointer while nothing is selected

    if (session_->colorPickerActive()) {
        session_->setPickerCursorGlobal(globalPos);
        updateCursor(globalPos);
        return;
    }

    if (session_->purpose() == CaptureSession::Purpose::Print && session_->annotationController().isDragging()) {
        session_->annotationController().mouseMove(toGlobalLogical(event->position()), event->modifiers());
        return;
    }

    if (drag_ == Drag::None) {
        if (session_->purpose() == CaptureSession::Purpose::Print) {
            AnnotationController &ctrl = session_->annotationController();
            const bool annHit = ctrl.mouseMove(toGlobalLogical(event->position()), event->modifiers());
            if (annHit || ctrl.effectiveTool(event->modifiers()) != ToolKind::Select) {
                session_->setHoveredSelectionId(0);
                updateCursor(globalPos);
                return;
            }
        }
        session_->setHoveredSelectionId(session_->selections().topmostAt(globalPos));
        session_->setPointerPick(session_->pickAt(globalPos));
        updateCursor(globalPos);
        return;
    }

    if (drag_ == Drag::Pending) {
        if ((globalPos - anchor_).manhattanLength() < QApplication::startDragDistance()) {
            return;
        }
        drag_ = Drag::Create;
        session_->setPointerPick({});
        dragId_ = session_->selections().add(QRect(anchor_, QSize(1, 1)));
        session_->setForceNew(false);
        session_->setDragging(true);
    } else if (drag_ == Drag::Move || drag_ == Drag::Resize) {
        session_->setDragging(true);
    }
    QPoint dragPos = globalPos;
    if (drag_ == Drag::Create || drag_ == Drag::Resize) {
        // After an arrow key the mouse waits until it moves clearly, then continues from the corner (#102).
        const std::optional<QPoint> filtered = nudgeZone_.filter(globalPos);
        if (!filtered) {
            return;
        }
        dragPos = *filtered;
    }
    if (spaceHeld_ && (drag_ == Drag::Create || drag_ == Drag::Resize)) {
        const QRect bounds = session_->desktopBounds();
        anchor_.setX(std::clamp(anchor_.x() + step.x(), bounds.left(), bounds.left() + bounds.width()));
        anchor_.setY(std::clamp(anchor_.y() + step.y(), bounds.top(), bounds.top() + bounds.height()));
        if (drag_ == Drag::Resize) {
            dragStartRect_ = clampToBounds(dragStartRect_.translated(step), bounds);
        }
    }
    applyDrag(dragPos, dragModifiers(event->modifiers()));
}

Qt::Edges CaptureOverlay::currentDragEdges(const QPoint &dragPos) const
{
    if (drag_ == Drag::Create) {
        return draggedEdges(anchor_, dragPos);
    }
    return drag_ == Drag::Resize ? dragEdges_ : Qt::Edges();
}

Qt::KeyboardModifiers CaptureOverlay::dragModifiers(Qt::KeyboardModifiers modifiers) const
{
    return nudgeTotal_.isNull() ? modifiers : modifiers & ~Qt::ShiftModifier;
}

void CaptureOverlay::applyDrag(const QPoint &globalPos, Qt::KeyboardModifiers modifiers)
{
    const SelectionConstraints constraints{modifiers.testFlag(Qt::AltModifier),
                                           modifiers.testFlag(Qt::ShiftModifier)};
    SelectionModel &model = session_->selections();
    const QRect bounds = session_->desktopBounds();
    const QList<QRect> screenRects = session_->screenRects();
    if (drag_ == Drag::Create) {
        QPoint effectivePos = globalPos;
        if (!spaceHeld_) {
            const QPoint snapped = snapPointToMonitorEdges(globalPos, screenRects);
            if (std::abs(snapped.x() - anchor_.x()) >= minimumAreaSize) {
                effectivePos.setX(snapped.x());
            }
            if (std::abs(snapped.y() - anchor_.y()) >= minimumAreaSize) {
                effectivePos.setY(snapped.y());
            }
        }
        model.setRect(dragId_, drawnSelection(anchor_, effectivePos, constraints, bounds));
    } else if (drag_ == Drag::Move) {
        QRect moved = clampToBounds(dragStartRect_.translated(globalPos - anchor_), bounds);
        if (!spaceHeld_) {
            moved = snapMovedRectToMonitorEdges(moved, screenRects, bounds);
        }
        model.setRect(dragId_, moved);
    } else if (drag_ == Drag::Resize) {
        QPoint delta = globalPos - anchor_;
        if (!spaceHeld_) {
            delta = snapResizeDeltaToMonitorEdges(dragStartRect_, dragEdges_, delta, screenRects);
        }
        model.setRect(dragId_, resizedSelection(dragStartRect_, dragEdges_, delta, constraints, bounds));
    } else {
        return;
    }
    lastDragPos_ = globalPos;
    const Qt::Edges edges = currentDragEdges(globalPos);
    if (drag_ == Drag::Create || drag_ == Drag::Resize) {
        // A new selection may flip sides; the nudge follows the corner the pointer drags (#102).
        model.setNativeNudge(dragId_, nudgedMargins(dragStartNudge_, edges, nudgeTotal_));
    }
    session_->setDragLoupe(drag_ == Drag::Create || drag_ == Drag::Resize, globalPos, edges, nudgeZone_.active());
    session_->changed();
}

void CaptureOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const QPoint globalPos = toGlobalPoint(event->position().toPoint());
    if (session_->colorPickerActive()) {
        return;
    }
    if (session_->purpose() == CaptureSession::Purpose::Print && session_->annotationController().isDragging()) {
        session_->annotationController().mouseRelease(toGlobalLogical(event->position()), Qt::LeftButton,
                                                      event->modifiers());
        session_->persistAnnotationSettings();
        updateCursor(globalPos);
        return;
    }
    SelectionModel &model = session_->selections();
    const Drag drag = std::exchange(drag_, Drag::None);
    spaceHeld_ = false;
    session_->setDragLoupe(false);
    session_->setDragging(false);
    if (drag == Drag::Pending) {
        if (pendingId_) {
            model.activate(pendingId_);
        } else if (const PickTarget pick = session_->pickAt(globalPos); pick.kind != PickTarget::Kind::None) {
            // Nothing drawn yet: a click takes the window or monitor under the pointer (#97).
            session_->takePick(pick);
            return;
        } else {
            model.deactivate();
        }
    } else if (drag == Drag::Create || drag == Drag::Resize) {
        const Selection *selection = model.find(dragId_);
        if (selection && (selection->rect.width() < minimumAreaSize || selection->rect.height() < minimumAreaSize)) {
            if (drag == Drag::Resize) {
                model.setRect(dragId_, dragStartRect_); // a resize never deletes the rectangle
            } else {
                model.remove(dragId_);
            }
        } else if (drag == Drag::Create && session_->store().captureOnRelease
                   && session_->purpose() == CaptureSession::Purpose::Print
                   && session_->annotations().isEmpty() && !session_->markupVisible()) {
            session_->save(screen_.connector);
            return;
        }
    }
    session_->changed();
    updateCursor(globalPos);
}

void CaptureOverlay::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }
    if (session_->purpose() == CaptureSession::Purpose::Print) {
        if (session_->annotationController().mouseDoubleClick(toGlobalLogical(event->position()), Qt::LeftButton,
                                                              event->modifiers())) {
            return;
        }
    }
    SelectionModel &model = session_->selections();
    const QPoint globalPos = toGlobalPoint(event->position().toPoint());
    if (const int id = model.topmostAt(globalPos)) {
        model.activate(id);
        if (session_->purpose() == CaptureSession::Purpose::Print) {
            session_->save(screen_.connector);
        } else {
            session_->changed();
            session_->showAreaPanel();
        }
    }
}

void CaptureOverlay::focusOutEvent(QFocusEvent *event)
{
    if (textEditor_) {
        if (QWidget *focused = QApplication::focusWidget(); focused && textEditor_->isAncestorOf(focused)) {
            QWidget::focusOutEvent(event);
            return;
        }
    }
    spaceHeld_ = false;
    if (session_->purpose() == CaptureSession::Purpose::Print) {
        session_->annotationController().cancelActiveGesture();
    }
    QWidget::focusOutEvent(event);
}

void CaptureOverlay::enterEvent(QEnterEvent *event)
{
    // The pointer may arrive without moving (from another monitor, after a key): frame what is
    // under it right away (#97).
    const QPoint globalPos = toGlobalPoint(event->position().toPoint());
    lastPosition_ = globalPos;
    session_->notePointerPosition(globalPos);
    if (drag_ == Drag::None) {
        session_->setPointerPick(session_->pickAt(globalPos));
    }
    QWidget::enterEvent(event);
}

void CaptureOverlay::leaveEvent(QEvent *event)
{
    if (drag_ == Drag::None) {
        session_->setHoveredSelectionId(0);
        session_->setPointerPick({});
    }
    if (session_->purpose() == CaptureSession::Purpose::Print) {
        session_->annotationController().clearHover();
    }
    QWidget::leaveEvent(event);
}

bool CaptureOverlay::reapplyModifiers(QKeyEvent *event)
{
    const int key = event->key();
    if (key != Qt::Key_Shift && key != Qt::Key_Alt && key != Qt::Key_Control) {
        return false;
    }
    if (drag_ == Drag::Create || drag_ == Drag::Resize || drag_ == Drag::Move) {
        applyDrag(drag_ == Drag::Move ? lastPosition_ : lastDragPos_, dragModifiers(event->modifiers()));
    }
    updateCursor(lastPosition_);
    return false;
}

void CaptureOverlay::keyReleaseEvent(QKeyEvent *event)
{
    if (session_->purpose() == CaptureSession::Purpose::Print) {
        if (session_->annotationController().isEditingText()) {
            event->accept();
            return;
        }
        if (drag_ == Drag::None) {
            session_->annotationController().keyRelease(event);
        }
    }
    if (event->key() == Qt::Key_Space && spaceHeld_) {
        if (!event->isAutoRepeat()) {
            spaceHeld_ = false;
        }
        return;
    }
    reapplyModifiers(event);
    QWidget::keyReleaseEvent(event);
}

bool CaptureOverlay::handlePanelKey(int key, Qt::KeyboardModifiers modifiers)
{
    using Drawer = CaptureSession::PanelDrawer;
    const QString name = panelState();
    const PanelState state = name == QLatin1String("area")        ? PanelState::Area
                             : name == QLatin1String("selection") ? PanelState::Selection
                                                                  : PanelState::Start;
    const Drawer open = session_->panelDrawer();
    const bool listOpen =
        open == Drawer::Window || open == Drawer::Areas || open == Drawer::Record || open == Drawer::Monitors;
    const PanelKey result = panelKeyAction(state, listOpen, key, modifiers);
    switch (result.action) {
    case PanelKeyAction::None:
        return false;
    case PanelKeyAction::ThisMonitor:
        if (session_->recordMode()) {
            session_->record(screenTargetId(session_->panelConnector()));
        } else {
            session_->captureMonitor(session_->panelConnector()); // the monitor showing the panel
        }
        return true;
    case PanelKeyAction::Monitors:
        session_->togglePanelDrawer(Drawer::Monitors);
        return true;
    case PanelKeyAction::Window:
        session_->togglePanelDrawer(Drawer::Window);
        return true;
    case PanelKeyAction::Areas:
        session_->togglePanelDrawer(Drawer::Areas);
        return true;
    case PanelKeyAction::Delay:
        session_->togglePanelDrawer(Drawer::Delay);
        return true;
    case PanelKeyAction::More:
        session_->togglePanelDrawer(Drawer::More);
        return true;
    case PanelKeyAction::Copy:
        session_->copy(screen_.connector);
        return true;
    case PanelKeyAction::SaveAs:
        session_->saveAs();
        return true;
    case PanelKeyAction::SaveAsArea:
        session_->showAreaPanel();
        return true;
    case PanelKeyAction::Settings:
        session_->openSettings();
        return true;
    case PanelKeyAction::Record:
        if (session_->recordMode() && state == PanelState::Start) {
            return false; // the record-mode panel is the Record menu itself
        }
        if (state == PanelState::Selection) {
            session_->recordSelection();
        } else {
            session_->togglePanelDrawer(Drawer::Record);
        }
        return true;
    case PanelKeyAction::PickEntry:
        session_->pickPanelEntry(result.entry); // the panel may sit on another monitor's overlay
        return true;
    }
    return false;
}

QList<QAbstractButton *> CaptureOverlay::drawerEntries() const
{
    QList<QAbstractButton *> entries;
    QWidget *content = panel_ ? panel_->drawer() : nullptr;
    if (!content) {
        return entries;
    }
    for (QAbstractButton *button : content->findChildren<QAbstractButton *>()) {
        if (button->isVisibleTo(content) && button->isEnabled()) {
            entries.append(button);
        }
    }
    // Reading order: rows top to bottom, then along the row (mirrored right to left).
    const bool rtl = layoutDirection() == Qt::RightToLeft;
    std::stable_sort(entries.begin(), entries.end(), [content, rtl](QAbstractButton *a, QAbstractButton *b) {
        const QPoint pa = a->mapTo(content, QPoint(0, 0));
        const QPoint pb = b->mapTo(content, QPoint(0, 0));
        if (std::abs(pa.y() - pb.y()) > 4) {
            return pa.y() < pb.y();
        }
        return rtl ? pa.x() > pb.x() : pa.x() < pb.x();
    });
    return entries;
}

bool CaptureOverlay::navigatePanel(QKeyEvent *event)
{
    if ((event->modifiers() & ~Qt::KeypadModifier) != Qt::NoModifier) {
        return false;
    }
    if (session_->panelConnector() != screen_.connector) {
        // The keyboard is on another monitor's overlay; the panel decides.
        CaptureOverlay *host = session_->panelOverlay();
        return host && host != this && host->navigatePanel(event);
    }
    if (!panel_ || panel_->isHidden()) {
        return false;
    }
    PanelNavState state;
    for (const QPointer<PanelButton> &button : std::as_const(mainButtons_)) {
        state.mainOpensDrawer.append(button && button->opensDrawer());
    }
    const QList<QAbstractButton *> entries = drawerEntries();
    const CaptureSession::PanelDrawer open = session_->panelDrawer();
    if (open != CaptureSession::PanelDrawer::None && open != CaptureSession::PanelDrawer::Help) {
        state.drawerOwner = int(mainButtons_.indexOf(drawerButtons_.value(int(open))));
        state.entryCount = int(entries.size());
    }
    QWidget *focus = QApplication::focusWidget();
    if (const int index = int(mainButtons_.indexOf(qobject_cast<PanelButton *>(focus))); focus && index >= 0) {
        state.focus = PanelNavFocus::Main;
        state.mainIndex = index;
    } else if (const int entry = int(entries.indexOf(qobject_cast<QAbstractButton *>(focus))); focus && entry >= 0) {
        state.focus = PanelNavFocus::Entry;
        state.entryIndex = entry;
    } else if (focus && focus != this && panel_->isAncestorOf(focus)) {
        return false; // the Freeze/Live cap or a drawer control keeps its own keys
    }
    state.hasSelection = !session_->selections().isEmpty();

    const PanelNav nav = panelNavigate(state, event->key(), layoutDirection() == Qt::RightToLeft);
    const auto focusMain = [this](int index) {
        if (index >= 0 && index < mainButtons_.size() && mainButtons_.at(index)) {
            if (!isActiveWindow()) {
                activateWindow();
            }
            mainButtons_.at(index)->setFocus(Qt::TabFocusReason);
        }
    };
    const auto focusEntry = [this](int index) {
        const QList<QAbstractButton *> current = drawerEntries();
        if (current.isEmpty()) {
            return false;
        }
        QAbstractButton *entry = current.at(std::clamp(index, 0, int(current.size()) - 1));
        if (!isActiveWindow()) {
            activateWindow();
        }
        entry->setFocus(Qt::TabFocusReason);
        for (QWidget *w = entry->parentWidget(); w; w = w->parentWidget()) {
            if (auto *scroll = qobject_cast<QScrollArea *>(w)) {
                scroll->ensureWidgetVisible(entry);
                break;
            }
        }
        return true;
    };
    switch (nav.action) {
    case PanelNavAction::Pass:
        return false;
    case PanelNavAction::Swallow:
        return true;
    case PanelNavAction::FocusMain:
        focusMain(nav.index);
        return true;
    case PanelNavAction::FocusEntry:
        focusEntry(nav.index);
        return true;
    case PanelNavAction::OpenDrawer: {
        PanelButton *button = mainButtons_.value(nav.index);
        if (!button) {
            return true;
        }
        const int kind = drawerButtons_.key(button, -1);
        if (kind >= 0 && session_->panelDrawer() != CaptureSession::PanelDrawer(kind)) {
            session_->setPanelDrawer(CaptureSession::PanelDrawer(kind));
        }
        if (!focusEntry(0)) {
            focusMain(nav.index); // an empty drawer: stay on its button
        }
        return true;
    }
    case PanelNavAction::CloseDrawerFocusMain:
        // Move the keyboard out of the drawer before it goes away.
        focusMain(nav.index);
        session_->setPanelDrawer(CaptureSession::PanelDrawer::None);
        return true;
    case PanelNavAction::CloseDrawer:
        session_->setPanelDrawer(CaptureSession::PanelDrawer::None);
        return true;
    case PanelNavAction::ClickMain:
        if (PanelButton *button = mainButtons_.value(nav.index)) {
            button->click();
        }
        return true;
    case PanelNavAction::ClickEntry: {
        if (nav.index >= 0 && nav.index < entries.size()) {
            entries.at(nav.index)->click();
        }
        // A drawer that rebuilt itself (Delay) keeps the keyboard on the same entry.
        if (panel_ && panel_->drawer() && !panel_->isAncestorOf(QApplication::focusWidget())) {
            focusEntry(nav.index);
        }
        return true;
    }
    case PanelNavAction::CloseOverlay:
        session_->close();
        return true;
    }
    return false;
}

void CaptureOverlay::pickListEntry(int index)
{
    // A missing or unavailable entry is a no-op.
    if (index >= 0 && index < listEntries_.size() && listEntries_.at(index)) {
        const std::function<void()> action = listEntries_.at(index); // may rebuild the drawer
        action();
    }
}

void CaptureOverlay::keyPressEvent(QKeyEvent *event)
{
    if (session_->purpose() == CaptureSession::Purpose::Print && session_->annotationController().isEditingText()) {
        event->accept();
        return;
    }

    // #124: P pins the focused record entry (or the drawn rectangle) as the shortcut's default.
    if (session_->recordMode() && event->key() == Qt::Key_P
        && (event->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) {
        QString targetId;
        if (const QWidget *focus = QApplication::focusWidget()) {
            targetId = focus->property(kOverlayPinTargetProperty).toString();
        }
        if (targetId.isEmpty()) {
            targetId = session_->selectionRecordingTargetId();
        }
        if (!targetId.isEmpty()) {
            session_->toggleDefaultRecordingTarget(targetId);
            event->accept();
            return;
        }
    }

    if (session_->panelDrawer() == CaptureSession::PanelDrawer::SaveArea) {
        if (event->key() == Qt::Key_Escape) {
            session_->setPanelDrawer(CaptureSession::PanelDrawer::None);
            setFocus();
            return;
        }
        QWidget::keyPressEvent(event);
        return;
    }

    SelectionModel &model = session_->selections();

    // Crop-drag in progress: Space repositions, modifiers reshape, Escape aborts only the drag.
    if (drag_ != Drag::None) {
        if (event->key() == Qt::Key_Space) {
            if (!event->isAutoRepeat()) {
                spaceHeld_ = true;
            }
            return;
        }
        // Arrow keys move the dragged corner or edge by one native pixel, Shift by ten (#102).
        if ((drag_ == Drag::Create || drag_ == Drag::Resize)
            && (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right || event->key() == Qt::Key_Up
                || event->key() == Qt::Key_Down)) {
            const int step = event->modifiers().testFlag(Qt::ShiftModifier) ? 10 : 1;
            const QPoint delta = event->key() == Qt::Key_Left    ? QPoint(-step, 0)
                                 : event->key() == Qt::Key_Right ? QPoint(step, 0)
                                 : event->key() == Qt::Key_Up    ? QPoint(0, -step)
                                                                 : QPoint(0, step);
            const bool first = nudgeTotal_.isNull();
            nudgeTotal_ += delta;
            nudgeZone_.keyPressed(lastPosition_);
            if (first && event->modifiers().testFlag(Qt::ShiftModifier)) {
                applyDrag(lastDragPos_, dragModifiers(event->modifiers())); // Shift no longer squares
            }
            const Qt::Edges edges = currentDragEdges(lastDragPos_);
            model.setNativeNudge(dragId_, nudgedMargins(dragStartNudge_, edges, nudgeTotal_));
            session_->setDragLoupe(true, session_->dragCursorGlobal(), edges, true);
            session_->changed();
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            const Drag aborted = std::exchange(drag_, Drag::None);
            spaceHeld_ = false;
            session_->setDragLoupe(false);
            session_->setDragging(false);
            if (aborted == Drag::Create) {
                model.remove(dragId_);
            } else if (aborted == Drag::Resize || aborted == Drag::Move) {
                model.setRect(dragId_, dragStartRect_);
                model.setNativeNudge(dragId_, dragStartNudge_);
            }
            session_->changed();
            updateCursor(lastPosition_);
            return;
        }
        reapplyModifiers(event);
        return;
    }

    reapplyModifiers(event);

    const Qt::KeyboardModifiers modifiers = event->modifiers();
    const bool plainKey = !modifiers.testFlag(Qt::ControlModifier) && !modifiers.testFlag(Qt::AltModifier)
                       && !modifiers.testFlag(Qt::MetaModifier);

    if (session_->colorPickerActive()) {
        if (event->key() == Qt::Key_Tab) {
            session_->cycleColorFormat();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Escape || (plainKey && !event->isAutoRepeat() && event->key() == Qt::Key_I)) {
            session_->setColorPickerActive(false);
            updateCursor(lastPosition_);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            session_->pickColorAt(session_->pickerCursorGlobal(), modifiers.testFlag(Qt::AltModifier));
            updateCursor(lastPosition_);
            event->accept();
            return;
        }
        const int pickerStep = modifiers.testFlag(Qt::ShiftModifier) ? 10 : 1;
        QPoint pickerDelta;
        switch (event->key()) {
        case Qt::Key_Left:
            pickerDelta = {-pickerStep, 0};
            break;
        case Qt::Key_Right:
            pickerDelta = {pickerStep, 0};
            break;
        case Qt::Key_Up:
            pickerDelta = {0, -pickerStep};
            break;
        case Qt::Key_Down:
            pickerDelta = {0, pickerStep};
            break;
        default:
            event->accept();
            return;
        }
        session_->setPickerCursorGlobal(session_->pickerCursorGlobal() + pickerDelta);
        event->accept();
        return;
    }

    if (!event->isAutoRepeat() && handlePanelKey(event->key(), modifiers)) {
        event->accept();
        return;
    }

    if (session_->purpose() == CaptureSession::Purpose::Print && plainKey && !event->isAutoRepeat()) {
        switch (event->key()) {
        case Qt::Key_I:
            session_->setPickerCursorGlobal(lastPosition_);
            session_->setColorPickerActive(true);
            updateCursor(lastPosition_);
            return;
        case Qt::Key_P:
            session_->pin();
            return;
        case Qt::Key_O:
            session_->copyText(screen_.connector);
            return;
        case Qt::Key_V:
            session_->activateMarkupTool(ToolKind::Select);
            return;
        case Qt::Key_A:
            session_->activateMarkupTool(ToolKind::Arrow);
            return;
        case Qt::Key_R:
            session_->activateMarkupTool(ToolKind::Rectangle);
            return;
        case Qt::Key_E:
            session_->activateMarkupTool(ToolKind::Ellipse);
            return;
        case Qt::Key_B:
            session_->activateMarkupTool(ToolKind::Pen);
            return;
        case Qt::Key_M:
            session_->activateMarkupTool(ToolKind::Highlighter);
            return;
        case Qt::Key_T:
            session_->activateMarkupTool(ToolKind::Text);
            return;
        case Qt::Key_N:
            session_->activateMarkupTool(ToolKind::Step);
            return;
        case Qt::Key_X:
            session_->activateMarkupTool(ToolKind::Redact);
            return;
        case Qt::Key_S:
            session_->activateMarkupTool(ToolKind::Spotlight);
            return;
        default:
            break;
        }
    }

    if (session_->purpose() == CaptureSession::Purpose::Print && !event->isAutoRepeat()
        && event->key() == Qt::Key_E && modifiers == Qt::ControlModifier) {
        session_->editInWindow(screen_.connector);
        return;
    }

    if (session_->purpose() == CaptureSession::Purpose::Print) {
        if (session_->annotationController().keyPress(event)) {
            if (event->key() == Qt::Key_BracketLeft || event->key() == Qt::Key_BracketRight) {
                session_->persistAnnotationSettings();
            }
            updateCursor(lastPosition_);
            return;
        }
    }

    // Nothing in the panel has focus: without a selection the first arrow moves into it (#96).
    if (navigatePanel(event)) {
        return;
    }

    const int step = modifiers.testFlag(Qt::ShiftModifier) ? 10 : 1;
    const SelectionConstraints constraints{modifiers.testFlag(Qt::AltModifier),
                                           modifiers.testFlag(Qt::ControlModifier)};
    QPoint delta;
    switch (event->key()) {
    case Qt::Key_F1:
        session_->toggleHelp();
        return;
    case Qt::Key_Space:
        if (!event->isAutoRepeat()) {
            session_->setFrozen(!session_->frozen());
        }
        return;
    case Qt::Key_D:
        if (modifiers.testFlag(Qt::ControlModifier) && !modifiers.testFlag(Qt::AltModifier) && model.activeId()) {
            model.deactivate();
            session_->changed();
            return;
        }
        break;
    case Qt::Key_Escape:
        if (session_->forceNew()) {
            session_->setForceNew(false);
        } else if (session_->panelDrawer() != CaptureSession::PanelDrawer::None) {
            session_->setPanelDrawer(CaptureSession::PanelDrawer::None); // Escape closes the drawer first
        } else {
            session_->close();
        }
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (model.isEmpty()) {
            if (session_->purpose() == CaptureSession::Purpose::Print && !session_->annotations().isEmpty()) {
                session_->save(screen_.connector);
            }
            return;
        }
        if (session_->purpose() == CaptureSession::Purpose::Print && session_->textMode()) {
            session_->copyText(screen_.connector);
        } else if (session_->purpose() == CaptureSession::Purpose::Print && session_->recordMode()) {
            session_->recordSelection();
        } else if (session_->purpose() == CaptureSession::Purpose::Print) {
            session_->save(screen_.connector);
        } else {
            session_->showAreaPanel();
        }
        return;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        if (model.activeId()) {
            model.remove(model.activeId());
            session_->changed();
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
    const Selection *active = model.active();
    if (!active) {
        QWidget::keyPressEvent(event);
        return;
    }
    model.setRect(active->id, steppedSelection(active->rect, delta, constraints, minimumAreaSize,
                                               session_->desktopBounds()));
    session_->changed();
}

} // namespace itsova
