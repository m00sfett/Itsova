#include "app.h"

#include "notifyconfig.h"
#include "annotations/annotationdocument.h"
#include "annotations/overlayexport.h"
#include "annotations/programmaticannotator.h"
#include "areaselector.h"
#include "captureoverlay.h"
#include "logging.h"
#include "recordingmetadata.h"
#include "captureservice.h"
#include "cli.h"
#include "colorpicker.h"
#include "countdowntimer.h"
#include "historystore.h"
#include "hookrunner.h"
#include "intervalcontroller.h"
#include "latencytrace.h"
#include "mcpconfirmdialog.h"
#include "mcppolicy.h"
#include "mcpserver.h"
#include "ocrengine.h"
#include "pinmanager.h"
#include "printkey.h"
#include "quickeditorwindow.h"
#include "recordingplan.h"
#include "recordinghint.h"
#include "replaymanager.h"
#include "screenrecorder.h"
#include "controlservice.h"

#include <algorithm>
#include <mutex>
#include <utility>
#include "imageoutput.h"
#include "screens.h"
#include "settingswindow.h"
#include "shortcutmanager.h"
#include "thumbnailcache.h"
#include "traycontroller.h"
#include "triggerengine.h"
#include "triggerplan.h"
#include "windowlist.h"

#include <KConfig>
#include <KGlobalAccel>
#include <KNotification>

#include <QBuffer>
#include <QClipboard>
#include <QCoreApplication>
#include <QProcess>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QImageReader>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QLocale>
#include <QMimeData>
#include <QPointer>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent>

