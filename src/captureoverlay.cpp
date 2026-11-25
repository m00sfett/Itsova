#include "captureoverlay.h"

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

namespace glimply
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

// A floating panel in the window colours of the desktop theme, with a soft shadow.
class OverlayPanel : public QWidget
{
public:
    explicit OverlayPanel(QWidget *parent)
        : QWidget(parent)
    {
        setAutoFillBackground(false);
        setAttribute(Qt::WA_StyledBackground, false);
        setContentsMargins(shadow, shadow, shadow, shadow + 2);
        setCursor(Qt::ArrowCursor);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF body = QRectF(rect()).adjusted(shadow, shadow, -shadow, -shadow - 2);
        for (int i = shadow; i > 0; --i) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, 6));
            painter.drawRoundedRect(body.adjusted(-i, -i + 2, i, i + 2), radius + i, radius + i);
        }
        painter.setBrush(palette().color(QPalette::Window));
        QColor border = palette().color(QPalette::WindowText);
        border.setAlpha(45);
        painter.setPen(QPen(border, 1));
        painter.drawRoundedRect(body.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    }

private:
    static constexpr int shadow = 6;
    static constexpr qreal radius = 8;
};

QToolButton *barButton(QWidget *parent, const QString &icon, const QString &text, const QString &tip)
{
    auto *button = new QToolButton(parent);
    button->setIcon(QIcon::fromTheme(icon));
    button->setText(text);
    button->setToolTip(tip);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

// Outlines an area on screen while the mouse is over its row in the Areas list.
class HoverOutline : public QObject
{
public:
    HoverOutline(QObject *parent, CaptureSession *session, QList<CaptureSession::Highlight> outline)
        : QObject(parent)
        , session_(session)
        , outline_(std::move(outline))
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Enter || event->type() == QEvent::HoverEnter) {
            session_->setHighlights(outline_);
        } else if (event->type() == QEvent::Leave || event->type() == QEvent::HoverLeave) {
            session_->setHighlights({});
        }
        return QObject::eventFilter(watched, event);
    }

private:
    CaptureSession *session_;
    QList<CaptureSession::Highlight> outline_;
};

QString elided(const QString &text, int length)
{
    return text.size() <= length ? text : text.left(length - 1) + QStringLiteral("…");
}

} // namespace

// ------------------------------------------------------------------------------------------ Session

CaptureSession::CaptureSession(App *app, Purpose purpose)
    : QObject(app)
    , app_(app)
    , purpose_(purpose)
    , annotationController_(&annotations_, this)
    , transientHintTimer_(new QTimer(this))
{
    transientHintTimer_->setSingleShot(true);
    transientHintTimer_->setInterval(2500);
    connect(transientHintTimer_, &QTimer::timeout, this, [this] {
        transientHint_.clear();
        changed();
    });
}

CaptureSession::~CaptureSession()
{
    for (const QPointer<CaptureOverlay> &overlay : std::as_const(overlays_)) {
        delete overlay.data();
    }
}

void CaptureSession::start()
{
    screens_ = currentScreens();
    if (screens_.isEmpty()) {
        finish({}, {}, {tr("No monitor is connected.")});
        return;
    }

    QRect desktopBounds;
    for (const ScreenInfo &screen : std::as_const(screens_)) {
        desktopBounds = desktopBounds.united(screen.geometry);
    }
    desktopBounds_ = desktopBounds;
    pointerConnector_ = screens_.first().connector;
    for (const ScreenInfo &screen : std::as_const(screens_)) {
        if (screen.geometry.contains(QCursor::pos())) {
            pointerConnector_ = screen.connector;
        }
    }
    annotationController_.setCropToolEnabled(false);
    annotationController_.setBounds(QRectF(desktopBounds));
    annotationController_.applySettings(store().annotations);

    connect(&annotations_, &AnnotationDocument::changed, this, &CaptureSession::changed);
    connect(&annotations_, &AnnotationDocument::selectionChanged, this, [this](int id) {
        if (id > 0) {
            selections_.deactivate();
        }
        changed();
    });
    connect(&annotationController_, &AnnotationController::previewChanged, this, &CaptureSession::changed);
    connect(&annotationController_, &AnnotationController::cursorChanged, this, &CaptureSession::changed);
    connect(&annotationController_, &AnnotationController::toolChanged, this, [this](ToolKind t) {
        if (t != ToolKind::Select) {
            selections_.deactivate();
        }
        changed();
    });
    connect(&annotationController_, &AnnotationController::textEditStarted, this,
            &CaptureSession::handleInlineTextRequested);

    frozen_ = forcedFrozen_.has_value() ? *forcedFrozen_ : store().overlayStartsFrozen();
    if (purpose_ == Purpose::Print && textMode_) {
        // Text mode writes no file (Q15); it only needs the frozen frames to read from.
        frozen_ = true;
        QStringList connectors;
        for (const ScreenInfo &screen : std::as_const(screens_)) {
            connectors.append(screen.connector);
        }
        captureFrames(connectors);
    } else if (purpose_ == Purpose::Print) {
        savePrintImage();
        if (finished_) {
            return;
        }
        if (!forceShowOverlay_ && !store().printOpensOverlay) {
            const ExportOptions printOpts = store().effectiveExportOptions(allMonitorsTargetId());
            app_->runPostCaptureHook(allMonitorsTargetId(), tr("All monitors"), {printPath()},
                                     QDateTime::currentDateTime(), formatConfigKey(printOpts.format),
                                     QStringLiteral("print"));
            finish({}, {printPath()});
            return;
        }
    } else if (frozen_) {
        QStringList connectors;
        for (const ScreenInfo &screen : std::as_const(screens_)) {
            connectors.append(screen.connector);
        }
        captureFrames(connectors);
    }

    // Every monitor gets its own top bar, so "This Monitor" always means the one you look at.
    for (const ScreenInfo &screen : std::as_const(screens_)) {
        auto *overlay = new CaptureOverlay(this, screen);
        overlays_.append(overlay);
        overlay->setAttribute(Qt::WA_DeleteOnClose);
        overlay->winId();
        if (QScreen *qscreen = findScreen(screen.connector)) {
            overlay->windowHandle()->setScreen(qscreen);
            overlay->setGeometry(qscreen->geometry());
        }
    }
    showOverlays();
}

void CaptureSession::savePrintImage()
{
    waitForPrintWrite(); // a previous Print press may still be writing its file
    QStringList connectors;
    for (const ScreenInfo &screen : std::as_const(screens_)) {
        connectors.append(screen.connector);
    }
    captureFrames(connectors);
    LatencyTrace::mark(LatencyTrace::Stage::Pixels);
    for (const ScreenInfo &screen : std::as_const(screens_)) {
        if (frames_.value(screen.connector).image.isNull()) {
            QString error;
            (void)composeAllMonitors(screens_, frames_, &error);
            finish({}, {}, {error.isEmpty() ? tr("The screen could not be captured.") : error});
            return;
        }
    }

    // Composing, encoding and writing take far longer than the capture itself, so they run in the
    // background while the overlay opens; everything that needs the file waits in printPath() (#67).
    const ExportOptions options = store().effectiveExportOptions(allMonitorsTargetId());
    const QString directory = store().effectiveCaptureDirectory();
    const QString filenameTemplate = store().effectiveFilenameTemplate();
    const QDateTime timestamp = QDateTime::currentDateTime();
    const QList<ScreenInfo> screens = screens_;
    const QHash<QString, CaptureResult> frames = frames_;
    printWritePending_ = true;
    printWrite_ = QtConcurrent::run([screens, frames, directory, filenameTemplate, options, timestamp] {
        PrintWrite result;
        QString error;
        result.image = composeAllMonitors(screens, frames, &error);
        if (result.image.isNull()) {
            result.errors.append(error.isEmpty() ? tr("The screen could not be captured.") : error);
            return result;
        }
        const OutputWriter writer(directory, filenameTemplate, options);
        QString path;
        if (writer.writeImage(result.image, allMonitorsTargetId(), timestamp, options, &path, &error)) {
            result.paths.append(path);
        } else {
            result.errors.append(error);
        }
        return result;
    });
    auto *watcher = new QFutureWatcher<PrintWrite>(this);
    connect(watcher, &QFutureWatcher<PrintWrite>::finished, this, [this, watcher] {
        watcher->deleteLater();
        applyPrintWrite();
    });
    watcher->setFuture(printWrite_);
}

void CaptureSession::waitForPrintWrite()
{
    if (printWritePending_) {
        printWrite_.waitForFinished();
        applyPrintWrite();
    }
}

void CaptureSession::applyPrintWrite()
{
    if (!printWritePending_) {
        return; // already applied by waitForPrintWrite()
    }
    printWritePending_ = false;
    const PrintWrite result = printWrite_.result();
    if (result.paths.isEmpty()) {
        finish({}, {}, result.errors);
        return;
    }
    if (!printPath_.isEmpty()) {
        QFile::remove(printPath_);
    }
    printPath_ = result.paths.first();
    LatencyTrace::mark(LatencyTrace::Stage::File);
    QGuiApplication::clipboard()->setImage(result.image);
    app_->recordCapture(result.paths, tr("All monitors"));
    finishLatencyBenchIfDone();
}

QString &CaptureSession::printPath()
{
    waitForPrintWrite();
    return printPath_;
}

void CaptureSession::printPressed()
{
    if (finished_ || purpose_ != Purpose::Print || textMode_) {
        return;
    }
    hideOverlays();
    QTimer::singleShot(liveHideDelayMs, this, [this] {
        savePrintImage();
        if (!finished_) {
            showOverlays();
        }
    });
}

void CaptureSession::finishLatencyBenchIfDone()
{
    if (finished_ || purpose_ != Purpose::Print || !qEnvironmentVariableIsSet("GLIMPLY_LATENCY_BENCH")) {
        return;
    }
    if (LatencyTrace::reached(LatencyTrace::Stage::Overlay) && LatencyTrace::reached(LatencyTrace::Stage::File)) {
        QTimer::singleShot(0, this, [this] { finish({}, printPath_.isEmpty() ? QStringList() : QStringList{printPath_}); });
    }
}

void CaptureSession::setForcedFrozen(std::optional<bool> frozen)
{
    forcedFrozen_ = frozen;
}

void CaptureSession::setForceShowOverlay(bool force)
{
    forceShowOverlay_ = force;
}

CaptureSession::Purpose CaptureSession::purpose() const
{
    return purpose_;
}

bool CaptureSession::frozen() const
{
    return frozen_;
}

bool CaptureSession::canSwitchToLive() const
{
    return annotations_.isEmpty() && !annotationController_.isEditingText();
}

QImage CaptureSession::frame(const QString &connector) const
{
    return frozen_ ? frames_.value(connector).image : QImage();
}

qreal CaptureSession::frameScale(const QString &connector) const
{
    const qreal scale = frames_.value(connector).scale;
    return scale > 0.0 ? scale : 1.0;
}

SelectionModel &CaptureSession::selections()
{
    return selections_;
}

TargetStore &CaptureSession::store() const
{
    return app_->store();
}

AnnotationDocument &CaptureSession::annotations()
{
    return annotations_;
}

const AnnotationDocument &CaptureSession::annotations() const
{
    return annotations_;
}

AnnotationController &CaptureSession::annotationController()
{
    return annotationController_;
}

const AnnotationController &CaptureSession::annotationController() const
{
    return annotationController_;
}

bool CaptureSession::markupVisible() const
{
    return panelDrawer_ == PanelDrawer::Markup;
}

