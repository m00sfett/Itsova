#include "captureoverlay.h"

#include "annotations/annotationrenderer.h"
#include "annotations/annotationtoolbar.h"
#include "annotations/inlinetexteditor.h"
#include "annotations/overlayexport.h"
#include "app.h"
#include "imageoutput.h"
#include "screens.h"
#include "selectiongeometry.h"
#include "shortcutmanager.h"
#include "triggerplan.h"

#include <KKeySequenceWidget>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
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
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidgetAction>
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
constexpr int barGap = 8;
constexpr int frozenDim = 140;
// Live: lighter, so a video underneath stays watchable.
constexpr int liveDim = 70;

QRect clampToArea(QRect rect, const QSize &area)
{
    rect = rect.normalized();
    rect.setWidth(std::min(rect.width(), area.width()));
    rect.setHeight(std::min(rect.height(), area.height()));
    rect.moveLeft(std::clamp(rect.left(), 0, area.width() - rect.width()));
    rect.moveTop(std::clamp(rect.top(), 0, area.height() - rect.height()));
    return rect;
}

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

QPushButton *actionButton(QWidget *parent, const QString &icon, const QString &text, const QString &tip)
{
    auto *button = new QPushButton(QIcon::fromTheme(icon), text, parent);
    button->setToolTip(tip);
    button->setFocusPolicy(Qt::NoFocus);
    button->setFlat(true);
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
{
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

    frozen_ = store().overlayStartsFrozen();
    if (purpose_ == Purpose::Print) {
        savePrintImage();
        if (finished_) {
            return;
        }
        if (!store().printOpensOverlay) {
            finish({}, {printPath_});
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
    QStringList connectors;
    for (const ScreenInfo &screen : std::as_const(screens_)) {
        connectors.append(screen.connector);
    }
    captureFrames(connectors);
    QString error;
    const QImage image = composeAllMonitors(screens_, frames_, &error);
    if (image.isNull()) {
        finish({}, {}, {error.isEmpty() ? tr("The screen could not be captured.") : error});
        return;
    }
    QStringList errors;
    const QStringList paths = write({{allMonitorsTargetId(), image}}, &errors);
    if (paths.isEmpty()) {
        finish({}, {}, errors);
        return;
    }
    if (!printPath_.isEmpty()) {
        QFile::remove(printPath_);
    }
    printPath_ = paths.first();
    QGuiApplication::clipboard()->setImage(image);
    app_->recordCapture(paths, tr("All monitors"));
}

void CaptureSession::printPressed()
{
    if (finished_ || purpose_ != Purpose::Print) {
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
    return markupVisible_;
}

void CaptureSession::setMarkupVisible(bool visible)
{
    if (purpose_ != Purpose::Print) {
        return;
    }
    if (visible == markupVisible_) {
        return;
    }
    if (!visible) {
        if (annotationController_.isEditingText()) {
            annotationController_.requestCommitActiveText(true);
        }
        markupVisible_ = false;
        annotationController_.setTool(ToolKind::Select);
        changed();
        return;
    }
    markupVisible_ = true;
    if (!frozen_) {
        setFrozen(true);
    } else {
        changed();
    }
}

void CaptureSession::activateMarkupTool(ToolKind tool)
{
    if (purpose_ != Purpose::Print) {
        return;
    }
    markupVisible_ = true;
    annotationController_.setTool(tool);
    if (!frozen_) {
        setFrozen(true);
    } else {
        changed();
    }
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

QString CaptureSession::hint() const
{
    if (dragging_) {
        return tr("Shift keeps a square, Alt grows from the centre, Space repositions while dragging.");
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
    const OutputWriter writer(store().effectiveCaptureDirectory(), store().effectiveFilenameTemplate());
    const QDateTime timestamp = QDateTime::currentDateTime();
    QStringList paths;
    for (const auto &[token, image] : images) {
        QString path;
        QString error;
        if (writer.writeImage(image, token, timestamp, &path, &error)) {
            paths.append(path);
        } else {
            errors->append(error);
        }
    }
    return paths;
}

QImage CaptureSession::selectionImage(const Selection &selection, QString *error) const
{
    const CaptureResult frame = frames_.value(selection.connector);
    if (frame.image.isNull()) {
        *error = frame.error.isEmpty() ? tr("Monitor %1 could not be captured.").arg(selection.connector) : frame.error;
        return {};
    }
    const QImage cropped = cropLogicalArea(frame.image, selection.rect, frame.scale, error);
    if (cropped.isNull() || annotations_.isEmpty()) {
        return cropped;
    }
    const ScreenInfo *screen = screenFor(selection.connector);
    const QRectF globalLogical = screen ? QRectF(selection.rect.translated(screen->geometry.topLeft()))
                                        : QRectF(selection.rect);
    const qreal scale = frame.scale > 0.0 ? frame.scale : 1.0;
    return renderAnnotatedCrop(cropped, globalLogical, scale, annotations_);
}

void CaptureSession::removePrintSnapshotIfRedacted()
{
    if (!shouldDeletePrintFileOnCopy(annotations_, printPath_)) {
        return;
    }
    const QString oldPath = printPath_;
    if (purgeUnredactedPrintFile(oldPath, false)) {
        app_->forgetCapturePath(oldPath);
        printPath_.clear();
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
        if (!connectors.contains(selection.connector)) {
            connectors.append(selection.connector);
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
            finish(tr("Copied to the clipboard"), printPath_.isEmpty() ? QStringList() : QStringList{printPath_});
        });
        return;
    }
    const Selection selection = *active;
    withFrames({selection.connector}, [this, selection] {
        QString error;
        const QImage image = selectionImage(selection, &error);
        if (image.isNull()) {
            finish({}, {}, {error});
            return;
        }
        QGuiApplication::clipboard()->setImage(image);
        removePrintSnapshotIfRedacted();
        finish(tr("Copied to the clipboard"), printPath_.isEmpty() ? QStringList() : QStringList{printPath_});
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
        connectors.append(selection.connector);
    }
    // The file dialog needs the screen; the frames are taken before the overlays step aside.
    const bool wasFrozen = frozen_;
    const auto ask = [this, wasFrozen] {
        hideOverlays();
        const QString suggestion = QDir(store().effectiveCaptureDirectory())
                                       .filePath(filenameFor(store().effectiveFilenameTemplate(),
                                                             QStringLiteral("selection"), QDateTime::currentDateTime()));
        const QString path = QFileDialog::getSaveFileName(nullptr, tr("Save Screenshot As"), suggestion,
                                                          tr("PNG images (*.png)"));
        if (path.isEmpty()) {
            if (!wasFrozen) {
                frames_.clear();
            }
            showOverlays();
            return;
        }
        const QFileInfo info(path);
        const QString stem = info.path() + QLatin1Char('/') + info.completeBaseName();
        QStringList paths;
        QStringList errors;
        QImage clipboard;
        const QList<Selection> ordered = selections_.inDrawingOrder();
        for (const Selection &selection : ordered) {
            QString error;
            const QImage image = selectionImage(selection, &error);
            if (image.isNull()) {
                errors.append(error);
                continue;
            }
            const int number = selections_.number(selection.id);
            const QString target = number <= 1 ? stem + QStringLiteral(".png")
                                               : QStringLiteral("%1-%2.png").arg(stem).arg(number);
            QSaveFile file(target);
            QImageWriter writer(&file, "png");
            if (!file.open(QIODevice::WriteOnly) || !writer.write(image) || !file.commit()) {
                errors.append(tr("Could not write %1.").arg(target));
                continue;
            }
            paths.append(target);
            if (selection.id == selections_.activeId() || clipboard.isNull()) {
                clipboard = image;
            }
        }
        finishWith(paths, clipboard, errors, tr("Selection"));
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

QString CaptureSession::saveAsAreas(const QString &name, const QKeySequence &shortcut, bool combine)
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

    QStringList areaIds;
    for (const Selection &selection : ordered) {
        const auto screen = std::find_if(screens_.cbegin(), screens_.cend(),
                                         [&](const ScreenInfo &s) { return s.connector == selection.connector; });
        if (screen == screens_.cend()) {
            continue;
        }
        Area &area = targets.addArea(*screen, selection.rect);
        if (ordered.size() == 1) {
            area.name = trimmed;
        }
        areaIds.append(area.id);
    }
    QString shortcutOwner = areaIds.size() == 1 ? areaIds.first() : QString();
    if (ordered.size() > 1 && combine) {
        Group &group = targets.addGroup(trimmed);
        group.targetIds = areaIds;
        shortcutOwner = group.id;
    }
    app_->storeChanged();
    if (!shortcut.isEmpty() && !shortcutOwner.isEmpty()) {
        app_->shortcuts()->setShortcut(shortcutOwner, shortcut);
    }

    const QString saved = ordered.size() == 1 ? tr("Area “%1” saved").arg(trimmed)
                          : combine           ? tr("Group “%1” saved").arg(trimmed)
                                              : tr("%n areas saved", nullptr, int(areaIds.size()));
    finish(saved, printPath_.isEmpty() ? QStringList() : QStringList{printPath_});
    return {};
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
        finishWith(write({{name, out}}, &errors), out, errors, name);
    });
}

void CaptureSession::captureAllMonitors()
{
    if (annotationController_.isEditingText()) {
        annotationController_.requestCommitActiveText(false);
    }
    if (shouldBypassRawPrintFastPath(frozen_, printPath_, annotations_)) {
        // The full screenshot of this moment is already saved without edits.
        finish({}, {printPath_});
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
        finishWith(write({{allMonitorsTargetId(), image}}, &errors), image, errors, tr("All monitors"));
    });
}

void CaptureSession::captureTarget(const QString &id)
{
    const TriggerPlan plan = planTrigger(store(), id, screens_);
    if (plan.outputs.isEmpty()) {
        finish({}, {}, plan.errors);
        return;
    }
    withFrames(plan.connectors, [this, plan] {
        QList<QPair<QString, QImage>> images;
        QStringList errors = plan.errors;
        for (const RenderedOutput &output : renderOutputs(plan, frames_)) {
            if (output.image.isNull()) {
                errors.append(output.error);
            } else {
                images.append({output.token, output.image});
            }
        }
        const QImage clipboard = images.isEmpty() ? QImage() : images.first().second;
        finishWith(write(images, &errors), clipboard, errors, plan.displayName);
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
        if (const Area *area = store().area(areaId); area && isConnected(area->connector)) {
            selections_.add(area->connector, area->rect);
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
    return store().overlayHelpExpanded;
}

void CaptureSession::toggleHelp()
{
    store().overlayHelpExpanded = !store().overlayHelpExpanded;
    app_->saveSettings();
    changed();
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
    if (purpose_ == Purpose::Print && annotations_.hasRedactions() && !printPath_.isEmpty() && !finished_) {
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
                                   .arg(QFileInfo(printPath_).fileName()));
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
            const QString oldPath = printPath_;
            purgeUnredactedPrintFile(oldPath, true);
            app_->forgetCapturePath(oldPath);
            printPath_.clear();
            finish(tr("Unredacted screenshot deleted"), {});
            return;
        }
        Q_UNUSED(keepBtn);
    }
    finish({}, printPath_.isEmpty() ? QStringList() : QStringList{printPath_});
}

void CaptureSession::finishWith(const QStringList &paths, const QImage &clipboardImage, const QStringList &errors,
                                const QString &displayName)
{
    if (paths.isEmpty()) {
        finish({}, {}, errors);
        return;
    }
    // The narrowed-down result replaces the full screenshot taken when Print was pressed.
    if (!printPath_.isEmpty() && !paths.contains(printPath_)) {
        QFile::remove(printPath_);
        printPath_.clear();
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
    app_->recordCapture(paths, displayName);
    finish({}, paths, errors);
}

void CaptureSession::finish(const QString &title, const QStringList &paths, const QStringList &errors)
{
    if (finished_) {
        return;
    }
    finished_ = true;
    for (const QPointer<CaptureOverlay> &overlay : std::as_const(overlays_)) {
        if (overlay) {
            overlay->close();
        }
    }
    overlays_.clear();
    if (purpose_ == Purpose::Print || !title.isEmpty() || !errors.isEmpty()) {
        app_->notifySaved(title, paths, errors);
    }
    Q_EMIT finished();
}

// ------------------------------------------------------------------------------------------ Overlay

CaptureOverlay::CaptureOverlay(CaptureSession *session, const ScreenInfo &screen)
    : session_(session)
    , screen_(screen)
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setWindowTitle(tr("glimply capture on %1").arg(session->monitorName(screen.connector)));
    // Always translucent, so the same window can show the frozen picture or the live screen.
    setAttribute(Qt::WA_TranslucentBackground);
    setCursor(Qt::CrossCursor);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    buildTopBar();
    if (session_->purpose() == CaptureSession::Purpose::Print) {
        buildMarkupBar();
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
    buildActionBar();
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

QPointF CaptureOverlay::toGlobalLogical(const QPointF &localPos) const
{
    return localPos + QPointF(screen_.geometry.topLeft());
}

void CaptureOverlay::buildMarkupBar()
{
    auto *panel = new OverlayPanel(this);
    markupPanel_ = panel;
    auto *layout = new QHBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    annotationToolbar_ = new AnnotationToolbar(&session_->annotationController(), panel);
    annotationToolbar_->setCompactOverlayMode(true);
    annotationToolbar_->setSettings(&session_->store().annotations);
    connect(annotationToolbar_, &AnnotationToolbar::settingsModified, this, [this] {
        session_->persistAnnotationSettings();
    });
    layout->addWidget(annotationToolbar_);
    panel->hide();
}

void CaptureOverlay::buildTopBar()
{
    const bool print = session_->purpose() == CaptureSession::Purpose::Print;
    auto *panel = new OverlayPanel(this);
    topBar_ = panel;
    auto *column = new QVBoxLayout(panel);
    column->setSpacing(4);
    auto *row = new QHBoxLayout;
    row->setSpacing(2);
    column->addLayout(row);

    frozenButton_ = barButton(panel, QStringLiteral("media-playback-pause"), tr("Frozen"),
                              tr("Show the moment you pressed the key. Captures cut from this picture. (Space)"));
    liveButton_ = barButton(panel, QStringLiteral("media-playback-start"), tr("Live"),
                            tr("See the screen move on, for example a video. Captures are taken when you save. "
                               "(Space)"));
    frozenButton_->setCheckable(true);
    liveButton_->setCheckable(true);
    connect(frozenButton_, &QToolButton::clicked, this, [this] { session_->setFrozen(true); });
    connect(liveButton_, &QToolButton::clicked, this, [this] { session_->setFrozen(false); });
    row->addWidget(frozenButton_);
    row->addWidget(liveButton_);

    const auto separator = [&] {
        auto *line = new QFrame(panel);
        line->setFrameShape(QFrame::VLine);
        line->setFrameShadow(QFrame::Sunken);
        row->addSpacing(4);
        row->addWidget(line);
        row->addSpacing(4);
    };
    separator();

    if (print) {
        auto *monitor = barButton(panel, QStringLiteral("video-display"), tr("This Monitor"),
                                  tr("Capture all of %1").arg(session_->monitorName(screen_.connector)));
        connect(monitor, &QToolButton::clicked, this, [this] { session_->captureMonitor(screen_.connector); });
        row->addWidget(monitor);
        auto *all = barButton(panel, QStringLiteral("view-fullscreen"), tr("All Monitors"),
                              tr("Capture every monitor in one picture"));
        connect(all, &QToolButton::clicked, this, [this] { session_->captureAllMonitors(); });
        row->addWidget(all);

        auto *window = barButton(panel, QStringLiteral("window"), tr("Window"),
                                 tr("Capture one window, also when it is covered or minimized"));
        window->setPopupMode(QToolButton::InstantPopup);
        auto *windowMenu = new QMenu(window);
        window->setMenu(windowMenu);
        connect(windowMenu, &QMenu::aboutToShow, this, &CaptureOverlay::fillWindowsMenu);
        connect(windowMenu, &QMenu::aboutToHide, this, [this] { session_->setHighlights({}); });
        row->addWidget(window);
    }

    auto *areas = barButton(panel, QStringLiteral("select-rectangular"), tr("Areas"),
                            print ? tr("Capture a saved area or group, or add it as a rectangle")
                                  : tr("Add a saved area or group as a rectangle"));
    areas->setPopupMode(QToolButton::InstantPopup);
    auto *areasMenu = new QMenu(areas);
    areas->setMenu(areasMenu);
    connect(areasMenu, &QMenu::aboutToShow, this, &CaptureOverlay::fillAreasMenu);
    connect(areasMenu, &QMenu::aboutToHide, this, [this] { session_->setHighlights({}); });
    row->addWidget(areas);

    if (print) {
        annotateButton_ = barButton(panel, QStringLiteral("draw-freehand"), tr("Annotate"),
                                    tr("Show or hide markup tools (A, R, E, B, M, T, N, X, S)"));
        annotateButton_->setCheckable(true);
        connect(annotateButton_, &QToolButton::clicked, this, [this](bool on) { session_->setMarkupVisible(on); });
        row->addWidget(annotateButton_);
    }

    separator();
    helpButton_ = barButton(panel, QStringLiteral("help-contextual"), tr("Help"), tr("Show or hide the controls (F1)"));
    helpButton_->setCheckable(true);
    connect(helpButton_, &QToolButton::clicked, this, [this] { session_->toggleHelp(); });
    row->addWidget(helpButton_);
    if (print) {
        auto *settings = barButton(panel, QStringLiteral("configure"), tr("Settings"),
                                   tr("Keep the full screenshot and open the settings"));
        connect(settings, &QToolButton::clicked, this, [this] { session_->openSettings(); });
        row->addWidget(settings);
    }
    auto *close = barButton(panel, QStringLiteral("window-close"), tr("Close"),
                            print ? tr("Keep the full screenshot and close (Escape)") : tr("Cancel (Escape)"));
    connect(close, &QToolButton::clicked, this, [this] { session_->close(); });
    row->addWidget(close);

    hint_ = new QLabel(panel);
    hint_->setAlignment(Qt::AlignCenter);
    hint_->setWordWrap(true);
    QPalette hintPalette = hint_->palette();
    QColor muted = hintPalette.color(QPalette::WindowText);
    muted.setAlphaF(0.75);
    hintPalette.setColor(QPalette::WindowText, muted);
    hint_->setPalette(hintPalette);
    column->addWidget(hint_);

    helpPanel_ = buildHelpPanel(panel);
    column->addWidget(helpPanel_);
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
        for (const auto &[keys, meaning] : rows) {
            auto *keyLabel = new QLabel(keys, panel);
            keyLabel->setFont(bold);
            keyLabel->setAlignment(Qt::AlignRight | Qt::AlignTop);
            auto *meaningLabel = new QLabel(meaning, panel);
            meaningLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
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
    }
    section(tr("Keys"), keys);
    return panel;
}

void CaptureOverlay::fillWindowsMenu()
{
    auto *menu = qobject_cast<QMenu *>(sender());
    if (!menu) {
        return;
    }
    menu->clear();
    const QList<WindowInfo> windows = session_->windows();
    if (windows.isEmpty()) {
        menu->addAction(tr("No windows found"))->setEnabled(false);
        return;
    }
    for (const WindowInfo &window : windows) {
        QIcon icon = QIcon::fromTheme(window.iconName);
        if (icon.isNull()) {
            icon = QIcon::fromTheme(QStringLiteral("window"));
        }
        QString text = elided(window.caption.isEmpty() ? window.appName : window.caption, 70);
        if (window.minimized) {
            text = tr("%1 (minimized)").arg(text);
        }
        QAction *action = menu->addAction(icon, text);
        connect(action, &QAction::hovered, this, [this, window] {
            session_->setHighlights({{window.geometry, window.caption}});
        });
        connect(action, &QAction::triggered, this, [this, window] { session_->captureWindow(window); });
    }
}

void CaptureOverlay::fillAreasMenu()
{
    auto *menu = qobject_cast<QMenu *>(sender());
    if (!menu) {
        return;
    }
    menu->clear();
    const bool print = session_->purpose() == CaptureSession::Purpose::Print;
    const TargetStore &store = session_->store();
    if (store.areas.isEmpty() && store.groups.isEmpty()) {
        menu->addAction(tr("No saved areas yet. Draw a rectangle, then choose Save as Area."))->setEnabled(false);
        return;
    }

    const auto addRow = [&](const QString &id, const QString &icon, const QString &name, bool available,
                            const QList<CaptureSession::Highlight> &outline) {
        auto *action = new QWidgetAction(menu);
        auto *row = new QWidget(menu);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(4, 1, 4, 1);
        layout->setSpacing(2);
        QString label = name;
        if (const QKeySequence shortcut = session_->shortcut(id); !shortcut.isEmpty()) {
            label = tr("%1    %2").arg(name, shortcut.toString(QKeySequence::NativeText));
        }
        auto *main = barButton(row, icon, label, print ? tr("Capture now") : tr("Add as a rectangle"));
        main->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        main->setEnabled(available);
        layout->addWidget(main);
        connect(main, &QToolButton::clicked, this, [this, menu, id, print] {
            menu->close();
            if (print) {
                session_->captureTarget(id);
            } else {
                session_->addTarget(id);
            }
        });
        if (print) {
            auto *add = barButton(row, QStringLiteral("list-add"), QString(), tr("Add as a rectangle to adjust it"));
            add->setToolButtonStyle(Qt::ToolButtonIconOnly);
            add->setAccessibleName(tr("Add %1 as a rectangle").arg(name));
            add->setEnabled(available);
            layout->addWidget(add);
            connect(add, &QToolButton::clicked, this, [this, menu, id] {
                menu->close();
                session_->addTarget(id);
            });
        }
        // Hovering a row outlines the area on screen.
        row->setAttribute(Qt::WA_Hover);
        row->installEventFilter(new HoverOutline(row, session_, outline));
        action->setDefaultWidget(row);
        menu->addAction(action);
    };

    const auto outlineOf = [&](const QString &areaId) -> QList<CaptureSession::Highlight> {
        const Area *area = store.area(areaId);
        if (!area) {
            return {};
        }
        const QScreen *screen = findScreen(area->connector);
        if (!screen) {
            return {};
        }
        return {{area->rect.translated(screen->geometry().topLeft()), area->name}};
    };

    for (const Group &group : store.groups) {
        QList<CaptureSession::Highlight> outline;
        bool available = false;
        for (const QString &member : group.targetIds) {
            outline += outlineOf(member);
            if (const Area *area = store.area(member); area && session_->isConnected(area->connector)) {
                available = true;
            }
        }
        addRow(group.id, QStringLiteral("object-group"), group.name, available, outline);
    }
    if (!store.groups.isEmpty() && !store.areas.isEmpty()) {
        menu->addSeparator();
    }
    for (const Area &area : store.areas) {
        addRow(area.id, QStringLiteral("select-rectangular"), area.name, session_->isConnected(area.connector),
               outlineOf(area.id));
    }
}

void CaptureOverlay::buildActionBar()
{
    const bool print = session_->purpose() == CaptureSession::Purpose::Print;
    auto *panel = new OverlayPanel(this);
    actionBar_ = panel;
    auto *stack = new QVBoxLayout(panel);
    stack->setSizeConstraint(QLayout::SetFixedSize);

    actionButtons_ = new QWidget(panel);
    auto *row = new QHBoxLayout(actionButtons_);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(2);
    sizeLabel_ = new QLabel(actionButtons_);
    sizeLabel_->setContentsMargins(6, 0, 8, 0);
    row->addWidget(sizeLabel_);

    if (print) {
        // The primary action: a raised default button, like the main button of a dialog.
        auto *save = actionButton(actionButtons_, QStringLiteral("document-save"), tr("Save"),
                                  tr("Save to the capture folder and copy to the clipboard (Enter)"));
        saveButton_ = save;
        save->setFlat(false);
        save->setDefault(true);
        connect(save, &QPushButton::clicked, this, [this] { session_->save(); });
        row->addWidget(save);
        auto *copy = actionButton(actionButtons_, QStringLiteral("edit-copy"), tr("Copy"),
                                  tr("Copy to the clipboard only"));
        connect(copy, &QPushButton::clicked, this, [this] { session_->copy(); });
        row->addWidget(copy);
        auto *saveAs = actionButton(actionButtons_, QStringLiteral("document-save-as"), tr("Save As…"),
                                    tr("Choose where to save"));
        connect(saveAs, &QPushButton::clicked, this, [this] { session_->saveAs(); });
        row->addWidget(saveAs);
        auto *annotate = actionButton(actionButtons_, QStringLiteral("draw-freehand"), tr("Annotate"),
                                      tr("Open markup tools to draw or redact"));
        connect(annotate, &QPushButton::clicked, this, [this] { session_->setMarkupVisible(!session_->markupVisible()); });
        row->addWidget(annotate);
    }
    auto *area = actionButton(actionButtons_, QStringLiteral("bookmark-new"), tr("Save as Area"),
                              tr("Remember this rectangle to capture it again with one click or shortcut"));
    if (!print) {
        area->setFlat(false);
        area->setDefault(true);
    }
    connect(area, &QPushButton::clicked, this, &CaptureOverlay::showAreaPanel);
    row->addWidget(area);
    auto *more = actionButton(actionButtons_, QStringLiteral("list-add"), tr("Add Rectangle"),
                              tr("Draw another rectangle, also over an existing one"));
    connect(more, &QPushButton::clicked, this, [this] { session_->setForceNew(true); });
    row->addWidget(more);
    auto *cancel = actionButton(actionButtons_, QStringLiteral("dialog-cancel"), tr("Cancel"),
                                print ? tr("Keep the full screenshot and close (Escape)") : tr("Close (Escape)"));
    connect(cancel, &QPushButton::clicked, this, [this] { session_->close(); });
    row->addWidget(cancel);
    stack->addWidget(actionButtons_);
    panel->hide();
}

void CaptureOverlay::showActionButtons()
{
    if (areaPanel_) {
        areaPanel_->deleteLater();
        areaPanel_ = nullptr;
    }
    actionButtons_->show();
    refresh();
    setFocus();
}

void CaptureOverlay::showAreaPanel()
{
    SelectionModel &model = session_->selections();
    const int count = model.count();
    if (count == 0 || areaPanel_) {
        return;
    }
    const TargetStore &store = session_->store();
    actionButtons_->hide();

    areaPanel_ = new QWidget(actionBar_);
    auto *form = new QFormLayout(areaPanel_);
    form->setContentsMargins(6, 4, 6, 2);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto *title = new QLabel(areaPanel_);
    QFont bold = title->font();
    bold.setBold(true);
    title->setFont(bold);
    form->addRow(title);

    auto *combine = new QCheckBox(tr("Combine into a group"), areaPanel_);
    combine->setChecked(true);
    combine->setVisible(count > 1);
    if (count > 1) {
        form->addRow(combine);
    }

    auto *name = new QLineEdit(areaPanel_);
    name->setMinimumWidth(260);
    if (count == 1) {
        name->setText(store.nextAreaName(session_->monitorName(model.inDrawingOrder().first().connector)));
    } else {
        int n = 1;
        while (!store.isNameAvailable(tr("Group %1").arg(n))) {
            ++n;
        }
        name->setText(tr("Group %1").arg(n));
    }
    name->selectAll();
    form->addRow(tr("Name:"), name);

    auto *shortcut = new KKeySequenceWidget(areaPanel_);
    shortcut->setCheckForConflictsAgainst(KKeySequenceWidget::GlobalShortcuts | KKeySequenceWidget::StandardShortcuts);
    form->addRow(tr("Shortcut:"), shortcut);

    auto *explain = new QLabel(areaPanel_);
    explain->setWordWrap(true);
    explain->setMaximumWidth(380);
    form->addRow(QString(), explain);

    auto *error = new QLabel(areaPanel_);
    error->setWordWrap(true);
    error->setMaximumWidth(380);
    error->hide();
    form->addRow(QString(), error);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    auto *back = new QPushButton(QIcon::fromTheme(QStringLiteral("go-previous")), tr("Back"), areaPanel_);
    auto *confirm = new QPushButton(QIcon::fromTheme(QStringLiteral("document-save")), QString(), areaPanel_);
    confirm->setDefault(true);
    buttons->addWidget(back);
    buttons->addWidget(confirm);
    form->addRow(buttons);

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
    connect(combine, &QCheckBox::toggled, areaPanel_, update);

    const auto submit = [this, name, shortcut, combine, error] {
        const QString problem = session_->saveAsAreas(name->text(), shortcut->keySequence(), combine->isChecked());
        if (!problem.isEmpty()) {
            error->setText(problem);
            error->show();
            name->setFocus();
            placeActionBar();
        }
    };
    connect(confirm, &QPushButton::clicked, areaPanel_, submit);
    connect(name, &QLineEdit::returnPressed, areaPanel_, submit);
    connect(back, &QPushButton::clicked, this, &CaptureOverlay::showActionButtons);

    actionBar_->layout()->addWidget(areaPanel_);
    activateWindow();
    refresh();
    name->setFocus();
}

const Selection *CaptureOverlay::activeHere() const
{
    const Selection *active = session_->selections().active();
    return active && active->connector == screen_.connector ? active : nullptr;
}

void CaptureOverlay::refresh()
{
    if (topBar_) {
        frozenButton_->setChecked(session_->frozen());
        liveButton_->setChecked(!session_->frozen());
        liveButton_->setEnabled(session_->canSwitchToLive());
        liveButton_->setToolTip(
            session_->canSwitchToLive()
                ? tr("See the screen move on, for example a video. Captures are taken when you save. (Space)")
                : tr("Clear or undo annotations to return to Live mode"));
        if (annotateButton_) {
            annotateButton_->setChecked(session_->markupVisible());
        }
        hint_->setText(session_->hint());
        helpButton_->setChecked(session_->helpExpanded());
        helpPanel_->setVisible(session_->helpExpanded());
        topBar_->adjustSize();
        const QSize size = topBar_->sizeHint();
        topBar_->setGeometry((width() - size.width()) / 2, 16, size.width(), size.height());
        topBar_->show();
        topBar_->raise();
    }
    if (markupPanel_) {
        const bool showMarkup = session_->markupVisible() && session_->purpose() == CaptureSession::Purpose::Print;
        markupPanel_->setVisible(showMarkup);
        if (showMarkup && topBar_) {
            markupPanel_->adjustSize();
            const QSize mSize = markupPanel_->sizeHint();
            const int x = std::clamp((width() - mSize.width()) / 2, 0, std::max(0, width() - mSize.width()));
            const int y = std::clamp(topBar_->geometry().bottom() + 6, 0, std::max(0, height() - mSize.height()));
            markupPanel_->setGeometry(x, y, mSize.width(), mSize.height());
            markupPanel_->raise();
        }
    }
    placeActionBar();
    updateCursor(lastPosition_);
    update();
}

void CaptureOverlay::placeActionBar()
{
    const Selection *active = activeHere();
    if (!active || drag_ == Drag::Create || drag_ == Drag::Move || drag_ == Drag::Resize) {
        actionBar_->hide();
        return;
    }
    const int count = session_->selections().count();
    sizeLabel_->setText(tr("%1 × %2").arg(active->rect.width()).arg(active->rect.height()));
    if (saveButton_) {
        saveButton_->setText(count > 1 ? tr("Save %n Areas", nullptr, count) : tr("Save"));
    }
    actionBar_->adjustSize();
    const QSize size = actionBar_->sizeHint();
    const QRect rect = active->rect;
    int topObstacleBottom = topBar_ ? topBar_->geometry().bottom() + 4 : 0;
    if (markupPanel_ && !markupPanel_->isHidden()) {
        topObstacleBottom = std::max(topObstacleBottom, markupPanel_->geometry().bottom() + 4);
    }
    QPoint position(rect.right() + 1 - size.width(), rect.bottom() + 1 + barGap);
    if (position.y() + size.height() > height()) {
        position.setY(rect.top() - barGap - size.height());
        if (position.y() < topObstacleBottom && position.y() < 0) {
            position.setY(rect.bottom() - size.height() - barGap);
        }
    }
    if (position.y() < 0) {
        position.setY(rect.bottom() - size.height() - barGap);
    }
    position.setX(std::clamp(position.x(), 0, std::max(0, width() - size.width())));
    position.setY(std::clamp(position.y(), 0, std::max(0, height() - size.height())));
    actionBar_->setGeometry(QRect(position, size));
    actionBar_->show();
    actionBar_->raise();
    if (topBar_) {
        topBar_->raise();
    }
    if (markupPanel_ && !markupPanel_->isHidden()) {
        markupPanel_->raise();
    }
}

QRect CaptureOverlay::closeButton(const QRect &rect) const
{
    QRect button(0, 0, closeButtonSize, closeButtonSize);
    // Inside the top right corner, clear of the corner handle; outside on small rectangles.
    constexpr int inset = 20;
    if (rect.width() >= 3 * inset && rect.height() >= 3 * inset) {
        button.moveCenter(QPoint(rect.right() - inset, rect.top() + inset));
    } else {
        button.moveCenter(QPoint(rect.right() + closeButtonSize / 2 + 4, rect.top() - closeButtonSize / 2 - 4));
    }
    return clampToArea(button, size());
}

int CaptureOverlay::closeButtonAt(const QPoint &pos) const
{
    const SelectionModel &model = session_->selections();
    for (const int id : {model.activeId(), hoverId_}) {
        const Selection *selection = model.find(id);
        if (selection && selection->connector == screen_.connector && closeButton(selection->rect).contains(pos)) {
            return id;
        }
    }
    return 0;
}

void CaptureOverlay::paintEvent(QPaintEvent *)
{
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
            if (selection.connector == screen_.connector) {
                holes.addRect(selection.rect);
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
    for (const Selection &selection : model.stack()) {
        if (selection.connector != screen_.connector) {
            continue;
        }
        const bool active = selection.id == model.activeId();
        const QRectF frame(selection.rect);
        painter.setBrush(Qt::NoBrush);
        if (active) {
            painter.setPen(QPen(accent, 2));
            painter.drawRect(frame.adjusted(1, 1, -1, -1));
        } else {
            // A hovered rectangle shows in the theme's negative colour: a right click deletes it.
            const bool hovered = selection.id == hoverId_ && drag_ == Drag::None;
            painter.setPen(QPen(QColor(0, 0, 0, 120), 3));
            painter.drawRect(frame.adjusted(0.5, 0.5, -0.5, -0.5));
            painter.setPen(QPen(hovered ? negative : QColor(255, 255, 255, 200), hovered ? 2 : 1));
            painter.drawRect(frame.adjusted(0.5, 0.5, -0.5, -0.5));
        }

        if (numbered) {
            const int number = model.number(selection.id);
            QRectF badge(0, 0, 22, 22);
            badge.moveCenter(frame.topLeft() + QPointF(14, 14));
            painter.setPen(QPen(QColor(255, 255, 255, 230), 1.5));
            painter.setBrush(active ? accent : QColor(60, 60, 60, 220));
            painter.drawEllipse(badge);
            QFont font = painter.font();
            font.setBold(true);
            painter.setFont(font);
            painter.setPen(active ? pal.color(QPalette::HighlightedText) : QColor(Qt::white));
            painter.drawText(badge, Qt::AlignCenter, QString::number(number));
        }

        if (active || selection.id == hoverId_) {
            const QRect button = closeButton(selection.rect);
            painter.setPen(QPen(QColor(0, 0, 0, 60), 1));
            painter.setBrush(pal.color(QPalette::Window));
            painter.drawEllipse(QRectF(button).adjusted(0.5, 0.5, -0.5, -0.5));
            const QIcon icon = QIcon::fromTheme(QStringLiteral("window-close"));
            const QRect iconRect(button.center() - QPoint(7, 7), QSize(16, 16));
            icon.paint(&painter, iconRect.adjusted(1, 1, -1, -1));
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

void CaptureOverlay::updateCursor(const QPoint &pos)
{
    if (closeButtonAt(pos)) {
        setCursor(Qt::PointingHandCursor);
        return;
    }
    if (session_->purpose() == CaptureSession::Purpose::Print) {
        AnnotationController &ctrl = session_->annotationController();
        const Qt::KeyboardModifiers mods = QGuiApplication::keyboardModifiers();
        const QPointF gPos = toGlobalLogical(pos);
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
    const Selection *active = activeHere();
    if (active && !session_->forceNew()) {
        const Qt::Edges edges = edgesAt(active->rect, pos);
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
        if (active->rect.contains(pos)) {
            setCursor(Qt::SizeAllCursor);
            return;
        }
    }
    if (!session_->forceNew() && session_->selections().topmostAt(screen_.connector, pos)) {
        setCursor(Qt::PointingHandCursor);
        return;
    }
    setCursor(Qt::CrossCursor);
}

void CaptureOverlay::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton) {
        // Right click deletes the topmost annotation under pointer first, else the crop rectangle.
        if (drag_ == Drag::None) {
            const QPoint pos = event->position().toPoint();
            if (session_->purpose() == CaptureSession::Purpose::Print
                && !session_->annotationController().isDragging()) {
                if (const int annId = session_->annotations().topmostHitAt(toGlobalLogical(pos))) {
                    session_->annotations().removeItem(annId);
                    updateCursor(pos);
                    return;
                }
            }
            if (const int id = session_->selections().topmostAt(screen_.connector, pos)) {
                session_->selections().remove(id);
                hoverId_ = 0;
                session_->changed();
                updateCursor(pos);
            }
        }
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }
    if (areaPanel_) {
        showActionButtons();
    }
    if (session_->purpose() == CaptureSession::Purpose::Print
        && session_->annotationController().isEditingText()) {
        session_->annotationController().requestCommitActiveText(false);
        return;
    }

    SelectionModel &model = session_->selections();
    const QPoint pos = event->position().toPoint();
    anchor_ = pos;
    lastPosition_ = pos;
    spaceHeld_ = false;

    const bool selectTool = session_->purpose() != CaptureSession::Purpose::Print
                         || session_->annotationController().effectiveTool(event->modifiers()) == ToolKind::Select;
    if (selectTool) {
        if (const int id = closeButtonAt(pos)) {
            model.remove(id);
            hoverId_ = 0;
            drag_ = Drag::None;
            session_->changed();
            updateCursor(pos);
            return;
        }
    }

    if (session_->purpose() == CaptureSession::Purpose::Print) {
        if (session_->annotationController().mousePress(toGlobalLogical(event->position()), Qt::LeftButton,
                                                        event->modifiers(), 8.0)) {
            return;
        }
    }

    const Selection *active = activeHere();
    if (active && !session_->forceNew()) {
        dragStartRect_ = active->rect;
        dragId_ = active->id;
        if (const Qt::Edges edges = edgesAt(active->rect, pos)) {
            drag_ = Drag::Resize;
            dragEdges_ = edges;
            return;
        }
        if (active->rect.contains(pos)) {
            drag_ = Drag::Move;
            return;
        }
    }
    // Decide on release (click) or on the first movement (drag).
    drag_ = Drag::Pending;
    pendingId_ = session_->forceNew() ? 0 : model.topmostAt(screen_.connector, pos);
}

void CaptureOverlay::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint position = event->position().toPoint();
    const QPoint step = position - lastPosition_;
    lastPosition_ = position;

    if (session_->purpose() == CaptureSession::Purpose::Print && session_->annotationController().isDragging()) {
        session_->annotationController().mouseMove(toGlobalLogical(event->position()), event->modifiers());
        return;
    }

    if (drag_ == Drag::None) {
        if (session_->purpose() == CaptureSession::Purpose::Print) {
            AnnotationController &ctrl = session_->annotationController();
            const bool annHit = ctrl.mouseMove(toGlobalLogical(event->position()), event->modifiers());
            if (annHit || ctrl.effectiveTool(event->modifiers()) != ToolKind::Select) {
                if (hoverId_) {
                    hoverId_ = 0;
                    update();
                }
                updateCursor(position);
                return;
            }
        }
        const int hover = session_->selections().topmostAt(screen_.connector, position);
        if (hover != hoverId_) {
            hoverId_ = hover;
            update();
        }
        updateCursor(position);
        return;
    }

    if (drag_ == Drag::Pending) {
        if ((position - anchor_).manhattanLength() < QApplication::startDragDistance()) {
            return;
        }
        drag_ = Drag::Create;
        dragId_ = session_->selections().add(screen_.connector, QRect(anchor_, QSize(1, 1)));
        session_->setForceNew(false);
        session_->setDragging(true);
    } else if (drag_ == Drag::Move || drag_ == Drag::Resize) {
        session_->setDragging(drag_ == Drag::Resize);
    }
    if (spaceHeld_ && (drag_ == Drag::Create || drag_ == Drag::Resize)) {
        anchor_.setX(std::clamp(anchor_.x() + step.x(), 0, width()));
        anchor_.setY(std::clamp(anchor_.y() + step.y(), 0, height()));
        if (drag_ == Drag::Resize) {
            dragStartRect_ = clampToArea(dragStartRect_.translated(step), size());
        }
    }
    applyDrag(position, event->modifiers());
}

void CaptureOverlay::applyDrag(const QPoint &position, Qt::KeyboardModifiers modifiers)
{
    const SelectionConstraints constraints{modifiers.testFlag(Qt::AltModifier),
                                           modifiers.testFlag(Qt::ShiftModifier)};
    SelectionModel &model = session_->selections();
    const QSize bounds = size();
    if (drag_ == Drag::Create) {
        model.setRect(dragId_, drawnSelection(anchor_, position, constraints, bounds));
    } else if (drag_ == Drag::Move) {
        model.setRect(dragId_, clampToArea(dragStartRect_.translated(position - anchor_), bounds));
    } else if (drag_ == Drag::Resize) {
        model.setRect(dragId_, resizedSelection(dragStartRect_, dragEdges_, position - anchor_, constraints, bounds));
    } else {
        return;
    }
    placeActionBar();
    update();
}

void CaptureOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }
    if (session_->purpose() == CaptureSession::Purpose::Print && session_->annotationController().isDragging()) {
        session_->annotationController().mouseRelease(toGlobalLogical(event->position()), Qt::LeftButton,
                                                      event->modifiers());
        session_->persistAnnotationSettings();
        updateCursor(event->position().toPoint());
        return;
    }
    SelectionModel &model = session_->selections();
    const Drag drag = std::exchange(drag_, Drag::None);
    spaceHeld_ = false;
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
    updateCursor(event->position().toPoint());
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
    if (const int id = model.topmostAt(screen_.connector, event->position().toPoint())) {
        model.activate(id);
        if (session_->purpose() == CaptureSession::Purpose::Print) {
            session_->save(screen_.connector);
        } else {
            session_->changed();
            showAreaPanel();
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
    if (hoverId_) {
        hoverId_ = 0;
        update();
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

    if (areaPanel_) {
        if (event->key() == Qt::Key_Escape) {
            showActionButtons();
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

    if (session_->purpose() == CaptureSession::Purpose::Print && plainKey && !event->isAutoRepeat()) {
        switch (event->key()) {
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
        if (session_->purpose() == CaptureSession::Purpose::Print) {
            session_->save(screen_.connector);
        } else {
            showAreaPanel();
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
    const Selection *active = activeHere();
    if (!active) {
        QWidget::keyPressEvent(event);
        return;
    }
    model.setRect(active->id, steppedSelection(active->rect, delta, constraints, minimumAreaSize, size()));
    session_->changed();
}

} // namespace glimply
