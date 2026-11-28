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
constexpr int liveHideDelayMs = 250;
} // namespace

// ------------------------------------------------------------------------------------------ Session

CaptureSession::CaptureSession(App *app, Purpose purpose)
    : QObject(app)
    , app_(app)
    , purpose_(purpose)
    , annotationController_(&annotations_, this)
    , transientHintTimer_(new QTimer(this))
{
    connect(app, &App::defaultRecordingTargetChanged, this, &CaptureSession::defaultRecordingTargetChanged);
    transientHintTimer_->setSingleShot(true);
    transientHintTimer_->setInterval(2000); // feedback pills (#72, Q16)
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
    qCInfo(lcOverlay) << "session start," << screens_.size() << "monitors";
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
    pointerGlobal_ = QCursor::pos();
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
    if (purpose_ == Purpose::Print && recordMode_) {
        // Choosing what to record needs no picture and writes no file. The panel itself lists the
        // targets (#95), so no drawer opens.
        frozen_ = false;
    } else if (purpose_ == Purpose::Print && textMode_) {
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

    // The windows under the pointer (#97), fetched off the GUI thread once the overlay is up, so the
    // Print path never waits for KWin's window list.
    if (purpose_ == Purpose::Print) {
        auto *watcher = new QFutureWatcher<QList<WindowInfo>>(this);
        connect(watcher, &QFutureWatcher<QList<WindowInfo>>::finished, this, [this, watcher] {
            pickWindows_ = pickableWindows(watcher->result());
            watcher->deleteLater();
            qCDebug(lcOverlay) << "pickable windows" << pickWindows_.size();
            setPointerPick(pickAt(pointerGlobal_));
        });
        const qint64 pid = QCoreApplication::applicationPid();
        watcher->setFuture(QtConcurrent::run([pid] { return listWindows(pid); }));
    }
}

bool CaptureSession::pickingActive() const
{
    if (purpose_ != Purpose::Print || finished_ || !selections_.isEmpty() || !annotations_.isEmpty()
        || colorPickerActive_ || markupVisible()) {
        return false;
    }
    return panelDrawer_ != PanelDrawer::SaveArea && panelDrawer_ != PanelDrawer::Help;
}

PickTarget CaptureSession::pickAt(const QPoint &globalPos) const
{
    if (!pickingActive()) {
        return {};
    }
    return pickTargetAt(globalPos, pickWindows_, screens_);
}

void CaptureSession::setPointerPick(const PickTarget &pick)
{
    if (pick.kind == pointerPick_.kind && pick.rect == pointerPick_.rect && pick.connector == pointerPick_.connector
        && pick.window.handle == pointerPick_.window.handle) {
        return;
    }
    pointerPick_ = pick;
    for (const QPointer<CaptureOverlay> &overlay : std::as_const(overlays_)) {
        if (overlay) {
            overlay->update();
        }
    }
}

void CaptureSession::notePointerPosition(const QPoint &globalPos)
{
    pointerGlobal_ = globalPos;
}

const PickTarget &CaptureSession::pointerPick() const
{
    return pointerPick_;
}

void CaptureSession::takePick(const PickTarget &pick)
{
    qCInfo(lcOverlay) << "click takes" << (pick.kind == PickTarget::Kind::Window ? "window" : "monitor")
                      << (pick.kind == PickTarget::Kind::Window ? pick.window.caption : pick.connector);
    if (pick.kind == PickTarget::Kind::Window) {
        if (recordMode_) {
            record(windowRecordingTargetId(pick.window.handle));
        } else {
            captureWindow(pick.window);
        }
    } else if (pick.kind == PickTarget::Kind::Monitor) {
        if (recordMode_) {
            record(screenTargetId(pick.connector));
        } else {
            captureMonitor(pick.connector);
        }
    }
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
    printFrames_ = frames_; // implicitly shared; the live loupe reads it (#101)
    for (const ScreenInfo &screen : std::as_const(screens_)) {
        if (frames_.value(screen.connector).image.isNull()) {
            QString error;
            (void)composeAllMonitors(screens_, frames_, &error);
            finish({}, {}, {error.isEmpty() ? tr("The screen could not be captured.") : error});
            return;
        }
    }

    // RAM-first default (#134): keep Print captures in memory until the user saves, copies, pins,
    // or exports, unless immediate-save is explicitly enabled or Print runs without the overlay.
    if (!shouldSavePrintImmediately(store().printOpensOverlay, forceShowOverlay_, store().saveImmediatelyOnPrint)) {
        finishLatencyBenchIfDone();
        return;
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
    if (finished_ || purpose_ != Purpose::Print || !qEnvironmentVariableIsSet("ITSOVA_LATENCY_BENCH")) {
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

QImage CaptureSession::loupeFrame(const QString &connector, bool *snapshot) const
{
    if (snapshot) {
        *snapshot = !frozen_;
    }
    return frozen_ ? frames_.value(connector).image : printFrames_.value(connector).image;
}

qreal CaptureSession::monitorScale(const QString &connector) const
{
    for (const QHash<QString, CaptureResult> *source : {&frames_, &printFrames_}) {
        if (const qreal scale = source->value(connector).scale; scale > 0.0) {
            return scale;
        }
    }
    const QScreen *screen = findScreen(connector);
    return screen ? screen->devicePixelRatio() : 1.0;
}

QRect CaptureSession::nativeSelectionOn(const QRect &globalRect, const QString &connector) const
{
    const auto screen = std::find_if(screens_.cbegin(), screens_.cend(),
                                     [&](const ScreenInfo &s) { return s.connector == connector; });
    if (screen == screens_.cend() || globalRect.isEmpty()) {
        return {};
    }
    // The same rounding as nativeAreaFor(), the rule the saved crop follows (#101), but without its
    // range check: an edge may lie on another monitor.
    const qreal scale = monitorScale(connector);
    const QRect local = globalRect.translated(-screen->geometry.topLeft());
    const auto edge = [scale](int logical) { return int(std::lround(logical * scale)); };
    const int left = edge(local.left());
    const int top = edge(local.top());
    return QRect(left, top, edge(local.left() + local.width()) - left, edge(local.top() + local.height()) - top);
}

QSize CaptureSession::nativeSize(const QRect &globalRect) const
{
    const QList<ScreenInfo> hit = intersectingScreens(globalRect);
    if (hit.size() == 1 && hit.first().geometry.contains(globalRect)) {
        return nativeSelectionOn(globalRect, hit.first().connector).size();
    }
    qreal scale = 1.0;
    for (const ScreenInfo &screen : hit) {
        scale = std::max(scale, monitorScale(screen.connector));
    }
    return QSize(int(std::lround(globalRect.width() * scale)), int(std::lround(globalRect.height() * scale)));
}

QRect CaptureSession::nativeSelectionOn(const Selection &selection, const QString &connector) const
{
    return nudgedNativeArea(nativeSelectionOn(selection.rect, connector), selection.nativeNudge);
}

QSize CaptureSession::nativeSize(const Selection &selection) const
{
    const QList<ScreenInfo> hit = intersectingScreens(selection.rect);
    if (hit.size() == 1 && hit.first().geometry.contains(selection.rect)) {
        return nativeSelectionOn(selection, hit.first().connector).size();
    }
    return nativeSize(selection.rect); // across monitors the nudge does not apply
}

QRectF CaptureSession::displayRect(const Selection &selection) const
{
    if (selection.nativeNudge.isNull()) {
        return QRectF(selection.rect);
    }
    const QList<ScreenInfo> hit = intersectingScreens(selection.rect);
    const qreal scale = hit.isEmpty() ? 1.0 : monitorScale(hit.first().connector);
    const QMargins &n = selection.nativeNudge;
    return QRectF(selection.rect).adjusted(n.left() / scale, n.top() / scale, n.right() / scale, n.bottom() / scale);
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
        // Leaving Markup: commit a text being typed and return to selecting rectangles.
        if (annotationController_.isEditingText()) {
            annotationController_.requestCommitActiveText(true);
        }
        annotationController_.setTool(ToolKind::Select);
    }
    if (panelDrawer_ == PanelDrawer::Window || panelDrawer_ == PanelDrawer::Areas || panelDrawer_ == PanelDrawer::Monitors
        || panelDrawer_ == PanelDrawer::Record) {
        highlights_.clear();
    }
    qCInfo(lcOverlay) << "drawer" << int(panelDrawer_) << "->" << int(drawer);
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

void CaptureSession::pickPanelEntry(int index)
{
    const QString connector = panelConnector();
    for (const QPointer<CaptureOverlay> &overlay : std::as_const(overlays_)) {
        if (overlay && overlay->connector() == connector) {
            overlay->pickListEntry(index);
            return;
        }
    }
}

CaptureOverlay *CaptureSession::panelOverlay() const
{
    const QString connector = panelConnector();
    for (const QPointer<CaptureOverlay> &overlay : std::as_const(overlays_)) {
        if (overlay && overlay->connector() == connector) {
            return overlay.data();
        }
    }
    return nullptr;
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

void CaptureSession::setDragLoupe(bool active, const QPoint &globalPos, Qt::Edges edges, bool keyNudge)
{
    dragLoupeActive_ = active;
    if (active) {
        dragCursorGlobal_ = globalPos;
        dragEdges_ = edges;
        dragKeyNudge_ = keyNudge;
    } else {
        dragEdges_ = {};
        dragKeyNudge_ = false;
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
        // Entering the picker changes what a click does: say so once, as a feedback pill.
        transientHint_ = tr("Click copies the color · Alt+Click applies it to markup · Tab switches the format · "
                            "Escape exits");
        transientHintTimer_->start();
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
    return itsova::intersectingScreens(screens_, globalRect);
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
    // No permanent hint line (#72, Q16): short feedback, or the one-time first-start hint.
    if (!transientHint_.isEmpty()) {
        return transientHint_;
    }
    if (firstHintPending()) {
        return tr("Hover over buttons for names and shortcuts · F1 shows everything");
    }
    return {};
}

bool CaptureSession::firstHintPending() const
{
    return purpose_ == Purpose::Print && !store().overlayFirstHintShown && panelDrawer_ != PanelDrawer::Help;
}

void CaptureSession::dismissFirstHint()
{
    if (store().overlayFirstHintShown || purpose_ != Purpose::Print) {
        return;
    }
    store().overlayFirstHintShown = true;
    app_->saveSettings();
    changed();
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

QList<ScreenInfo> CaptureSession::orderedScreens() const
{
    QList<ScreenInfo> ordered = screens_;
    std::sort(ordered.begin(), ordered.end(), [](const ScreenInfo &a, const ScreenInfo &b) {
        return std::pair(a.geometry.x(), a.geometry.y()) < std::pair(b.geometry.x(), b.geometry.y());
    });
    return ordered;
}

void CaptureSession::changed()
{
    // A drawer whose button the current main row no longer has closes (Q25); the area form needs a
    // rectangle.
    if (purpose_ == Purpose::Print && !selections_.isEmpty()
        && (panelDrawer_ == PanelDrawer::Window || panelDrawer_ == PanelDrawer::Areas
            || panelDrawer_ == PanelDrawer::Delay || panelDrawer_ == PanelDrawer::Monitors)) {
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
    // Recordings need the real (fractional) scale; a capture is the only place it shows (#30).
    for (const ScreenInfo &screen : std::as_const(screens_)) {
        if (const auto it = captured.constFind(screen.connector);
            it != captured.cend() && !it->image.isNull() && screen.geometry.width() > 0) {
            app_->noteNativeScale(screen, qreal(it->image.width()) / screen.geometry.width());
        }
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
        cropped = cropLogicalArea(frame.image, selection.rect.translated(-screen.geometry.topLeft()), frame.scale, error,
                                  selection.nativeNudge);
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
        // Do not delete an immediate-save Print file on handoff (#134): if the user closes the
        // editor without saving, the capture must remain intact; clean it up only once saved.
        const QString deferredPrint = printPath();
        const bool deferredClearClipboard = annotations_.hasRedactions();
        if (!deferredPrint.isEmpty()) {
            printPath().clear();
        }

        finished_ = true;
        for (const QPointer<CaptureOverlay> &overlay : std::as_const(overlays_)) {
            if (overlay) {
                overlay->close();
            }
        }
        overlays_.clear();

        app_->openEditorFromCapture(rawImage, scale, std::move(handoffDoc), baseToken, deferredPrint,
                                    deferredClearClipboard);
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

void CaptureSession::setRecordMode(bool on)
{
    recordMode_ = on;
}

bool CaptureSession::recordingAudioAvailable() const
{
    return purpose_ == Purpose::Print && app_->recordingAudioAvailable();
}

void CaptureSession::toggleRecordingAudio(RecordingAudioSource source)
{
    recordingAudioSelection_.toggle(recordingAudioAvailable() ? source : RecordingAudioSource::None);
    changed();
}

void CaptureSession::record(const QString &targetId)
{
    if (finished_) {
        return;
    }
    if (annotationController_.isEditingText()) {
        annotationController_.requestCommitActiveText(false);
    }
    // The Print file (if any) stays as it is; the recording is its own result. The overlay must be
    // off the screen before KWin starts the stream, or its last frame ends up in the recording.
    const int delay = captureDelaySeconds();
    const RecordingAudioSelection audio = recordingAudioSelection_;
    App *app = app_;
    finish({}, {});
    QTimer::singleShot(liveHideDelayMs, app, [app, targetId, delay, audio] {
        const QString error = app->startRecording(targetId, delay, audio);
        if (!error.isEmpty()) {
            app->notifySaved({}, {}, {error});
        }
    });
}

void CaptureSession::recordSelection()
{
    const QString targetId = selectionRecordingTargetId();
    if (!targetId.isEmpty()) {
        record(targetId);
    }
}

QString CaptureSession::selectionRecordingTargetId() const
{
    if (selections_.isEmpty()) {
        return {};
    }
    QRect bounds;
    for (const Selection &selection : selections_.stack()) {
        bounds = bounds.united(selection.rect);
    }
    return regionRecordingTargetId(bounds);
}

QString CaptureSession::defaultRecordingTarget() const
{
    return store().defaultRecordingTarget;
}

void CaptureSession::toggleDefaultRecordingTarget(const QString &targetId)
{
    if (targetId.isEmpty()) {
        return;
    }
    app_->setDefaultRecordingTarget(defaultRecordingTarget() == targetId ? QString() : targetId);
}

QString CaptureSession::defaultRecordingTip() const
{
    const QKeySequence key = shortcut(recordToggleActionId());
    return key.isEmpty() ? CaptureOverlay::tr("Default for the recording shortcut")
                         : CaptureOverlay::tr("Default for %1").arg(key.toString(QKeySequence::NativeText));
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
    dismissFirstHint();
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
    qCInfo(lcOverlay) << "session end" << title << paths << errors;
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

} // namespace itsova