void CaptureSession::setMarkupVisible(bool visible)
{
    if (purpose_ != Purpose::Print) {
        return;
    }
    if (visible) {
        setPanelDrawer(PanelDrawer::Markup);
    } else if (panelDrawer_ == PanelDrawer::Markup) {
        setPanelDrawer(PanelDrawer::None);
    }
}

void CaptureSession::activateMarkupTool(ToolKind tool)
{
    if (purpose_ != Purpose::Print) {
        return;
    }
    annotationController_.setTool(tool);
    if (panelDrawer_ != PanelDrawer::Markup) {
        setPanelDrawer(PanelDrawer::Markup);
    } else {
        changed();
    }
}

CaptureSession::PanelDrawer CaptureSession::panelDrawer() const
{
    return panelDrawer_;
}

void CaptureSession::setPanelDrawer(PanelDrawer drawer)
{
    if (drawer == panelDrawer_) {
        return;
    }
    if ((drawer == PanelDrawer::Markup || drawer == PanelDrawer::More) && purpose_ != Purpose::Print) {
        return;
    }
    if (drawer == PanelDrawer::SaveArea && selections_.isEmpty()) {
        return;
    }
    if (panelDrawer_ == PanelDrawer::Markup) {
        // Leaving Annotate: commit a text being typed and return to selecting rectangles.
        if (annotationController_.isEditingText()) {
            annotationController_.requestCommitActiveText(true);
        }
        annotationController_.setTool(ToolKind::Select);
    }
    if (panelDrawer_ == PanelDrawer::Window || panelDrawer_ == PanelDrawer::Areas) {
        highlights_.clear();
    }
    panelDrawer_ = drawer;
    if (drawer == PanelDrawer::Markup && !frozen_) {
        setFrozen(true); // markup draws on the frozen picture
        return;
    }
    changed();
}

void CaptureSession::togglePanelDrawer(PanelDrawer drawer)
{
    setPanelDrawer(panelDrawer_ == drawer ? PanelDrawer::None : drawer);
}

QString CaptureSession::panelConnector() const
{
    if (!manualPanelConnector_.isEmpty() && isConnected(manualPanelConnector_)) {
        return manualPanelConnector_;
    }
    if (!selections_.isEmpty()) {
        const Selection *selection = selections_.active();
        if (!selection) {
            selection = &selections_.stack().last();
        }
        return actionBarConnector(*selection);
    }
    return pointerConnector_;
}

void CaptureSession::notePointerOn(const QString &connector)
{
    if (connector == pointerConnector_) {
        return;
    }
    pointerConnector_ = connector;
    if (manualPanelConnector_.isEmpty() && selections_.isEmpty()) {
        changed();
    }
}

std::optional<QPoint> CaptureSession::panelPosition(const QString &connector) const
{
    const ScreenInfo *screen = screenFor(connector);
    if (!screen) {
        return std::nullopt;
    }
    const auto it = store().overlayPanelPositions.constFind(TargetStore::monitorKey(*screen));
    if (it == store().overlayPanelPositions.cend()) {
        return std::nullopt;
    }
    return *it;
}

void CaptureSession::setPanelPosition(const QString &connector, const std::optional<QPoint> &position)
{
    const ScreenInfo *screen = screenFor(connector);
    if (!screen) {
        return;
    }
    const QString key = TargetStore::monitorKey(*screen);
    if (position) {
        store().overlayPanelPositions.insert(key, *position);
    } else {
        store().overlayPanelPositions.remove(key);
    }
    app_->saveSettings();
    changed();
}

void CaptureSession::movePanelToMonitor(const QPoint &globalPointer, const QPoint &globalTopLeft)
{
    for (const ScreenInfo &screen : std::as_const(screens_)) {
        if (screen.geometry.contains(globalPointer)) {
            manualPanelConnector_ = screen.connector;
            // Kept on the monitor by the placement when the panel would stick out.
            setPanelPosition(screen.connector, globalTopLeft - screen.geometry.topLeft());
            return;
        }
    }
    changed();
}

void CaptureSession::persistAnnotationSettings()
{
    annotationController_.writeSettings(store().annotations);
    app_->saveSettings();
}

const ScreenInfo *CaptureSession::screenFor(const QString &connector) const
{
    for (const ScreenInfo &screen : screens_) {
        if (screen.connector == connector) {
            return &screen;
        }
    }
    return nullptr;
}

void CaptureSession::handleInlineTextRequested(const AnnotationItem &item, bool isExisting)
{
    const QPoint pos = item.rect.topLeft().toPoint();
    CaptureOverlay *target = nullptr;
    for (const QPointer<CaptureOverlay> &overlay : std::as_const(overlays_)) {
        if (overlay && overlay->screen().geometry.contains(pos)) {
            target = overlay.data();
            break;
        }
    }
    if (!target && !overlays_.isEmpty()) {
        target = overlays_.first().data();
    }
    for (const QPointer<CaptureOverlay> &overlay : std::as_const(overlays_)) {
        if (overlay && overlay != target) {
            overlay->stopInlineTextEdit();
        }
    }
    if (target) {
        target->activateWindow();
        target->startInlineTextEdit(item, isExisting);
    }
}

bool CaptureSession::forceNew() const
{
    return forceNew_;
}

void CaptureSession::setForceNew(bool on)
{
    forceNew_ = on;
    changed();
}

void CaptureSession::setDragging(bool dragging)
{
    if (dragging_ != dragging) {
        dragging_ = dragging;
        changed();
    }
}

bool CaptureSession::isDragging() const
{
    return dragging_;
}

bool CaptureSession::dragLoupeActive() const
{
    return dragLoupeActive_;
}

QPoint CaptureSession::dragCursorGlobal() const
{
    return dragCursorGlobal_;
}

void CaptureSession::setDragLoupe(bool active, const QPoint &globalPos)
{
    dragLoupeActive_ = active;
    if (active) {
        dragCursorGlobal_ = globalPos;
    }
}

bool CaptureSession::colorPickerActive() const
{
    return colorPickerActive_;
}

void CaptureSession::setColorPickerActive(bool active)
{
    if (purpose_ != Purpose::Print) {
        return;
    }
    if (colorPickerActive_ == active) {
        return;
    }
    if (active) {
        if (annotationController_.isEditingText()) {
            annotationController_.requestCommitActiveText(false);
        }
        transientHint_.clear();
        transientHintTimer_->stop();
        if (!desktopBounds_.contains(pickerCursorGlobal_)) {
            pickerCursorGlobal_ = QCursor::pos();
        }
        if (!desktopBounds_.isEmpty()) {
            pickerCursorGlobal_.setX(
                std::clamp(pickerCursorGlobal_.x(), desktopBounds_.left(), desktopBounds_.right()));
            pickerCursorGlobal_.setY(
                std::clamp(pickerCursorGlobal_.y(), desktopBounds_.top(), desktopBounds_.bottom()));
        }
        colorPickerActive_ = true;
        if (!frozen_) {
            setFrozen(true);
            return;
        }
    } else {
        colorPickerActive_ = false;
    }
    changed();
}

QPoint CaptureSession::pickerCursorGlobal() const
{
    return pickerCursorGlobal_;
}

void CaptureSession::setPickerCursorGlobal(const QPoint &globalPos)
{
    QPoint clamped = globalPos;
    if (!desktopBounds_.isEmpty()) {
        clamped.setX(std::clamp(clamped.x(), desktopBounds_.left(), desktopBounds_.right()));
        clamped.setY(std::clamp(clamped.y(), desktopBounds_.top(), desktopBounds_.bottom()));
    }
    if (pickerCursorGlobal_ != clamped) {
        pickerCursorGlobal_ = clamped;
        changed();
    }
}

void CaptureSession::cycleColorFormat()
{
    store().colorPickerFormat = nextColorFormat(store().colorPickerFormat);
    app_->saveSettings();
    changed();
}

void CaptureSession::pickColorAt(const QPoint &globalPos, bool applyToTool)
{
    if (purpose_ != Purpose::Print) {
        return;
    }
    setPickerCursorGlobal(globalPos);
    const ScreenInfo *target = nullptr;
    for (const ScreenInfo &s : std::as_const(screens_)) {
        if (s.geometry.contains(pickerCursorGlobal_)) {
            target = &s;
            break;
        }
    }
    if (!target && !screens_.isEmpty()) {
        target = &screens_.first();
    }
    QColor sampled;
    if (target) {
        const CaptureResult cap = frames_.value(target->connector);
        if (!cap.image.isNull()) {
            const qreal scale = cap.scale > 0.0 ? cap.scale : 1.0;
            const QPoint nativePx = logicalToNativePixel(
                QPointF(pickerCursorGlobal_), QPointF(target->geometry.topLeft()), scale, cap.image.size());
            sampled = sampleNativePixel(cap.image, nativePx);
        }
    }
    colorPickerActive_ = false;
    if (sampled.isValid()) {
        const QString formatted = formatColor(sampled, store().colorPickerFormat);
        if (QGuiApplication::clipboard()) {
            QGuiApplication::clipboard()->setText(formatted);
        }
        if (applyToTool) {
            switch (annotationController_.tool()) {
            case ToolKind::Highlighter:
                annotationController_.setHighlighterColor(sampled);
                break;
            case ToolKind::Text:
                annotationController_.setTextColor(sampled);
                break;
            case ToolKind::Redact:
                annotationController_.setRedactionColor(sampled);
                break;
            default:
                annotationController_.setStrokeColor(sampled);
                break;
            }
            persistAnnotationSettings();
            transientHint_ = tr("Applied %1 and copied to clipboard").arg(formatted);
        } else {
            transientHint_ = tr("Copied %1 to clipboard").arg(formatted);
        }
        transientHintTimer_->start();
    }
    changed();
}

int CaptureSession::hoveredSelectionId() const
{
    return hoveredSelectionId_;
}

void CaptureSession::setHoveredSelectionId(int id)
{
    if (hoveredSelectionId_ != id) {
        hoveredSelectionId_ = id;
        changed();
    }
}

QRect CaptureSession::desktopBounds() const
{
    return desktopBounds_;
}

QList<QRect> CaptureSession::screenRects() const
{
    QList<QRect> rects;
    rects.reserve(screens_.size());
    for (const ScreenInfo &screen : screens_) {
        rects.append(screen.geometry);
    }
    return rects;
}

QList<ScreenInfo> CaptureSession::intersectingScreens(const QRect &globalRect) const
{
    return glimply::intersectingScreens(screens_, globalRect);
}

QStringList CaptureSession::intersectingConnectors(const QRect &globalRect) const
{
    QStringList connectors;
    for (const ScreenInfo &screen : screens_) {
        if (!screen.geometry.intersected(globalRect).isEmpty()) {
            connectors.append(screen.connector);
        }
    }
    return connectors;
}

QString CaptureSession::actionBarConnector(const Selection &selection) const
{
    const QRect &r = selection.rect;
    for (const QPoint &pt : {r.bottomRight(), r.bottomLeft(), r.topRight(), r.topLeft()}) {
        for (const ScreenInfo &screen : screens_) {
            if (screen.geometry.contains(pt)) {
                return screen.connector;
            }
        }
    }
    QString bestConnector = screens_.isEmpty() ? QString() : screens_.first().connector;
    qint64 bestArea = -1;
    for (const ScreenInfo &screen : screens_) {
        const QRect overlap = screen.geometry.intersected(r);
        if (!overlap.isEmpty()) {
            const qint64 area = qint64(overlap.width()) * overlap.height();
            if (area > bestArea) {
                bestArea = area;
                bestConnector = screen.connector;
            }
        }
    }
    return bestConnector;
}