namespace itsova
{


namespace
{
// Recording hints (#125): short enough not to be in the way, long enough to read the title.
constexpr int recordingHintMs = 1500;
// The stop hint can be clicked to open the file.
constexpr int recordingSavedHintMs = 4000;
// After hiding the hint, until the compositor has taken it off the screen.
constexpr int recordingHintGoneMs = 200;
} // namespace


namespace
{

QString recordingStateKey(ScreenRecorder::State state)
{
    switch (state) {
    case ScreenRecorder::State::Starting:
        return QStringLiteral("starting");
    case ScreenRecorder::State::Recording:
        return QStringLiteral("recording");
    case ScreenRecorder::State::Paused:
        return QStringLiteral("paused");
    case ScreenRecorder::State::Finishing:
        return QStringLiteral("finishing");
    case ScreenRecorder::State::Idle:
        break;
    }
    return QStringLiteral("idle");
}

} // namespace

std::optional<RecordingTarget> App::recordingTargetFor(const QString &targetId, QString *error) const
{
    const QList<ScreenInfo> screens = currentScreens();
    QList<QRect> rects;
    QList<qreal> scales;
    for (const ScreenInfo &screen : screens) {
        rects.append(screen.geometry);
        scales.append(nativeScale(screen));
    }
    const auto fail = [error](const QString &message) -> std::optional<RecordingTarget> {
        if (error) {
            *error = message;
        }
        return std::nullopt;
    };

    RecordingTarget target;
    target.targetId = targetId;
    QRect region;
    if (isWindowRecordingTargetId(targetId)) {
        const QString handle = windowHandleFromRecordingTargetId(targetId);
        const QList<WindowInfo> windows = listWindows(QCoreApplication::applicationPid());
        const auto it = std::find_if(windows.cbegin(), windows.cend(),
                                     [&](const WindowInfo &w) { return w.handle == handle; });
        if (it == windows.cend()) {
            return fail(tr("The window to record is no longer open."));
        }
        target.kind = RecordingTarget::Kind::Window;
        target.windowHandle = handle;
        target.region = it->geometry; // where the window is, for the recording's metadata (#103)
        target.displayName = it->caption.isEmpty() ? it->appName : it->caption;
        target.token = windowToken(*it);
        return target;
    }
    if (const std::optional<QRect> rect = regionFromRecordingTargetId(targetId)) {
        region = *rect;
        target.displayName = tr("Selection");
        target.token = QStringLiteral("selection");
    } else if (targetId == allMonitorsTargetId()) {
        for (const QRect &rect : std::as_const(rects)) {
            region = region.united(rect);
        }
        target.displayName = tr("All monitors");
        target.token = QStringLiteral("all");
    } else if (isScreenTargetId(targetId)) {
        const QString connector = connectorFromScreenTargetId(targetId);
        const auto it = std::find_if(screens.cbegin(), screens.cend(),
                                     [&](const ScreenInfo &s) { return s.connector == connector; });
        if (it == screens.cend()) {
            return fail(tr("Monitor %1 is not connected.").arg(connector));
        }
        region = it->geometry;
        target.displayName = store_.monitorName(*it);
        target.token = connector;
    } else if (const Area *area = store_.area(targetId)) {
        if (area->isCrossMonitor()) {
            region = area->rect;
        } else {
            const auto it = std::find_if(screens.cbegin(), screens.cend(),
                                         [&](const ScreenInfo &s) { return s.connector == area->connector; });
            if (it == screens.cend()) {
                return fail(tr("The monitor of area %1 is not connected.").arg(area->name));
            }
            region = area->rect.translated(it->geometry.topLeft());
        }
        target.displayName = area->name;
        target.token = area->name;
    } else if (store_.group(targetId)) {
        return fail(tr("Groups cannot be recorded; choose one of their targets."));
    } else {
        return fail(tr("Unknown recording target: %1").arg(targetId));
    }

    region = clipRegionToScreens(region, rects);
    if (region.isEmpty()) {
        return fail(tr("The area to record is not on any connected monitor."));
    }
    target.kind = RecordingTarget::Kind::Region;
    target.scale = regionScale(region, rects, scales);
    target.region = evenSizedRegion(region, target.scale);
    return target;
}

QString App::startRecording(const QString &targetId, int delaySeconds)
{
    qCInfo(lcRecord) << "start requested for" << targetId << "delay" << delaySeconds;
    if (recorder_->isBusy() || recordingStartPending()) {
        return tr("A recording is already running.");
    }
    if (targetId == activeWindowRecordingTargetId()) {
        // #124: the window that has focus now (the overlay is already gone).
        const QString window =
            resolveDefaultRecordingTarget(targetId, listWindows(QCoreApplication::applicationPid()));
        if (window.isEmpty()) {
            return tr("No window has the focus.");
        }
        return startRecording(window, delaySeconds);
    }
    QString error;
    if (!recordingTargetFor(targetId, &error)) {
        return error;
    }
    const int delay = std::clamp(delaySeconds, 0, 60);
    if (delay > 0) {
        if (countdown_->isActive()) {
            return tr("A capture countdown is already running.");
        }
        runWithDelay(delay, tr("Recording"), [this, targetId] { beginRecording(targetId); });
        return {};
    }
    beginRecording(targetId);
    return {};
}

void App::noteNativeScale(const ScreenInfo &screen, qreal scale)
{
    nativeScales_.insert(screen.connector, {screen.geometry, scale});
}

qreal App::nativeScale(const ScreenInfo &screen) const
{
    const auto it = nativeScales_.constFind(screen.connector);
    if (it != nativeScales_.cend() && it->first == screen.geometry && it->second > 0) {
        return it->second;
    }
    const QScreen *qscreen = findScreen(screen.connector);
    return qscreen ? qscreen->devicePixelRatio() : 1.0;
}

void App::beginRecording(const QString &targetId)
{
    // Region streams need each monitor's real scale, which only a capture reveals. One frame per
    // monitor whose scale is not known yet (about 20 ms); later recordings reuse it.
    QStringList unknown;
    if (!isWindowRecordingTargetId(targetId)) {
        for (const ScreenInfo &screen : currentScreens()) {
            const auto it = nativeScales_.constFind(screen.connector);
            if (it == nativeScales_.cend() || it->first != screen.geometry) {
                unknown.append(screen.connector);
            }
        }
    }
    if (unknown.isEmpty()) {
        startRecorder(targetId);
        return;
    }
    if (recordingPending_) {
        return;
    }
    recordingPending_ = true;
    auto *watcher = new QFutureWatcher<QHash<QString, qreal>>(this);
    connect(watcher, &QFutureWatcher<QHash<QString, qreal>>::finished, this, [this, watcher, targetId] {
        const QHash<QString, qreal> scales = watcher->result();
        watcher->deleteLater();
        recordingPending_ = false;
        for (const ScreenInfo &screen : currentScreens()) {
            if (const qreal scale = scales.value(screen.connector); scale > 0) {
                nativeScales_.insert(screen.connector, {screen.geometry, scale});
            }
        }
        startRecorder(targetId);
    });
    QHash<QString, int> logicalWidths;
    for (const ScreenInfo &screen : currentScreens()) {
        logicalWidths.insert(screen.connector, screen.geometry.width());
    }
    watcher->setFuture(QtConcurrent::run([unknown, logicalWidths] {
        QHash<QString, qreal> scales;
        for (const QString &connector : unknown) {
            const CaptureResult result = CaptureService::captureScreen(connector);
            const int logical = logicalWidths.value(connector);
            if (!result.image.isNull() && logical > 0) {
                scales.insert(connector, qreal(result.image.width()) / logical);
            } else {
                qCWarning(lcRecord) << "no native scale for" << connector << ":" << result.error;
            }
        }
        return scales;
    }));
}

void App::startRecorder(const QString &targetId)
{
    QString error;
    const std::optional<RecordingTarget> target = recordingTargetFor(targetId, &error);
    if (!target) {
        notifyError(tr("Recording failed"), {error});
        return;
    }
    const QString directory = store_.effectiveCaptureDirectory();
    if (!QDir().mkpath(directory)) {
        notifyError(tr("Recording failed"), {tr("The folder %1 could not be created.").arg(directory)});
        return;
    }
    ScreenRecorder::Options options;
    options.format = store_.recordingFormat;
    options.frameRate = store_.recordingFrameRate;
    options.includeCursor = store_.recordingCursor;
    {
        // Where the recording comes from (#103), for a later replay on the same monitors.
        const QList<ScreenInfo> screens = currentScreens();
        QList<qreal> scales;
        for (const ScreenInfo &screen : screens) {
            scales.append(nativeScale(screen));
        }
        options.metadata = recordingMetadataJson(buildRecordingMetadata(*target, screens, scales));
    }
    options.path = uniqueRecordingPath(
        directory, recordingFileName(store_.effectiveFilenameTemplate(), target->token, QDateTime::currentDateTime(),
                                     options.format));
    if (!store_.recordingHints) {
        recorder_->start(*target, options); // reports its own errors through failed()
        return;
    }
    // #125: say it at once, where the recording does not see it. If it would see it everywhere,
    // the recording starts once the hint has gone.
    const QList<ScreenInfo> screens = currentScreens();
    QList<QRect> rects;
    for (const ScreenInfo &screen : screens) {
        rects.append(screen.geometry);
    }
    if (!recordingHint_) {
        recordingHint_ = new RecordingHint;
    }
    recordingHint_->setContent(QStringLiteral("media-record"), tr("Recording %1").arg(target->displayName),
                               recordingStopInstruction(), false);
    recordingHintPlacement_ = planRecordingHint(target->kind == RecordingTarget::Kind::Window, target->region, rects,
                                                recordingHint_->size());
    recordingHintConnector_ =
        recordingHintPlacement_.screen >= 0 ? screens.at(recordingHintPlacement_.screen).connector : QString();
    if (!recordingHintPlacement_.waitBeforeRecording) {
        showRecordingHint(QStringLiteral("media-record"), tr("Recording %1").arg(target->displayName),
                          recordingStopInstruction(), recordingHintMs);
        recorder_->start(*target, options);
        return;
    }
    showRecordingHint(QStringLiteral("media-record"), tr("Recording %1 starts…").arg(target->displayName),
                      recordingStopInstruction(), recordingHintMs);
    qCInfo(lcRecord) << "recording waits for its start hint";
    if (!recordingStartAfterHint_) {
        recordingStartAfterHint_ = new QTimer(this);
        recordingStartAfterHint_->setSingleShot(true);
    }
    recordingStartAfterHint_->disconnect();
    connect(recordingStartAfterHint_, &QTimer::timeout, this, [this, target = *target, options] {
        qCInfo(lcRecord) << "start hint gone, starting the recording";
        syncRecordingUi();
        recorder_->start(target, options);
    });
    // The hint goes after recordingHintMs; the compositor needs a moment to take it off the screen.
    recordingStartAfterHint_->start(recordingHintMs + recordingHintGoneMs);
    syncRecordingUi();
}

void App::cancelPendingRecordingStart()
{
    if (recordingStartAfterHint_ && recordingStartAfterHint_->isActive()) {
        recordingStartAfterHint_->stop();
        qCInfo(lcRecord) << "recording cancelled before it started";
        if (recordingHint_) {
            recordingHint_->dismiss();
        }
        syncRecordingUi();
    }
}

bool App::recordingStartPending() const
{
    return recordingStartAfterHint_ && recordingStartAfterHint_->isActive();
}

QString App::recordingStopInstruction() const
{
    const QString shortcut = recordingStopShortcutText();
    return shortcut.isEmpty() ? tr("Stop with the Itsova icon in the system tray")
                              : tr("Stop with the Itsova tray icon or %1").arg(shortcut);
}

void App::showRecordingHint(const QString &iconName, const QString &title, const QString &detail, int durationMs,
                            const std::function<void()> &onClick)
{
    if (!store_.recordingHints) {
        return;
    }
    if (!recordingHint_) {
        recordingHint_ = new RecordingHint;
    }
    recordingHint_->disconnect(this);
    recordingHint_->dismiss();
    recordingHint_->setContent(iconName, title, detail, bool(onClick));
    if (onClick) {
        connect(recordingHint_, &RecordingHint::clicked, this, onClick);
    }
    // The place planned at the start, with this hint's size; without one (a failed start before
    // planning), the top of the first monitor.
    const QList<ScreenInfo> screens = currentScreens();
    QList<QRect> rects;
    for (const ScreenInfo &screen : screens) {
        rects.append(screen.geometry);
    }
    int index = -1;
    for (int i = 0; i < screens.size(); ++i) {
        if (screens.at(i).connector == recordingHintConnector_) {
            index = i;
        }
    }
    QRect rect;
    if (index >= 0) {
        rect = QRect(recordingHintPlacement_.rect.center() - QPoint(recordingHint_->width() / 2, recordingHint_->height() / 2),
                     recordingHint_->size());
        rect.moveLeft(std::clamp(rect.left(), rects.at(index).left(), rects.at(index).right() - rect.width()));
    } else if (!rects.isEmpty()) {
        index = 0;
        rect = QRect(QPoint(rects.first().center().x() - recordingHint_->width() / 2, rects.first().top() + 24),
                     recordingHint_->size());
    } else {
        return;
    }
    QScreen *screen = nullptr;
    for (QScreen *candidate : QGuiApplication::screens()) {
        if (candidate->name() == screens.at(index).connector) {
            screen = candidate;
        }
    }
    qCInfo(lcRecord) << "recording hint" << title << "at" << rect << "on" << screens.at(index).connector;
    recordingHint_->showAt(screen, rect, durationMs);
}

void App::stopRecording(const QString &origin)
{
    if (recordingStartPending()) {
        cancelPendingRecordingStart();
        return;
    }
    recorder_->stop(origin);
}

void App::setRecordingPaused(bool paused)
{
    // #125: when everything is recorded, the resume hint shows while still paused and the
    // recording goes on once it has gone.
    if (!paused && store_.recordingHints && recordingHintPlacement_.waitBeforeRecording
        && recorder_->state() == ScreenRecorder::State::Paused) {
        if (recordingResumeAfterHint_ && recordingResumeAfterHint_->isActive()) {
            return;
        }
        showRecordingHint(QStringLiteral("media-record"), tr("Recording resumes…"), recordingStopInstruction(),
                          recordingHintMs);
        resumeHintShown_ = true;
        if (!recordingResumeAfterHint_) {
            recordingResumeAfterHint_ = new QTimer(this);
            recordingResumeAfterHint_->setSingleShot(true);
            connect(recordingResumeAfterHint_, &QTimer::timeout, this, [this] { recorder_->setPaused(false); });
        }
        recordingResumeAfterHint_->start(recordingHintMs + recordingHintGoneMs);
        return;
    }
    recorder_->setPaused(paused);
}

void App::toggleRecording(const QString &origin)
{
    if (recordingStartPending()) {
        cancelPendingRecordingStart();
        return;
    }
    if (recorder_->isBusy()) {
        recorder_->stop(origin);
        return;
    }
    // #124: only the pinned default; without one (or when it cannot be recorded now) ask in the
    // overlay instead of doing nothing.
    if (!store_.defaultRecordingTarget.isEmpty()) {
        bool windowGone = false;
        const QString targetId = resolveDefaultRecordingTarget(
            store_.defaultRecordingTarget, listWindows(QCoreApplication::applicationPid()), &windowGone);
        if (windowGone) {
            qCInfo(lcRecord) << "default recording window is gone, default cleared";
            setDefaultRecordingTarget(QString());
        } else if (!targetId.isEmpty() && recordingTargetFor(targetId, nullptr)) {
            const QString error = startRecording(targetId);
            if (!error.isEmpty()) {
                notifyError(tr("Recording failed"), {error});
            }
            return;
        }
    }
    openOverlayForRecording();
}

QString App::recordingTargetName(const QString &targetId) const
{
    if (targetId == activeWindowRecordingTargetId()) {
        return tr("Active window");
    }
    if (const std::optional<QRect> rect = regionFromRecordingTargetId(targetId)) {
        return tr("Rectangle %1 × %2 at %3, %4").arg(rect->width()).arg(rect->height()).arg(rect->x()).arg(rect->y());
    }
    if (const std::optional<RecordingTarget> target = recordingTargetFor(targetId, nullptr)) {
        return isWindowRecordingTargetId(targetId) ? tr("Window %1").arg(target->displayName) : target->displayName;
    }
    return isWindowRecordingTargetId(targetId) ? tr("A window that is no longer open") : targetId;
}

void App::setDefaultRecordingTarget(const QString &targetId)
{
    if (store_.defaultRecordingTarget == targetId) {
        return;
    }
    qCInfo(lcRecord) << "default recording target" << (targetId.isEmpty() ? QStringLiteral("cleared") : targetId);
    store_.defaultRecordingTarget = targetId;
    saveSettings();
    Q_EMIT defaultRecordingTargetChanged();
}

void App::openOverlayForRecording()
{
    qCInfo(lcOverlay) << "record overlay requested";
    if (session_ || selector_ || colorPicker_ || recorder_->isBusy()) {
        return;
    }
    auto *session = new CaptureSession(this, CaptureSession::Purpose::Print);
    session->setRecordMode(true);
    session->setForcedFrozen(false);
    session->setForceShowOverlay(true);
    session_ = session;
    connect(session, &CaptureSession::finished, session, &QObject::deleteLater);
    startWithPinsHidden(session, [session] { session->start(); });
}

void App::quit()
{
    if (!recorder_->isBusy()) {
        QCoreApplication::quit();
        return;
    }
    // Quitting mid-recording keeps what was recorded.
    connect(recorder_, &ScreenRecorder::stateChanged, this, [this] {
        if (!recorder_->isBusy()) {
            QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        }
    });
    QTimer::singleShot(10000, qApp, &QCoreApplication::quit);
    recorder_->stop(QStringLiteral("quit"));
}

bool App::isRecording() const
{
    return recorder_->isBusy();
}

QJsonObject App::recordingState() const
{
    QJsonObject state;
    state.insert(QStringLiteral("state"), recordingStateKey(recorder_->state()));
    state.insert(QStringLiteral("elapsedMs"), double(recorder_->elapsedMs()));
    state.insert(QStringLiteral("format"), recordingFormatKey(store_.recordingFormat));
    state.insert(QStringLiteral("canPause"), ScreenRecorder::canPause());
    state.insert(QStringLiteral("stopHint"), recordingStopHint(recordingStopShortcutText()));
    if (recorder_->isBusy()) {
        state.insert(QStringLiteral("target"), recorder_->target().targetId);
        state.insert(QStringLiteral("targetName"), recorder_->target().displayName);
        state.insert(QStringLiteral("path"), recorder_->path());
    }
    return state;
}

QString App::recordingStopShortcutText() const
{
    return shortcuts()->shortcut(recordToggleActionId()).toString(QKeySequence::NativeText);
}

void App::syncRecordingUi()
{
    tray_->setRecordingStopShortcut(recordingStopShortcutText());
    tray_->setRecordingState(int(recorder_->state()), recorder_->elapsedMs(), recorder_->target().displayName);
    Q_EMIT recordingChanged();
}

QString App::startReplay(const QString &filePath, const QString &explicitScreen)
{
    if (!replay_) {
        return tr("Replay is not available.");
    }
    QStringList missing;
    const QString err = replay_->startReplay(filePath, explicitScreen, QCursor::pos(), &missing);
    if (!err.isEmpty()) {
        return err;
    }
    if (!missing.isEmpty()) {
        notifyPartialCapture(tr("Replay started without missing monitor(s)"), missing);
    }
    return {};
}

void App::stopReplay()
{
    if (replay_) {
        replay_->stopAll();
    }
}

bool App::isReplayActive() const
{
    return replay_ && replay_->isRunning();
}

} // namespace itsova