void CaptureSession::showAreaPanel()
{
    if (selections_.isEmpty()) {
        return;
    }
    setPanelDrawer(PanelDrawer::SaveArea);
}

void CaptureSession::dismissAreaPanels()
{
    if (panelDrawer_ == PanelDrawer::SaveArea) {
        setPanelDrawer(PanelDrawer::None);
    }
}

QString CaptureSession::hint() const
{
    if (colorPickerActive_) {
        return tr("Click to copy color, Alt+Click to apply to active markup tool, Tab switches format, Escape "
                  "exits.");
    }
    if (dragging_) {
        return tr("Shift keeps a square, Alt grows from the centre, Space repositions while dragging.");
    }
    if (!transientHint_.isEmpty()) {
        return transientHint_;
    }
    if (annotationController_.isEditingText()) {
        return tr("Type text, drag outside frame edge to move; Escape or Ctrl+Enter commits.");
    }
    if (annotationController_.tool() != ToolKind::Select) {
        return tr("Drag to draw (Ctrl temporarily selects; Shift snaps, Alt centres, Space moves; V returns).");
    }
    if (forceNew_) {
        return tr("Drag the next rectangle anywhere, also over another one.");
    }
    if (selections_.isEmpty()) {
        if (!annotations_.isEmpty()) {
            return tr("Enter saves the annotated capture, or draw a crop rectangle first. Escape closes.");
        }
        return purpose_ == Purpose::Print
                   ? tr("Drag a rectangle, click a monitor to capture it, or open Annotate. Escape keeps the full "
                        "screenshot.")
                   : tr("Drag a rectangle for the new area. Escape cancels.");
    }
    if (!selections_.active()) {
        return tr("Click a rectangle to change it, or drag a new one.");
    }
    return purpose_ == Purpose::Print
               ? tr("Drag inside to move, at the edges to resize. Drag elsewhere for another rectangle. Enter saves.")
               : tr("Drag inside to move, at the edges to resize. Drag elsewhere for another rectangle.");
}

const QList<CaptureSession::Highlight> &CaptureSession::highlights() const
{
    return highlights_;
}

void CaptureSession::setHighlights(const QList<Highlight> &highlights)
{
    highlights_ = highlights;
    changed();
}

QList<WindowInfo> CaptureSession::windows() const
{
    return listWindows(QCoreApplication::applicationPid());
}

QKeySequence CaptureSession::shortcut(const QString &id) const
{
    return app_->shortcuts()->shortcut(id);
}

bool CaptureSession::isConnected(const QString &connector) const
{
    return std::any_of(screens_.cbegin(), screens_.cend(), [&](const ScreenInfo &s) { return s.connector == connector; });
}

QString CaptureSession::monitorName(const QString &connector) const
{
    const auto screen = std::find_if(screens_.cbegin(), screens_.cend(),
                                     [&](const ScreenInfo &s) { return s.connector == connector; });
    return screen == screens_.cend() ? connector : store().monitorName(*screen);
}

void CaptureSession::changed()
{
    // A drawer whose button the current main row no longer has closes (Q25); the area form needs a
    // rectangle.
    if (purpose_ == Purpose::Print && !selections_.isEmpty()
        && (panelDrawer_ == PanelDrawer::Window || panelDrawer_ == PanelDrawer::Areas
            || panelDrawer_ == PanelDrawer::Delay)) {
        panelDrawer_ = PanelDrawer::None;
        highlights_.clear();
    }
    if (panelDrawer_ == PanelDrawer::SaveArea && selections_.isEmpty()) {
        panelDrawer_ = PanelDrawer::None;
    }
    for (const QPointer<CaptureOverlay> &overlay : std::as_const(overlays_)) {
        if (overlay) {
            overlay->refresh();
        }
    }
}

void CaptureSession::captureFrames(const QStringList &connectors)
{
    QHash<QString, CaptureResult> captured;
    std::mutex mutex;
    // All monitors at once, so the frames show (nearly) the same instant.
    QtConcurrent::blockingMap(connectors, [&](const QString &connector) {
        CaptureResult frame = CaptureService::captureScreen(connector);
        const std::lock_guard lock(mutex);
        captured.insert(connector, std::move(frame));
    });
    for (auto it = captured.cbegin(); it != captured.cend(); ++it) {
        frames_.insert(it.key(), it.value());
    }
}

void CaptureSession::withFrames(const QStringList &connectors, const std::function<void()> &next)
{
    if (frozen_) {
        next();
        return;
    }
    hideOverlays();
    QTimer::singleShot(liveHideDelayMs, this, [this, connectors, next] {
        captureFrames(connectors);
        next();
        if (!finished_) {
            showOverlays();
        }
    });
}

void CaptureSession::showOverlays()
{
    for (const QPointer<CaptureOverlay> &overlay : std::as_const(overlays_)) {
        if (overlay) {
            overlay->refresh();
            overlay->showFullScreen();
        }
    }
    for (const QPointer<CaptureOverlay> &overlay : std::as_const(overlays_)) {
        if (overlay && overlay->geometry().contains(QCursor::pos())) {
            overlay->activateWindow();
            overlay->setFocus();
        }
    }
}

void CaptureSession::hideOverlays()
{
    for (const QPointer<CaptureOverlay> &overlay : std::as_const(overlays_)) {
        if (overlay) {
            overlay->hide();
        }
    }
}

QStringList CaptureSession::write(const QList<QPair<QString, QImage>> &images, QStringList *errors)
{
    return write(images, errors, store().exportOptions);
}

QStringList CaptureSession::write(const QList<QPair<QString, QImage>> &images, QStringList *errors,
                                  const ExportOptions &options)
{
    const OutputWriter writer(store().effectiveCaptureDirectory(), store().effectiveFilenameTemplate(), options);
    const QDateTime timestamp = QDateTime::currentDateTime();
    QStringList paths;
    for (const auto &[token, image] : images) {
        QString path;
        QString error;
        if (writer.writeImage(image, token, timestamp, options, &path, &error)) {
            paths.append(path);
        } else {
            errors->append(error);
        }
    }
    return paths;
}

QStringList CaptureSession::writeOutputs(const QList<RenderedOutput> &outputs, QStringList *errors)
{
    const OutputWriter writer(store().effectiveCaptureDirectory(), store().effectiveFilenameTemplate(),
                              store().exportOptions);
    const QDateTime timestamp = QDateTime::currentDateTime();
    QStringList paths;
    for (const RenderedOutput &output : outputs) {
        QString path;
        QString error;
        if (writer.writeImage(output.image, output.token, timestamp, output.exportOptions, &path, &error)) {
            paths.append(path);
        } else {
            errors->append(error);
        }
    }
    return paths;
}

QImage CaptureSession::cropSelectionImage(const Selection &selection, bool includeAnnotations, qreal *outScale,
                                            QString *error) const
{
    const QList<ScreenInfo> hit = intersectingScreens(selection.rect);
    if (hit.isEmpty()) {
        if (error) {
            *error = tr("Selection does not overlap any connected monitor.");
        }
        return {};
    }

    QImage cropped;
    qreal scale = 1.0;
    if (hit.size() == 1 && hit.first().geometry.contains(selection.rect)) {
        const ScreenInfo &screen = hit.first();
        const CaptureResult frame = frames_.value(screen.connector);
        if (frame.image.isNull()) {
            if (error) {
                *error = frame.error.isEmpty() ? tr("Monitor %1 could not be captured.").arg(screen.connector)
                                               : frame.error;
            }
            return {};
        }
        cropped = cropLogicalArea(frame.image, selection.rect.translated(-screen.geometry.topLeft()), frame.scale, error);
        scale = frame.scale > 0.0 ? frame.scale : 1.0;
    } else {
        cropped = composeMonitorsCrop(hit, frames_, selection.rect, &scale, error);
    }

    if (outScale) {
        *outScale = scale;
    }
    if (!includeAnnotations || cropped.isNull() || annotations_.isEmpty()) {
        return cropped;
    }
    return renderAnnotatedCrop(cropped, QRectF(selection.rect), scale, annotations_);
}

QImage CaptureSession::selectionImage(const Selection &selection, QString *error) const
{
    return cropSelectionImage(selection, true, nullptr, error);
}

void CaptureSession::removePrintSnapshotIfRedacted()
{
    if (!shouldDeletePrintFileOnCopy(annotations_, printPath())) {
        return;
    }
    const QString oldPath = printPath();
    if (purgeUnredactedPrintFile(oldPath, false)) {
        app_->forgetCapturePath(oldPath);
        printPath().clear();
    }
}

void CaptureSession::save(const QString &preferredConnector)
{
    if (annotationController_.isEditingText()) {
        annotationController_.requestCommitActiveText(false);
    }
    if (selections_.isEmpty()) {
        if (purpose_ == Purpose::Print && !annotations_.isEmpty() && !screens_.isEmpty()) {
            const QList<QString> hit = annotatedConnectors(screens_, annotations_);
            if (hit.size() > 1) {
                captureAllMonitors();
            } else {
                const QString target = hit.size() == 1 ? hit.first()
                                     : (!preferredConnector.isEmpty() ? preferredConnector : screens_.first().connector);
                captureMonitor(target);
            }
        }
        return;
    }
    QStringList connectors;
    for (const Selection &selection : selections_.stack()) {
        for (const QString &conn : intersectingConnectors(selection.rect)) {
            if (!connectors.contains(conn)) {
                connectors.append(conn);
            }
        }
    }
    withFrames(connectors, [this] {
        const QList<Selection> ordered = selections_.inDrawingOrder();
        QList<QPair<QString, QImage>> images;
        QStringList errors;
        QImage clipboard;
        for (const Selection &selection : ordered) {
            QString error;
            const QImage image = selectionImage(selection, &error);
            if (image.isNull()) {
                errors.append(error);
                continue;
            }
            const QString token = ordered.size() == 1
                                      ? QStringLiteral("selection")
                                      : QStringLiteral("selection-%1").arg(selections_.number(selection.id));
            images.append({token, image});
            if (selection.id == selections_.activeId() || clipboard.isNull()) {
                clipboard = image;
            }
        }
        finishWith(write(images, &errors), clipboard, errors, tr("Selection"));
    });
}

void CaptureSession::copy(const QString &preferredConnector)
{
    if (annotationController_.isEditingText()) {
        annotationController_.requestCommitActiveText(false);
    }
    const Selection *active = selections_.active();
    if (!active && !selections_.isEmpty()) {
        active = &selections_.stack().last();
    }
    if (!active) {
        if (purpose_ != Purpose::Print || annotations_.isEmpty() || screens_.isEmpty()) {
            return;
        }
        const QList<QString> hit = annotatedConnectors(screens_, annotations_);
        QStringList connectors;
        const bool allMonitors = hit.size() > 1;
        QString targetConnector;
        if (allMonitors) {
            for (const ScreenInfo &screen : std::as_const(screens_)) {
                connectors.append(screen.connector);
            }
        } else {
            targetConnector = hit.size() == 1 ? hit.first()
                                              : (!preferredConnector.isEmpty() ? preferredConnector
                                                                               : screens_.first().connector);
            connectors.append(targetConnector);
        }
        withFrames(connectors, [this, allMonitors, targetConnector] {
            QString error;
            QImage image;
            if (allMonitors) {
                image = renderAnnotatedAllMonitors(screens_, frames_, annotations_, &error);
            } else if (const ScreenInfo *screen = screenFor(targetConnector)) {
                const CaptureResult frame = frames_.value(targetConnector);
                if (frame.image.isNull()) {
                    error = frame.error.isEmpty() ? tr("Monitor %1 could not be captured.").arg(targetConnector)
                                                  : frame.error;
                } else {
                    image = renderAnnotatedMonitor(*screen, frame, annotations_);
                }
            }
            if (image.isNull()) {
                finish({}, {}, {error});
                return;
            }
            QGuiApplication::clipboard()->setImage(image);
            removePrintSnapshotIfRedacted();
            finish(tr("Copied to the clipboard"), printPath().isEmpty() ? QStringList() : QStringList{printPath()});
        });
        return;
    }
    const Selection selection = *active;
    const QStringList connectors = intersectingConnectors(selection.rect);
    withFrames(connectors, [this, selection] {
        QString error;
        const QImage image = selectionImage(selection, &error);
        if (image.isNull()) {
            finish({}, {}, {error});
            return;
        }
        QGuiApplication::clipboard()->setImage(image);
        removePrintSnapshotIfRedacted();
        finish(tr("Copied to the clipboard"), printPath().isEmpty() ? QStringList() : QStringList{printPath()});
    });
}

void CaptureSession::pin()
{
    if (annotationController_.isEditingText()) {
        annotationController_.requestCommitActiveText(false);
    }
    if (selections_.isEmpty()) {
        return;
    }
    QStringList connectors;
    for (const Selection &selection : selections_.stack()) {
        for (const QString &conn : intersectingConnectors(selection.rect)) {
            if (!connectors.contains(conn)) {
                connectors.append(conn);
            }
        }
    }
    withFrames(connectors, [this] {
        QStringList errors;
        int pinned = 0;
        for (const Selection &selection : selections_.inDrawingOrder()) {
            QString error;
            const QImage image = selectionImage(selection, &error);
            if (image.isNull()) {
                errors.append(error);
                continue;
            }
            // Pins carry the annotations and write no file or history entry (Q14, Q15).
            app_->pinImage(image, selection.rect);
            ++pinned;
        }
        removePrintSnapshotIfRedacted();
        finish(pinned > 0 ? tr("Pinned to the screen") : QString(),
               printPath().isEmpty() ? QStringList() : QStringList{printPath()}, errors);
    });
}

void CaptureSession::setTextMode(bool on)
{
    textMode_ = on;
}

bool CaptureSession::textMode() const
{
    return textMode_;
}

void CaptureSession::copyText(const QString &preferredConnector)
{
    if (annotationController_.isEditingText()) {
        annotationController_.requestCommitActiveText(false);
    }
    if (screens_.isEmpty()) {
        return;
    }
    QStringList connectors;
    if (selections_.isEmpty()) {
        connectors.append(!preferredConnector.isEmpty() ? preferredConnector : screens_.first().connector);
    } else {
        for (const Selection &selection : selections_.stack()) {
            for (const QString &conn : intersectingConnectors(selection.rect)) {
                if (!connectors.contains(conn)) {
                    connectors.append(conn);
                }
            }
        }
    }
    withFrames(connectors, [this, connectors] {
        QList<QImage> images;
        QStringList errors;
        if (selections_.isEmpty()) {
            const CaptureResult frame = frames_.value(connectors.first());
            if (frame.image.isNull()) {
                errors.append(frame.error.isEmpty() ? tr("Monitor %1 could not be captured.").arg(connectors.first())
                                                    : frame.error);
            } else {
                images.append(frame.image);
            }
        } else {
            // OCR reads the picture without annotations, in reading order (Q14).
            const QList<Selection> stack = selections_.stack();
            QList<QRect> rects;
            for (const Selection &selection : stack) {
                rects.append(selection.rect);
            }
            for (int index : ocrReadingOrder(rects)) {
                QString error;
                const QImage image = cropSelectionImage(stack.at(index), false, nullptr, &error);
                if (image.isNull()) {
                    errors.append(error);
                } else {
                    images.append(image);
                }
            }
        }
        app_->copyTextFromImages(images);
        // The text notification comes from the recognition; only failures to read pixels show here.
        finish(QString(), QStringList(), errors);
    });
}

void CaptureSession::saveAs()
{
    if (annotationController_.isEditingText()) {
        annotationController_.requestCommitActiveText(false);
    }
    if (selections_.isEmpty()) {
        return;
    }
    QStringList connectors;
    for (const Selection &selection : selections_.stack()) {
        for (const QString &conn : intersectingConnectors(selection.rect)) {
            if (!connectors.contains(conn)) {
                connectors.append(conn);
            }
        }
    }
    // The file dialog needs the screen; the frames are taken before the overlays step aside.
    const bool wasFrozen = frozen_;
    const auto ask = [this, wasFrozen] {
        hideOverlays();
        QStringList errors;
        QList<QPair<int, QImage>> renderedSelections;
        QImage preview;
        const QList<Selection> ordered = selections_.inDrawingOrder();
        for (const Selection &selection : ordered) {
            QString error;
            const QImage image = selectionImage(selection, &error);
            if (image.isNull()) {
                errors.append(error);
                continue;
            }
            renderedSelections.append({selections_.number(selection.id), image});
            if (selection.id == selections_.activeId() || preview.isNull()) {
                preview = image;
            }
        }
        if (renderedSelections.isEmpty()) {
            finish({}, {}, errors);
            return;
        }

        const ExportOptions initialOptions = store().exportOptions;
        const QString suggestion = QDir(store().effectiveCaptureDirectory())
                                       .filePath(filenameFor(store().effectiveFilenameTemplate(),
                                                             QStringLiteral("selection"), QDateTime::currentDateTime(),
                                                             initialOptions.format));
        ExportDialog dialog(preview, suggestion, initialOptions);
        if (dialog.exec() != QDialog::Accepted) {
            if (!wasFrozen) {
                frames_.clear();
            }
            showOverlays();
            return;
        }

        const QString chosenPath = dialog.filePath();
        const ExportOptions chosenOptions = dialog.exportOptions();
        const QFileInfo info(chosenPath);
        const QString stem = info.dir().filePath(info.completeBaseName());
        const QString extension = canonicalExtension(chosenOptions.format);
        QStringList paths;
        for (const auto &[number, image] : std::as_const(renderedSelections)) {
            const QString target = number <= 1
                                       ? stem + QLatin1Char('.') + extension
                                       : QStringLiteral("%1-%2.%3").arg(stem).arg(number).arg(extension);
            QString writeErr;
            if (!writeImageToFile(target, image, chosenOptions, &writeErr)) {
                errors.append(tr("Could not write %1: %2").arg(target, writeErr));
                continue;
            }
            paths.append(target);
        }
        finishWith(paths, preview, errors, tr("Selection"));
    };
    if (frozen_) {
        ask();
        return;
    }
    hideOverlays();
    QTimer::singleShot(liveHideDelayMs, this, [this, connectors, ask] {
        captureFrames(connectors);
        ask();
    });
}

QString CaptureSession::saveAsAreas(const QString &name, const QKeySequence &shortcut, bool combine, int delaySeconds)
{
    const QList<Selection> ordered = selections_.inDrawingOrder();
    if (ordered.isEmpty()) {
        return tr("Draw a rectangle first.");
    }
    TargetStore &targets = store();
    QStringList connectors;
    for (const ScreenInfo &screen : std::as_const(screens_)) {
        connectors.append(screen.connector);
    }
    const bool named = ordered.size() == 1 || combine;
    const QString trimmed = name.trimmed();
    if (named) {
        if (trimmed.isEmpty() || (ordered.size() == 1 && sanitizeFilenameToken(trimmed).isEmpty())) {
            return tr("Enter a name that can be used in a file name.");
        }
        if (!targets.isNameAvailable(trimmed, QString(), connectors)) {
            return tr("“%1” is already used by another area or group.").arg(trimmed);
        }
    }

    const int clampedDelay = std::clamp(delaySeconds, 0, 60);
    QStringList areaIds;
    for (const Selection &selection : ordered) {
        if (intersectingScreens(selection.rect).isEmpty()) {
            continue;
        }
        Area &area = targets.addAreaForGlobalRect(screens_, selection.rect);
        if (ordered.size() == 1) {
            area.name = trimmed;
            area.delaySeconds = clampedDelay;
        } else if (!combine) {
            area.delaySeconds = clampedDelay;
        }
        areaIds.append(area.id);
    }
    QString shortcutOwner = areaIds.size() == 1 ? areaIds.first() : QString();
    if (ordered.size() > 1 && combine) {
        Group &group = targets.addGroup(trimmed);
        group.targetIds = areaIds;
        group.delaySeconds = clampedDelay;
        shortcutOwner = group.id;
    }
    app_->storeChanged();
    if (!shortcut.isEmpty() && !shortcutOwner.isEmpty()) {
        app_->shortcuts()->setShortcut(shortcutOwner, shortcut);
    }

    const QString saved = ordered.size() == 1 ? tr("Area “%1” saved").arg(trimmed)
                          : combine           ? tr("Group “%1” saved").arg(trimmed)
                                              : tr("%n areas saved", nullptr, int(areaIds.size()));
    finish(saved, printPath().isEmpty() ? QStringList() : QStringList{printPath()});
    return {};
}

void CaptureSession::editInWindow(const QString &preferredConnector)
{
    if (finished_ || purpose_ != Purpose::Print) {
        return;
    }
    if (annotationController_.isEditingText()) {
        annotationController_.requestCommitActiveText(false);
    }

    const Selection *active = selections_.active();
    if (!active && !selections_.isEmpty()) {
        active = &selections_.stack().last();
    }

    QStringList connectors;
    bool allMonitors = false;
    QString targetConnector;
    Selection chosenSelection;
    const bool hasSelection = (active != nullptr);

    if (hasSelection) {
        chosenSelection = *active;
        connectors = intersectingConnectors(chosenSelection.rect);
    } else {
        if (screens_.isEmpty()) {
            return;
        }
        const QList<QString> hit = annotatedConnectors(screens_, annotations_);
        allMonitors = hit.size() > 1;
        if (allMonitors) {
            for (const ScreenInfo &screen : std::as_const(screens_)) {
                connectors.append(screen.connector);
            }
        } else {
            targetConnector = hit.size() == 1 ? hit.first()
                                              : (!preferredConnector.isEmpty() ? preferredConnector
                                                                               : screens_.first().connector);
            connectors.append(targetConnector);
        }
    }

    withFrames(connectors, [this, hasSelection, chosenSelection, allMonitors, targetConnector] {
        if (finished_) {
            return;
        }
        QString error;
        QImage rawImage;
        qreal scale = 1.0;
        QPoint globalOrigin;
        QRectF clipBounds;
        QString baseToken;

        if (hasSelection) {
            rawImage = cropSelectionImage(chosenSelection, false, &scale, &error);
            if (!rawImage.isNull()) {
                scale = scale > 0.0 ? scale : 1.0;
                globalOrigin = chosenSelection.rect.topLeft();
                clipBounds = QRectF(chosenSelection.rect);
                baseToken = QStringLiteral("selection");
            }
        } else if (allMonitors) {
            rawImage = composeAllMonitors(screens_, frames_, &error);
            if (!rawImage.isNull()) {
                QRect bounds;
                qreal maxScale = 0.0;
                for (const ScreenInfo &screen : std::as_const(screens_)) {
                    bounds = bounds.united(screen.geometry);
                    maxScale = std::max(maxScale, frames_.value(screen.connector).scale);
                }
                scale = maxScale > 0.0 ? maxScale : 1.0;
                globalOrigin = bounds.topLeft();
                clipBounds = QRectF(bounds);
                baseToken = allMonitorsTargetId();
            }
        } else {
            const CaptureResult frame = frames_.value(targetConnector);
            if (frame.image.isNull()) {
                error = frame.error.isEmpty() ? tr("Monitor %1 could not be captured.").arg(targetConnector)
                                              : frame.error;
            } else {
                rawImage = frame.image;
                scale = frame.scale > 0.0 ? frame.scale : 1.0;
                const ScreenInfo *screen = screenFor(targetConnector);
                globalOrigin = screen ? screen->geometry.topLeft() : QPoint();
                clipBounds = screen ? QRectF(screen->geometry) : QRectF();
                baseToken = monitorName(targetConnector);
            }
        }

        if (rawImage.isNull()) {
            finish({}, {}, {error.isEmpty() ? tr("The screen could not be captured.") : error});
            return;
        }

        auto handoffDoc = annotations_.cloneTranslated(-QPointF(globalOrigin), clipBounds);
        if (!printPath().isEmpty()) {
            const QString oldPrint = printPath();
            purgeUnredactedPrintFile(oldPrint, annotations_.hasRedactions());
            app_->forgetCapturePath(oldPrint);
            printPath().clear();
        }

        finished_ = true;
        for (const QPointer<CaptureOverlay> &overlay : std::as_const(overlays_)) {
            if (overlay) {
                overlay->close();
            }
        }
        overlays_.clear();

        app_->openEditorFromCapture(rawImage, scale, std::move(handoffDoc), baseToken);
        Q_EMIT finished();
    });
}

void CaptureSession::captureMonitor(const QString &connector)
{
    if (annotationController_.isEditingText()) {
        annotationController_.requestCommitActiveText(false);
    }
    withFrames({connector}, [this, connector] {
        const CaptureResult frame = frames_.value(connector);
        if (frame.image.isNull()) {
            finish({}, {}, {frame.error.isEmpty() ? tr("Monitor %1 could not be captured.").arg(connector) : frame.error});
            return;
        }
        const ScreenInfo *screen = screenFor(connector);
        const QImage out = screen ? renderAnnotatedMonitor(*screen, frame, annotations_) : frame.image;
        QStringList errors;
        const QString name = monitorName(connector);
        const ExportOptions opts = store().effectiveExportOptions(screenTargetId(connector));
        finishWith(write({{name, out}}, &errors, opts), out, errors, name, screenTargetId(connector));
    });
}

void CaptureSession::captureAllMonitors()
{
    if (annotationController_.isEditingText()) {
        annotationController_.requestCommitActiveText(false);
    }
    if (shouldBypassRawPrintFastPath(frozen_, printPath(), annotations_)) {
        // The full screenshot of this moment is already saved without edits.
        const ExportOptions opts = store().effectiveExportOptions(allMonitorsTargetId());
        app_->runPostCaptureHook(allMonitorsTargetId(), tr("All monitors"), {printPath()},
                                 QDateTime::currentDateTime(), formatConfigKey(opts.format),
                                 QStringLiteral("overlay"));
        finish({}, {printPath()});
        return;
    }
    QStringList connectors;
    for (const ScreenInfo &screen : std::as_const(screens_)) {
        connectors.append(screen.connector);
    }
    withFrames(connectors, [this] {
        QString error;
        const QImage image = renderAnnotatedAllMonitors(screens_, frames_, annotations_, &error);
        if (image.isNull()) {
            finish({}, {}, {error});
            return;
        }
        QStringList errors;
        const ExportOptions opts = store().effectiveExportOptions(allMonitorsTargetId());
        finishWith(write({{allMonitorsTargetId(), image}}, &errors, opts), image, errors, tr("All monitors"),
                   allMonitorsTargetId());
    });
}

void CaptureSession::captureTarget(const QString &id)
{
    const TriggerPlan plan = planTrigger(store(), id, screens_);
    if (plan.outputs.isEmpty()) {
        if (!plan.partialNotifications.isEmpty()) {
            app_->notifyPartialCapture(tr("Area not captured"), plan.partialNotifications);
        }
        finish({}, {}, plan.errors);
        return;
    }
    withFrames(plan.connectors, [this, plan] {
        QList<RenderedOutput> validOutputs;
        QStringList errors = plan.errors;
        for (const RenderedOutput &output : renderOutputs(plan, frames_)) {
            if (output.image.isNull()) {
                errors.append(output.error);
            } else {
                validOutputs.append(output);
            }
        }
        if (!plan.partialNotifications.isEmpty()) {
            app_->notifyPartialCapture(validOutputs.isEmpty() ? tr("Area not captured") : tr("Partial capture"),
                                       plan.partialNotifications);
        }
        const QImage clipboard = validOutputs.isEmpty() ? QImage() : validOutputs.first().image;
        finishWith(writeOutputs(validOutputs, &errors), clipboard, errors, plan.displayName, plan.triggerId);
    });
}

void CaptureSession::addTarget(const QString &id)
{
    QStringList areaIds;
    if (const Group *group = store().group(id)) {
        areaIds = group->targetIds;
    } else {
        areaIds = {id};
    }
    for (const QString &areaId : std::as_const(areaIds)) {
        const Area *area = store().area(areaId);
        if (!area) {
            continue;
        }
        if (area->isCrossMonitor()) {
            const bool anyConnected = std::any_of(area->monitors.cbegin(), area->monitors.cend(),
                                                  [&](const AreaMonitorBinding &b) { return isConnected(b.connector); });
            if (!anyConnected) {
                continue;
            }
            const QRect clamped = area->rect.intersected(desktopBounds_);
            if (!clamped.isEmpty()) {
                selections_.add(clamped);
            }
        } else if (const ScreenInfo *s = screenFor(area->connector)) {
            selections_.add(area->rect.translated(s->geometry.topLeft()));
        }
    }
    highlights_.clear();
    changed();
}

void CaptureSession::captureWindow(const WindowInfo &window)
{
    // KWin renders the window on its own, so the overlay never shows up in it.
    const CaptureResult result = CaptureService::captureWindow(window.handle);
    if (result.image.isNull()) {
        finish({}, {}, {result.error});
        return;
    }
    QStringList errors;
    finishWith(write({{windowToken(window), result.image}}, &errors), result.image, errors, window.caption);
}

void CaptureSession::setFrozen(bool frozen)
{
    if (frozen == frozen_ || switching_ || finished_) {
        return;
    }
    if (!frozen && !canSwitchToLive()) {
        return;
    }
    // The next overlay starts in this mode when the setting says "Last used".
    store().lastOverlayFrozen = frozen;
    app_->saveSettings();
    if (!frozen) {
        frozen_ = false;
        frames_.clear();
        changed();
        return;
    }
    switching_ = true;
    hideOverlays();
    QTimer::singleShot(liveHideDelayMs, this, [this] {
        switching_ = false;
        if (finished_) {
            return;
        }
        QStringList connectors;
        for (const ScreenInfo &screen : std::as_const(screens_)) {
            connectors.append(screen.connector);
        }
        captureFrames(connectors);
        frozen_ = true;
        showOverlays();
    });
}

bool CaptureSession::helpExpanded() const
{
    return panelDrawer_ == PanelDrawer::Help;
}

void CaptureSession::retakeWithDelay(int seconds)
{
    if (finished_ || switching_ || seconds <= 0 || purpose_ != Purpose::Print) {
        return;
    }
    if (annotationController_.isEditingText()) {
        annotationController_.requestCommitActiveText(false);
    }
    switching_ = true;
    hideOverlays();
    QStringList connectors;
    for (const ScreenInfo &screen : std::as_const(screens_)) {
        connectors.append(screen.connector);
    }
    QPointer<CaptureSession> self(this);
    app_->runWithDelay(
        seconds, tr("Retake"),
        [self, connectors] {
            if (!self || self->finished_) {
                return;
            }
            self->switching_ = false;
            self->frozen_ = true;
            self->store().lastOverlayFrozen = true;
            self->app_->saveSettings();
            self->frames_.clear();
            if (self->annotations_.isEmpty() && self->selections_.isEmpty()) {
                self->savePrintImage();
                if (!self->finished_) {
                    self->changed();
                    self->showOverlays();
                }
                return;
            }
            if (!self->printPath().isEmpty()) {
                const QString oldPath = self->printPath();
                QFile::remove(oldPath);
                self->app_->forgetCapturePath(oldPath);
                self->printPath().clear();
            }
            self->captureFrames(connectors);
            self->changed();
            self->showOverlays();
        },
        [self] {
            if (!self || self->finished_) {
                return;
            }
            self->switching_ = false;
            self->showOverlays();
        });
}

int CaptureSession::captureDelaySeconds() const
{
    return app_->captureDelaySeconds();
}

bool CaptureSession::keepDelayActive() const
{
    return app_->keepDelayActive();
}

void CaptureSession::setCaptureDelay(int seconds, bool keepActive)
{
    app_->setCaptureDelay(seconds, keepActive);
    changed();
}

void CaptureSession::toggleHelp()
{
    togglePanelDrawer(PanelDrawer::Help);
}

void CaptureSession::openSettings()
{
    close();
    if (finished_) {
        app_->showSettings();
    }
}

void CaptureSession::close()
{
    if (purpose_ == Purpose::Print && annotations_.hasRedactions() && !printPath().isEmpty() && !finished_) {
        QWidget *parentWidget = nullptr;
        for (const QPointer<CaptureOverlay> &overlay : std::as_const(overlays_)) {
            if (overlay && overlay->geometry().contains(QCursor::pos())) {
                parentWidget = overlay.data();
                break;
            }
        }
        if (!parentWidget && !overlays_.isEmpty()) {
            parentWidget = overlays_.first().data();
        }
        QMessageBox box(QMessageBox::Warning, tr("Unredacted Screenshot Exists"),
                        tr("The full screenshot saved when you pressed Print does not include your redactions."),
                        QMessageBox::NoButton, parentWidget);
        box.setInformativeText(tr("Delete the unredacted file (%1) from disk, keep it, or return to editing?")
                                   .arg(QFileInfo(printPath()).fileName()));
        QPushButton *deleteBtn = box.addButton(tr("Delete Unredacted Screenshot"), QMessageBox::DestructiveRole);
        QPushButton *keepBtn = box.addButton(tr("Keep Full Screenshot"), QMessageBox::AcceptRole);
        QPushButton *backBtn = box.addButton(tr("Back to Overlay"), QMessageBox::RejectRole);
        box.setDefaultButton(backBtn);
        box.setEscapeButton(backBtn);
        box.exec();
        if (box.clickedButton() == backBtn || !box.clickedButton()) {
            return;
        }
        if (box.clickedButton() == deleteBtn) {
            const QString oldPath = printPath();
            purgeUnredactedPrintFile(oldPath, true);
            app_->forgetCapturePath(oldPath);
            printPath().clear();
            finish(tr("Unredacted screenshot deleted"), {});
            return;
        }
        Q_UNUSED(keepBtn);
    }
    if (!printPath().isEmpty()) {
        const ExportOptions opts = store().effectiveExportOptions(allMonitorsTargetId());
        app_->runPostCaptureHook(allMonitorsTargetId(), tr("All monitors"), {printPath()},
                                 QDateTime::currentDateTime(), formatConfigKey(opts.format),
                                 QStringLiteral("print"));
    }
    finish({}, printPath().isEmpty() ? QStringList() : QStringList{printPath()});
}

void CaptureSession::finishWith(const QStringList &paths, const QImage &clipboardImage, const QStringList &errors,
                                const QString &displayName, const QString &targetId)
{
    if (paths.isEmpty()) {
        finish({}, {}, errors);
        return;
    }
    // The narrowed-down result replaces the full screenshot taken when Print was pressed.
    if (!printPath().isEmpty() && !paths.contains(printPath())) {
        QFile::remove(printPath());
        printPath().clear();
    }
    auto *mime = new QMimeData;
    if (!clipboardImage.isNull()) {
        mime->setImageData(clipboardImage);
    }
    if (paths.size() > 1) {
        QList<QUrl> urls;
        for (const QString &path : paths) {
            urls.append(QUrl::fromLocalFile(path));
        }
        mime->setUrls(urls);
    }
    QGuiApplication::clipboard()->setMimeData(mime);
    app_->recordCapture(paths, displayName, targetId);
    const std::optional<ImageFormat> fmt = imageFormatFromPath(paths.first());
    const QString groupId = store().group(targetId) ? targetId : QString();
    app_->runPostCaptureHook(targetId, displayName, paths, QDateTime::currentDateTime(),
                             fmt ? formatConfigKey(*fmt) : QStringLiteral("png"), QStringLiteral("overlay"), false,
                             groupId);
    finish({}, paths, errors);
}

void CaptureSession::finish(const QString &title, const QStringList &paths, const QStringList &errors)
{
    if (finished_) {
        return;
    }
    waitForPrintWrite();
    if (finished_) {
        return; // the pending write failed and finished the session with its error
    }
    finished_ = true;
    for (const QPointer<CaptureOverlay> &overlay : std::as_const(overlays_)) {
        if (overlay) {
            overlay->close();
        }
    }
    overlays_.clear();
    if (!paths.isEmpty() || !title.isEmpty() || !errors.isEmpty()) {
        app_->notifySaved(title, paths, errors);
    }
    Q_EMIT finished();
}

// ------------------------------------------------------------------------------------------ Overlay

CaptureOverlay::CaptureOverlay(CaptureSession *session, const ScreenInfo &screen)
    : session_(session)
    , screen_(screen)
    , lastPosition_(screen.geometry.topLeft())
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setWindowTitle(tr("glimply capture on %1").arg(session->monitorName(screen.connector)));
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

void CaptureOverlay::buildPanel()
{
    panel_ = new CapsulePanel(this);
    PanelModeCap *cap = panel_->modeCap();
    cap->setTexts(tr("Frozen"), tr("Live"), QStringLiteral("Space"));
    connect(cap, &QAbstractButton::clicked, this, [this] { session_->setFrozen(!session_->frozen()); });

    connect(panel_, &CapsulePanel::dragMoved, this, [this](const QPoint &pos, const QPoint &grabOffset) {
        panelDragging_ = true;
        panelGrabOffset_ = grabOffset;
        const QSize size = panel_->size();
        QPoint topLeft = pos - grabOffset;
        topLeft.setX(std::clamp(topLeft.x(), 0, std::max(0, width() - size.width())));
        topLeft.setY(std::clamp(topLeft.y(), 0, std::max(0, height() - size.height())));
        panel_->move(topLeft);
        placeHint();
    });
    connect(panel_, &CapsulePanel::dragFinished, this, [this](const QPoint &pos) {
        const bool moved = std::exchange(panelDragging_, false);
        if (!moved) {
            return;
        }
        const QPoint pointer = toGlobalPoint(pos);
        if (!screen_.geometry.contains(pointer)) {
            // Dropped on another monitor: the panel moves there, at the drop position (Q18).
            session_->movePanelToMonitor(pointer, pointer - panelGrabOffset_);
            return;
        }
        session_->setPanelPosition(screen_.connector, panel_->pos());
    });
    connect(panel_, &CapsulePanel::resetRequested, this,
            [this] { session_->setPanelPosition(screen_.connector, std::nullopt); });

    auto *hintPanel = new OverlayPanel(this);
    hintPanel_ = hintPanel;
    auto *hintLayout = new QHBoxLayout(hintPanel);
    hintLayout->setContentsMargins(6, 0, 6, 0);
    hint_ = new QLabel(hintPanel);
    hint_->setAlignment(Qt::AlignCenter);
    hint_->setWordWrap(true);
    hint_->setMaximumWidth(720);
    QPalette hintPalette = hint_->palette();
    QColor muted = hintPalette.color(QPalette::WindowText);
    muted.setAlphaF(0.8);
    hintPalette.setColor(QPalette::WindowText, muted);
    hint_->setPalette(hintPalette);
    hintLayout->addWidget(hint_);
    hintPanel->hide();
    panel_->hide();
}

QString CaptureOverlay::panelState() const
{
    if (session_->purpose() != CaptureSession::Purpose::Print) {
        return QStringLiteral("area");
    }
    return session_->selections().isEmpty() ? QStringLiteral("start") : QStringLiteral("selection");
}

void CaptureOverlay::rebuildMainRow(const QString &state)
{
    using Drawer = CaptureSession::PanelDrawer;
    panel_->clearMainRow();
    drawerButtons_.clear();
    saveButton_ = nullptr;
    delayButton_ = nullptr;

    const auto add = [this](const QString &icon, const QString &title, const QString &shortcut,
                            const std::function<void()> &action) {
        auto *button = new PanelButton(icon, title, shortcut, panel_);
        connect(button, &QAbstractButton::clicked, this, action);
        panel_->addMainButton(button);
        return button;
    };
    const auto drawer = [&](Drawer kind, const QString &icon, const QString &title, const QString &shortcut) {
        PanelButton *button = add(icon, title, shortcut, [this, kind] { session_->togglePanelDrawer(kind); });
        button->setOpensDrawer(true);
        drawerButtons_.insert(int(kind), button);
        return button;
    };
    const auto close = [&] {
        add(QStringLiteral("window-close"), tr("Close"), QStringLiteral("Esc"), [this] { session_->close(); });
    };
    // Every tool letter opens Annotate; listed one by one, never abbreviated.
    const QString toolKeys = QStringLiteral("V A R E B M T N X S");

    if (state == QLatin1String("area")) {
        drawer(Drawer::Areas, QStringLiteral("select-rectangular"), tr("Areas"), QString());
        panel_->addGroupGap();
        add(QStringLiteral("list-add"), tr("Add Rectangle"), QString(), [this] { session_->setForceNew(true); });
        PanelButton *save = drawer(Drawer::SaveArea, QStringLiteral("bookmark-new"), tr("Save as Area"),
                                   QStringLiteral("Enter"));
        save->setOpensDrawer(false);
        save->setPrimary(true);
        saveButton_ = save;
        panel_->addGroupGap();
        close();
        return;
    }
    if (state == QLatin1String("selection")) {
        PanelButton *save =
            add(QStringLiteral("document-save"), tr("Save"), QStringLiteral("Enter"), [this] { session_->save(); });
        save->setPrimary(true);
        saveButton_ = save;
        add(QStringLiteral("edit-copy"), tr("Copy"), QString(), [this] { session_->copy(); });
        add(QStringLiteral("window-pin"), tr("Pin"), QStringLiteral("P"), [this] { session_->pin(); });
        add(QStringLiteral("edit-select-text"), tr("Copy Text"), QStringLiteral("O"),
            [this] { session_->copyText(screen_.connector); });
        panel_->addGroupGap();
        drawer(Drawer::Markup, QStringLiteral("draw-freehand"), tr("Annotate"), toolKeys);
        panel_->addGroupGap();
        drawer(Drawer::More, QStringLiteral("overflow-menu"), tr("More"), QString());
        close();
        return;
    }
    add(QStringLiteral("monitor-symbolic"), tr("This Monitor"), QString(),
        [this] { session_->captureMonitor(screen_.connector); });
    add(QStringLiteral("view-fullscreen"), tr("All Monitors"), QString(), [this] { session_->captureAllMonitors(); });
    drawer(Drawer::Window, QStringLiteral("window"), tr("Window"), QString());
    drawer(Drawer::Areas, QStringLiteral("select-rectangular"), tr("Areas"), QString());
    panel_->addGroupGap();
    delayButton_ = drawer(Drawer::Delay, QStringLiteral("chronometer"), tr("Delay"), QString());
    drawer(Drawer::Markup, QStringLiteral("draw-freehand"), tr("Annotate"), toolKeys);
    panel_->addGroupGap();
    drawer(Drawer::More, QStringLiteral("overflow-menu"), tr("More"), QString());
    close();
}

QString CaptureOverlay::drawerSignature(CaptureSession::PanelDrawer drawer) const
{
    using Drawer = CaptureSession::PanelDrawer;
    QString signature = QString::number(int(drawer));
    switch (drawer) {
    case Drawer::Delay:
        signature += QStringLiteral(":%1:%2").arg(session_->captureDelaySeconds()).arg(session_->keepDelayActive());
        break;
    case Drawer::More:
        signature += QLatin1Char(':') + panelState() + QLatin1Char(':') + QString::number(session_->colorPickerActive());
        break;
    case Drawer::Areas:
        signature += QLatin1Char(':') + panelState();
        break;
    default:
        break; // Markup and the area form keep their state; they are never rebuilt while open
    }
    return signature;
}

QWidget *CaptureOverlay::buildDrawer(CaptureSession::PanelDrawer drawer)
{
    using Drawer = CaptureSession::PanelDrawer;
    switch (drawer) {
    case Drawer::None:
        return nullptr;
    case Drawer::Window:
        return buildWindowList();
    case Drawer::Areas:
        return buildAreaList();
    case Drawer::Delay:
        return buildDelayRow();
    case Drawer::Markup: {
        auto *toolbar = new AnnotationToolbar(&session_->annotationController(), nullptr);
        toolbar->setCompactOverlayMode(true);
        toolbar->setSettings(&session_->store().annotations);
        connect(toolbar, &AnnotationToolbar::settingsModified, this, [this] { session_->persistAnnotationSettings(); });
        return toolbar;
    }
    case Drawer::More:
        return buildMoreRow();
    case Drawer::SaveArea:
        return buildAreaForm();
    case Drawer::Help:
        return buildHelpPanel(nullptr);
    }
    return nullptr;
}

namespace
{

// A list drawer (windows, areas): grows with its rows up to a limit, then scrolls.
QWidget *listDrawer(QWidget *content)
{
    constexpr int maxHeight = 360;
    auto *scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setAutoFillBackground(false);
    scroll->viewport()->setAutoFillBackground(false);
    content->setAutoFillBackground(false);
    scroll->setWidget(content);
    const QSize hint = content->sizeHint();
    const bool scrolls = hint.height() > maxHeight;
    const int scrollbar = scrolls ? scroll->style()->pixelMetric(QStyle::PM_ScrollBarExtent) : 0;
    scroll->setFixedSize(std::min(hint.width() + scrollbar, 640), std::min(hint.height(), maxHeight));
    return scroll;
}

QWidget *buttonRow(const QList<QWidget *> &buttons)
{
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    for (QWidget *button : buttons) {
        button->setParent(row);
        layout->addWidget(button);
    }
    layout->addStretch();
    return row;
}

} // namespace

QWidget *CaptureOverlay::buildWindowList()
{
    auto *content = new QWidget;
    auto *column = new QVBoxLayout(content);
    column->setContentsMargins(0, 2, 0, 2);
    column->setSpacing(0);
    const QList<WindowInfo> windows = session_->windows();
    if (windows.isEmpty()) {
        column->addWidget(new QLabel(tr("No windows found"), content));
        return content;
    }
    for (const WindowInfo &window : windows) {
        QString text = elided(window.caption.isEmpty() ? window.appName : window.caption, 70);
        if (window.minimized) {
            text = tr("%1 (minimized)").arg(text);
        }
        const QString icon = QIcon::hasThemeIcon(window.iconName) ? window.iconName : QStringLiteral("window");
        auto *row = barButton(content, icon, text, QString());
        row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        row->setAttribute(Qt::WA_Hover);
        row->installEventFilter(new HoverOutline(row, session_, {{window.geometry, window.caption}}));
        connect(row, &QToolButton::clicked, this, [this, window] { session_->captureWindow(window); });
        column->addWidget(row);
    }
    return listDrawer(content);
}

QWidget *CaptureOverlay::buildAreaList()
{
    auto *content = new QWidget;
    auto *column = new QVBoxLayout(content);
    column->setContentsMargins(0, 2, 0, 2);
    column->setSpacing(0);
    const bool print = session_->purpose() == CaptureSession::Purpose::Print;
    const TargetStore &store = session_->store();
    if (store.areas.isEmpty() && store.groups.isEmpty()) {
        auto *empty = new QLabel(tr("No saved areas yet. Draw a rectangle, then choose Save as Area."), content);
        empty->setWordWrap(true);
        empty->setMaximumWidth(360);
        column->addWidget(empty);
        return content;
    }

    const auto addRow = [&](const QString &id, const QString &icon, const QString &name, bool available,
                            const QList<CaptureSession::Highlight> &outline) {
        auto *row = new QWidget(content);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);
        QString label = name;
        if (const QKeySequence shortcut = session_->shortcut(id); !shortcut.isEmpty()) {
            label = tr("%1    %2").arg(name, shortcut.toString(QKeySequence::NativeText));
        }
        auto *main = barButton(row, icon, label, print ? tr("Capture now") : tr("Add as a rectangle"));
        main->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        main->setEnabled(available);
        layout->addWidget(main);
        connect(main, &QToolButton::clicked, this, [this, id, print] {
            if (print) {
                session_->captureTarget(id);
            } else {
                session_->setPanelDrawer(CaptureSession::PanelDrawer::None);
                session_->addTarget(id);
            }
        });
        if (print) {
            auto *add = barButton(row, QStringLiteral("list-add"), QString(), tr("Add as a rectangle to adjust it"));
            add->setToolButtonStyle(Qt::ToolButtonIconOnly);
            add->setAccessibleName(tr("Add %1 as a rectangle").arg(name));
            add->setEnabled(available);
            layout->addWidget(add);
            connect(add, &QToolButton::clicked, this, [this, id] {
                session_->setPanelDrawer(CaptureSession::PanelDrawer::None);
                session_->addTarget(id);
            });
        }
        // Hovering a row outlines the area on screen.
        row->setAttribute(Qt::WA_Hover);
        row->installEventFilter(new HoverOutline(row, session_, outline));
        column->addWidget(row);
    };

    const auto outlineOf = [&](const QString &areaId) -> QList<CaptureSession::Highlight> {
        const Area *area = store.area(areaId);
        if (!area) {
            return {};
        }
        if (area->isCrossMonitor()) {
            return {{area->rect, area->name}};
        }
        const QScreen *screen = findScreen(area->connector);
        if (!screen) {
            return {};
        }
        return {{area->rect.translated(screen->geometry().topLeft()), area->name}};
    };

    const auto areaAvailable = [&](const Area &area) {
        if (area.isCrossMonitor()) {
            return std::any_of(area.monitors.cbegin(), area.monitors.cend(),
                               [&](const AreaMonitorBinding &b) { return session_->isConnected(b.connector); });
        }
        return session_->isConnected(area.connector);
    };

    for (const Group &group : store.groups) {
        QList<CaptureSession::Highlight> outline;
        bool available = false;
        for (const QString &member : group.targetIds) {
            outline += outlineOf(member);
            if (const Area *area = store.area(member); area && areaAvailable(*area)) {
                available = true;
            }
        }
        addRow(group.id, QStringLiteral("object-group"), group.name, available, outline);
    }
    if (!store.groups.isEmpty() && !store.areas.isEmpty()) {
        auto *line = new QFrame(content);
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);
        column->addWidget(line);
    }
    for (const Area &area : store.areas) {
        addRow(area.id, QStringLiteral("select-rectangular"), area.name, areaAvailable(area), outlineOf(area.id));
    }
    return listDrawer(content);
}

QWidget *CaptureOverlay::buildDelayRow()
{
    const int currentDelay = session_->captureDelaySeconds();
    const bool keepActive = session_->keepDelayActive();
    const auto retake = [this, keepActive](int seconds) {
        session_->setPanelDrawer(CaptureSession::PanelDrawer::None);
        if (keepActive) {
            session_->setCaptureDelay(seconds, true);
        }
        session_->retakeWithDelay(seconds);
    };

    QList<QWidget *> buttons;
    for (const int preset : {3, 5, 10}) {
        auto *button = new PanelButton(QString(), tr("Retake in %1 s").arg(preset), QString(), nullptr);
        button->setGlyphText(tr("%1 s").arg(preset));
        button->setActive(keepActive && currentDelay == preset);
        connect(button, &QAbstractButton::clicked, this, [retake, preset] { retake(preset); });
        buttons.append(button);
    }
    auto *custom = new PanelButton(QStringLiteral("chronometer"), tr("Retake in custom…"), QString(), nullptr);
    connect(custom, &QAbstractButton::clicked, this, [this, currentDelay, retake] {
        bool ok = false;
        const int chosen = QInputDialog::getInt(this, tr("Retake Delay"), tr("Seconds:"),
                                              currentDelay > 0 ? currentDelay : 3, 1, 60, 1, &ok);
        if (ok) {
            retake(chosen);
        }
    });
    buttons.append(custom);
    auto *keep = new PanelButton(QStringLiteral("view-refresh"), tr("Keep delay active"), QString(), nullptr);
    keep->setActive(keepActive);
    connect(keep, &QAbstractButton::clicked, this, [this, currentDelay, keepActive] {
        session_->setCaptureDelay(!keepActive ? (currentDelay > 0 ? currentDelay : 3) : 0, !keepActive);
    });
    buttons.append(keep);
    return buttonRow(buttons);
}

QWidget *CaptureOverlay::buildMoreRow()
{
    using Drawer = CaptureSession::PanelDrawer;
    const auto button = [this](const QString &icon, const QString &title, const QString &shortcut,
                               const std::function<void()> &action) {
        auto *b = new PanelButton(icon, title, shortcut, nullptr);
        connect(b, &QAbstractButton::clicked, this, action);
        return static_cast<QWidget *>(b);
    };
    auto *pick = static_cast<PanelButton *>(
        button(QStringLiteral("color-picker"), tr("Pick Color"), QStringLiteral("I"), [this] {
            session_->setPickerCursorGlobal(lastPosition_);
            session_->setColorPickerActive(!session_->colorPickerActive());
        }));
    pick->setActive(session_->colorPickerActive());
    QWidget *edit = button(QStringLiteral("document-edit"), tr("Edit in Window"), QStringLiteral("Ctrl+E"),
                           [this] { session_->editInWindow(screen_.connector); });
    if (panelState() == QLatin1String("selection")) {
        return buttonRow({
            button(QStringLiteral("document-save-as"), tr("Save As…"), QString(), [this] { session_->saveAs(); }),
            button(QStringLiteral("bookmark-new"), tr("Save as Area"), QString(), [this] { session_->showAreaPanel(); }),
            button(QStringLiteral("list-add"), tr("Add Rectangle"), QString(),
                   [this] {
                       session_->setPanelDrawer(Drawer::None);
                       session_->setForceNew(true);
                   }),
            edit,
            pick,
        });
    }
    return buttonRow({
        pick,
        edit,
        button(QStringLiteral("configure"), tr("Settings"), QString(), [this] { session_->openSettings(); }),
        button(QStringLiteral("help-contextual"), tr("Help"), QStringLiteral("F1"), [this] { session_->toggleHelp(); }),
    });
}

QWidget *CaptureOverlay::buildAreaForm()
{
    SelectionModel &model = session_->selections();
    const int count = model.count();
    const TargetStore &store = session_->store();

    auto *form = new QWidget;
    auto *layout = new QFormLayout(form);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto *title = new QLabel(form);
    QFont bold = title->font();
    bold.setBold(true);
    title->setFont(bold);
    layout->addRow(title);

    auto *combine = new QCheckBox(tr("Combine into a group"), form);
    combine->setChecked(true);
    combine->setVisible(count > 1);
    if (count > 1) {
        layout->addRow(combine);
    }

    auto *name = new QLineEdit(form);
    name->setMinimumWidth(260);
    if (count == 1) {
        const QList<ScreenInfo> hit = session_->intersectingScreens(model.inDrawingOrder().first().rect);
        const QString prefix = hit.size() == 1 ? store.monitorName(hit.first()) : QStringLiteral("area");
        name->setText(store.nextAreaName(prefix));
    } else {
        int n = 1;
        while (!store.isNameAvailable(tr("Group %1").arg(n))) {
            ++n;
        }
        name->setText(tr("Group %1").arg(n));
    }
    name->selectAll();
    layout->addRow(tr("Name:"), name);

    auto *shortcut = new KKeySequenceWidget(form);
    shortcut->setCheckForConflictsAgainst(KKeySequenceWidget::GlobalShortcuts | KKeySequenceWidget::StandardShortcuts);
    layout->addRow(tr("Shortcut:"), shortcut);

    auto *delayCombo = new QComboBox(form);
    delayCombo->setAccessibleName(tr("Area capture delay"));
    delayCombo->addItem(tr("None"), 0);
    delayCombo->addItem(tr("%n seconds", nullptr, 3), 3);
    delayCombo->addItem(tr("%n seconds", nullptr, 5), 5);
    delayCombo->addItem(tr("%n seconds", nullptr, 10), 10);
    delayCombo->addItem(tr("Custom…"), -1);
    layout->addRow(tr("Delay:"), delayCombo);
    auto lastDelayIdx = std::make_shared<int>(0);
    connect(delayCombo, &QComboBox::currentIndexChanged, form, [form, delayCombo, lastDelayIdx](int idx) {
        if (idx < 0) {
            return;
        }
        const int data = delayCombo->itemData(idx).toInt();
        if (data >= 0) {
            *lastDelayIdx = idx;
            return;
        }
        bool ok = false;
        const int prev = std::max(1, delayCombo->itemData(*lastDelayIdx).toInt());
        const int chosen = QInputDialog::getInt(form, tr("Area Delay"), tr("Seconds:"), prev, 1, 60, 1, &ok);
        if (!ok) {
            const QSignalBlocker block(delayCombo);
            delayCombo->setCurrentIndex(*lastDelayIdx);
            return;
        }
        const int existing = delayCombo->findData(chosen);
        if (existing >= 0) {
            const QSignalBlocker block(delayCombo);
            delayCombo->setCurrentIndex(existing);
            *lastDelayIdx = existing;
            return;
        }
        const QSignalBlocker block(delayCombo);
        const int insertAt = delayCombo->count() - 1;
        delayCombo->insertItem(insertAt, tr("%n seconds", nullptr, chosen), chosen);
        delayCombo->setCurrentIndex(insertAt);
        *lastDelayIdx = insertAt;
    });

    auto *explain = new QLabel(form);
    explain->setWordWrap(true);
    explain->setMaximumWidth(380);
    layout->addRow(QString(), explain);

    auto *error = new QLabel(form);
    error->setWordWrap(true);
    error->setMaximumWidth(380);
    error->hide();
    layout->addRow(QString(), error);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    auto *back = new QPushButton(QIcon::fromTheme(QStringLiteral("go-previous")), tr("Back"), form);
    auto *confirm = new QPushButton(QIcon::fromTheme(QStringLiteral("document-save")), QString(), form);
    confirm->setDefault(true);
    buttons->addWidget(back);
    buttons->addWidget(confirm);
    layout->addRow(buttons);

    const auto update = [=] {
        const bool group = count > 1 && combine->isChecked();
        const bool named = count == 1 || group;
        name->setEnabled(named);
        shortcut->setEnabled(named);
        if (count == 1) {
            title->setText(tr("Save as Area"));
            explain->setText(tr("The shortcut is optional and works in every application. You can also capture the "
                                "area from the glimply widget."));
            confirm->setText(tr("Save Area"));
        } else if (group) {
            title->setText(tr("Save %n areas as a group", nullptr, count));
            explain->setText(tr("One shortcut captures all %n areas at the same moment, each into its own file.",
                                nullptr, count));
            confirm->setText(tr("Save Group"));
        } else {
            title->setText(tr("Save %n areas", nullptr, count));
            explain->setText(tr("Each rectangle becomes its own area with an automatic name. You can rename them and "
                                "give them shortcuts later in the widget or the settings."));
            confirm->setText(tr("Save %n Areas", nullptr, count));
        }
    };
    update();
    connect(combine, &QCheckBox::toggled, form, update);

    const auto submit = [this, name, shortcut, delayCombo, combine, error] {
        const int chosenDelay = std::max(0, delayCombo->currentData().toInt());
        const QString problem =
            session_->saveAsAreas(name->text(), shortcut->keySequence(), combine->isChecked(), chosenDelay);
        if (!problem.isEmpty()) {
            error->setText(problem);
            error->show();
            name->setFocus();
            panel_->adjustSize();
            refreshPanel();
        }
    };
    connect(confirm, &QPushButton::clicked, form, submit);
    connect(name, &QLineEdit::returnPressed, form, submit);
    connect(back, &QPushButton::clicked, this, [this] {
        session_->setPanelDrawer(CaptureSession::PanelDrawer::None);
        setFocus();
    });

    QTimer::singleShot(0, name, [this, name] {
        activateWindow();
        name->setFocus();
    });
    return form;
}

void CaptureOverlay::refreshPanel()
{
    using Drawer = CaptureSession::PanelDrawer;
    if (!panel_) {
        return;
    }
    if (session_->panelConnector() != screen_.connector) {
        if (panel_->drawerOpen()) {
            panel_->setDrawer(nullptr);
        }
        builtDrawer_ = Drawer::None;
        builtDrawerSignature_.clear();
        panel_->hide();
        hintPanel_->hide();
        return;
    }

    const QString state = panelState();
    if (state != builtState_) {
        rebuildMainRow(state);
        builtState_ = state;
    }
    const Drawer drawer = session_->panelDrawer();
    const QString signature = drawerSignature(drawer);
    if (drawer != builtDrawer_ || signature != builtDrawerSignature_) {
        panel_->setDrawer(buildDrawer(drawer));
        builtDrawer_ = drawer;
        builtDrawerSignature_ = signature;
    }

    PanelModeCap *cap = panel_->modeCap();
    cap->setFrozen(session_->frozen());
    cap->setEnabled(!session_->frozen() || session_->canSwitchToLive());
    for (auto it = drawerButtons_.cbegin(); it != drawerButtons_.cend(); ++it) {
        if (it.value()) {
            it.value()->setActive(it.key() == int(drawer));
        }
    }
    if (saveButton_ && state == QLatin1String("selection")) {
        const int count = session_->selections().count();
        saveButton_->setTitle(count > 1 ? tr("Save %n Areas", nullptr, count) : tr("Save"));
    }
    if (saveButton_ && state == QLatin1String("area")) {
        saveButton_->setEnabled(!session_->selections().isEmpty());
    }
    if (delayButton_) {
        const int delay = session_->captureDelaySeconds();
        delayButton_->setBadge(delay > 0 ? tr("%1 s").arg(delay) : QString());
    }

    panel_->adjustSize();
    if (!panelDragging_ && !session_->isDragging() && drag_ == Drag::None) {
        QRect covered;
        for (const Selection &selection : session_->selections().stack()) {
            covered = covered.united(toLocalRect(selection.rect).intersected(rect()));
        }
        const QPoint position =
            placeOverlayPanel(panel_->size(), size(), covered, session_->panelPosition(screen_.connector), panelMargin);
        panel_->move(position);
    }
    panel_->show();
    panel_->raise();
    placeHint();
}

void CaptureOverlay::placeHint()
{
    const QString text = session_->hint();
    if (text.isEmpty() || panel_->isHidden()) { // isVisible() is false until the overlay is shown
        hintPanel_->hide();
        return;
    }
    hint_->setText(text);
    hintPanel_->adjustSize();
    const QSize size = hintPanel_->sizeHint();
    const QRect panel = panel_->geometry();
    const int x = std::clamp(panel.center().x() - size.width() / 2, 0, std::max(0, width() - size.width()));
    // Below the panel, or above it when the panel sits in the lower half.
    const bool above = panel.center().y() > height() / 2;
    const int y = above ? panel.top() - size.height() : panel.bottom() + 1;
    hintPanel_->setGeometry(x, std::clamp(y, 0, std::max(0, height() - size.height())), size.width(), size.height());
    hintPanel_->show();
    hintPanel_->raise();
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

    QList<QPair<QString, QString>> mouse{
        {tr("Drag"), tr("Draw a rectangle (or active markup tool)")},
        {tr("Drag inside / edge"), tr("Move / resize the active rectangle or annotation")},
        {tr("Shift / Alt while dragging"), tr("Keep a square or 15° snap / grow from centre or duplicate")},
        {tr("Space while dragging"), tr("Reposition shape or selection live")},
        {tr("Ctrl in markup tool"), tr("Temporarily switch to Select / Move")},
        {tr("Click / Right-click"), tr("Activate / delete topmost annotation or rectangle")},
        {tr("Double-click"), print ? tr("Edit text annotation or save rectangle") : tr("Save rectangle as area")},
    };
    if (print) {
        mouse.append({tr("Click, nothing drawn"), tr("Capture this monitor (before annotating)")});
    }
    section(tr("Mouse"), mouse);

    QList<QPair<QString, QString>> keys{
        {tr("V / A R E B M T N X S"), print ? tr("Select / Arrow Rect Ellipse Pen Marker Text Step Redact Spotlight")
                                          : tr("Selection modifiers work on all monitors")},
        {tr("[ / ]  ·  Ctrl+Z / Ctrl+Shift+Z"), tr("Step brush or font size  ·  Undo / Redo")},
        {tr("Space"), tr("Switch between Frozen and Live (when idle)")},
        {tr("Arrows / Shift+Arrows"), tr("Move active annotation or rectangle by 1 px / 10 px")},
        {tr("Alt+Arrows / Ctrl+Arrows"), tr("Resize from centre / resize square")},
        {tr("Enter"), print ? tr("Save selection or annotated screen") : tr("Save as area")},
        {tr("Delete / Backspace"), tr("Delete active annotation or rectangle")},
        {tr("Escape"), print ? tr("Commit text, step back, or close") : tr("Cancel")},
        {tr("F1"), tr("Show or hide this help")},
    };
    if (!print) {
        keys.removeFirst();
        keys.removeFirst();
    } else {
        keys.insert(2, {tr("I"), tr("Pick pixel color (Tab cycles format, Alt+Click applies to markup)")});
        keys.insert(3, {tr("Ctrl+E"), tr("Open selection or screen in Quick Editor window")});
        keys.insert(4, {tr("P"), tr("Pin the selection to the screen (one pin per rectangle)")});
        keys.insert(5, {tr("O"), tr("Copy the text of the selection (needs tesseract)")});
    }
    section(tr("Keys"), keys);
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
            const QRect hole = toLocalRect(selection.rect).intersected(area);
            if (!hole.isEmpty()) {
                holes.addRect(hole);
            }
        }
        QPainterPath shade;
        shade.addRect(area);
        painter.fillPath(shade.subtracted(holes), QColor(0, 0, 0, frozen ? frozenDim : liveDim));
    }

    // Outlines of areas or windows hovered in a list.
    for (const CaptureSession::Highlight &highlight : session_->highlights()) {
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
        const QRectF frame(local);
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
        const QString sizeText = tr("%1 × %2").arg(selection.rect.width()).arg(selection.rect.height());
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
            if (frozen && session_->store().showSelectionLoupe) {
                const QPoint nativePx = logicalToNativePixel(
                    QPointF(localCursor), QPointF(0.0, 0.0), session_->frameScale(screen_.connector), background.size());
                PixelLoupe::paintSelectionLoupe(painter, localCursor, area, pal, background, nativePx,
                                                session_->dragCursorGlobal(), sel->rect.size());
            } else {
                PixelLoupe::paintDimensionPill(painter, localCursor, area, pal, session_->dragCursorGlobal(),
                                               sel->rect.size());
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
    spaceHeld_ = false;

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
        updateCursor(globalPos);
        return;
    }

    if (drag_ == Drag::Pending) {
        if ((globalPos - anchor_).manhattanLength() < QApplication::startDragDistance()) {
            return;
        }
        drag_ = Drag::Create;
        dragId_ = session_->selections().add(QRect(anchor_, QSize(1, 1)));
        session_->setForceNew(false);
        session_->setDragging(true);
    } else if (drag_ == Drag::Move || drag_ == Drag::Resize) {
        session_->setDragging(true);
    }
    if (spaceHeld_ && (drag_ == Drag::Create || drag_ == Drag::Resize)) {
        const QRect bounds = session_->desktopBounds();
        anchor_.setX(std::clamp(anchor_.x() + step.x(), bounds.left(), bounds.left() + bounds.width()));
        anchor_.setY(std::clamp(anchor_.y() + step.y(), bounds.top(), bounds.top() + bounds.height()));
        if (drag_ == Drag::Resize) {
            dragStartRect_ = clampToBounds(dragStartRect_.translated(step), bounds);
        }
    }
    applyDrag(globalPos, event->modifiers());
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
    session_->setDragLoupe(drag_ == Drag::Create || drag_ == Drag::Resize, globalPos);
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
        } else if (model.isEmpty() && session_->purpose() == CaptureSession::Purpose::Print
                   && session_->annotations().isEmpty() && !session_->markupVisible()) {
            // Nothing drawn yet: a click captures this monitor.
            session_->captureMonitor(screen_.connector);
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

void CaptureOverlay::leaveEvent(QEvent *event)
{
    if (drag_ == Drag::None) {
        session_->setHoveredSelectionId(0);
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
        applyDrag(lastPosition_, event->modifiers());
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

void CaptureOverlay::keyPressEvent(QKeyEvent *event)
{
    if (session_->purpose() == CaptureSession::Purpose::Print && session_->annotationController().isEditingText()) {
        event->accept();
        return;
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
        if (event->key() == Qt::Key_Escape) {
            const Drag aborted = std::exchange(drag_, Drag::None);
            spaceHeld_ = false;
            session_->setDragLoupe(false);
            session_->setDragging(false);
            if (aborted == Drag::Create) {
                model.remove(dragId_);
            } else if (aborted == Drag::Resize || aborted == Drag::Move) {
                model.setRect(dragId_, dragStartRect_);
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

} // namespace glimply
