#include "app.h"

#include "annotations/annotationdocument.h"
#include "annotations/programmaticannotator.h"
#include "areaselector.h"
#include "captureoverlay.h"
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
#include <QSaveFile>
#include <QScreen>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent>

namespace glimply
{

App::App(QObject *parent)
    : QObject(parent)
    , config_(KSharedConfig::openConfig(QStringLiteral("glimplyrc"), KConfig::SimpleConfig))
    , shortcuts_(new ShortcutManager(this))
    , engine_(new TriggerEngine(this))
    , tray_(new TrayController([this] { return menuTargets(); }, this))
    , countdown_(new CountdownController(this))
    , countdownBanner_(new CountdownBanner(countdown_))
    , intervals_(new IntervalController(
          [this](const QString &targetId) { return planTrigger(store_, targetId, currentScreens()); },
          [this] { return store_.effectiveCaptureDirectory(); },
          [this] { return store_.effectiveFilenameTemplate(); },
          [this](TriggerPlan plan, const QString &dir, const QString &tmpl,
                 std::optional<TriggerEngine::FrameMap> prev,
                 std::function<void(TriggerResult, TriggerEngine::FrameMap)> done) {
              // Pins turn transparent for the tick so they never end up in a capture (#28).
              auto run = [this, plan, dir, tmpl, prev, done]() {
                  engine_->runIntervalTick(plan, dir, tmpl, prev,
                                           [this, done](TriggerResult result, TriggerEngine::FrameMap frames) {
                                               pins_->endCapture();
                                               done(std::move(result), std::move(frames));
                                           });
              };
              pins_->beginCapture(this, run);
          },
          this))
    , hooks_(new HookRunner(this))
{
    store_.load(*config_);
    tray_->setDelayState(store_.captureDelaySeconds, store_.keepDelayActive);

    ocr_ = std::make_shared<TesseractOcrEngine>();
    pins_ = new PinManager(this);
    connect(pins_, &PinManager::changed, this, [this] { tray_->setPinState(pins_->count(), pins_->pinsShown()); });
    connect(pins_, &PinManager::copyRequested, this, [](const QImage &image) {
        QGuiApplication::clipboard()->setImage(image);
    });
    connect(pins_, &PinManager::editRequested, this,
            [this](const QImage &image) { openEditorFromCapture(image, 1.0, nullptr, QStringLiteral("pin")); });
    connect(pins_, &PinManager::saveAsRequested, this, &App::savePinImageAs);

    history_ = std::make_unique<HistoryStore>(HistoryStore::defaultFilePath(), store_.historyLimit);
    thumbnails_ = std::make_unique<ThumbnailCache>();
    QString historyError;
    if (!history_->load(&historyError)) {
        qWarning("glimply: ignoring unreadable capture history %s: %s", qPrintable(history_->filePath()),
                 qPrintable(historyError));
    }
    if (history_->prune()) {
        history_->save();
    }
    thumbnails_->sweep([this] {
        QStringList files;
        for (const HistoryEntry &entry : history_->entries()) {
            files += entry.files;
        }
        return files;
    }());
    // Thumbnail completions and other bursts are folded into one HistoryChanged signal.
    historyChangedTimer_ = new QTimer(this);
    historyChangedTimer_->setSingleShot(true);
    historyChangedTimer_->setInterval(100);
    connect(historyChangedTimer_, &QTimer::timeout, this, &App::historyChanged);
    tray_->setHistoryProvider([this] { return historyEntries(); });

    connect(hooks_, &HookRunner::hookFailed, this, [this](const HookResult &res) {
        QString detail = res.timedOut ? tr("Hook timed out after %1 ms.").arg(res.elapsedMs)
                                      : (!res.errorString.isEmpty() ? res.errorString
                                                                    : tr("Hook failed with exit code %1.").arg(res.exitCode));
        const QString errTrim = res.stdErr.trimmed();
        if (!errTrim.isEmpty()) {
            const QString firstLine = errTrim.section(QLatin1Char('\n'), 0, 0).trimmed();
            if (!firstLine.isEmpty()) {
                detail += QStringLiteral("\n") + firstLine.left(180);
            }
        }
        notify(QStringLiteral("hookFailed"),
               res.targetName.isEmpty() ? tr("Post-capture hook failed")
                                        : tr("Post-capture hook failed (%1)").arg(res.targetName),
               {detail});
    });

    connect(intervals_, &IntervalController::changed, this, [this] {
        tray_->setActiveIntervals(intervals_->activeIntervals());
        Q_EMIT intervalsChanged();
        Q_EMIT targetsChanged();
    });
    connect(intervals_, &IntervalController::captureSaved, this, [this](const QString &targetId, const TriggerResult &result) {
        recordCapture(result.writtenPaths, result.displayName, targetId, QStringLiteral("interval"));
        const QString fmt = result.outputInfos.isEmpty() ? formatConfigKey(store_.effectiveExportOptions(targetId).format)
                                                         : result.outputInfos.first().format;
        const QString groupId = store_.group(targetId) ? targetId : QString();
        runPostCaptureHook(targetId, result.displayName, result.writtenPaths, result.timestamp, fmt,
                           QStringLiteral("interval"), true, groupId);
    });
    connect(intervals_, &IntervalController::intervalFinished, this,
            [this](const QString &, const QString &displayName, int captureCount) {
                notify(QStringLiteral("intervalStopped"), tr("Interval capture finished"),
                       {tr("Stopped interval for %1 after %n capture(s).", nullptr, captureCount).arg(displayName)});
            });
    connect(intervals_, &IntervalController::intervalStoppedWithError, this,
            [this](const QString &, const QString &, const QString &reason) {
                notifyError(tr("Interval capture stopped"), {reason});
            });

    const auto syncCountdownUi = [this] {
        tray_->setCountdownState(countdown_->isActive(), countdown_->remainingSeconds());
        Q_EMIT targetsChanged();
    };
    connect(countdown_, &CountdownController::started, this, syncCountdownUi);
    connect(countdown_, &CountdownController::tick, this, [this](int secs) {
        tray_->setCountdownState(countdown_->isActive(), secs);
    });
    connect(countdown_, &CountdownController::completed, this, syncCountdownUi);
    connect(countdown_, &CountdownController::cancelled, this, syncCountdownUi);

    connect(shortcuts_, &ShortcutManager::triggered, this, &App::trigger);
    connect(engine_, &TriggerEngine::finished, this, &App::onTriggerFinished);
    connect(tray_, &TrayController::triggerRequested, this, &App::trigger);
    connect(tray_, &TrayController::delayChangedRequested, this, &App::setCaptureDelay);
    connect(tray_, &TrayController::cancelCountdownRequested, this, &App::cancelCountdown);
    connect(tray_, &TrayController::toggleIntervalRequested, this, [this](const QString &id) {
        toggleInterval(id);
    });
    connect(tray_, &TrayController::stopIntervalRequested, this, [this](const QString &id) {
        stopInterval(id);
    });
    connect(tray_, &TrayController::stopAllIntervalsRequested, this, &App::stopAllIntervals);
    connect(tray_, &TrayController::openLastCaptureRequested, this, &App::openLastCapture);
    connect(tray_, &TrayController::editLastCaptureRequested, this, &App::editLastCapture);
    connect(tray_, &TrayController::openFolderRequested, this, &App::openCaptureFolder);
    connect(tray_, &TrayController::configureRequested, this, [this] { showSettings(); });
    connect(tray_, &TrayController::addAreaRequested, this, &App::addArea);
    connect(tray_, &TrayController::pickColorRequested, this, &App::startColorPicker);
    connect(tray_, &TrayController::renameMonitorRequested, this, &App::renameMonitorWithDialog);
    connect(tray_, &TrayController::historyActionRequested, this,
            [this](const QString &action, const QString &entryId, const QString &path) {
                const QString error = historyAction(action, entryId, path);
                if (!error.isEmpty()) {
                    notifyError(tr("Recent capture"), {error});
                }
            });
    connect(tray_, &TrayController::clearHistoryRequested, this, &App::clearHistory);
    connect(tray_, &TrayController::pinsShownRequested, pins_, &PinManager::setPinsShown);
    connect(tray_, &TrayController::removeAllPinsRequested, pins_, &PinManager::removeAllWithConfirmation);

    // Screen recording (#30). The recorder touches neither KWin nor PipeWire before the first recording.
    recorder_ = new ScreenRecorder(this);
    recordingTicker_ = new QTimer(this);
    recordingTicker_->setInterval(1000);
    connect(recordingTicker_, &QTimer::timeout, this, &App::syncRecordingUi);
    connect(recorder_, &ScreenRecorder::stateChanged, this, [this] {
        if (recorder_->isBusy()) {
            recordingTicker_->start();
        } else {
            recordingTicker_->stop();
        }
        syncRecordingUi();
    });
    connect(recorder_, &ScreenRecorder::finished, this,
            [this](const QString &path, const RecordingTarget &target, qint64) {
                const QDateTime now = QDateTime::currentDateTime();
                recordCapture({path}, target.displayName, target.targetId, QStringLiteral("record"));
                notifySaved(tr("Recording saved"), {path}, {});
                runPostCaptureHook(target.targetId, target.displayName, {path}, now,
                                   recordingFormatKey(store_.recordingFormat), QStringLiteral("record"));
            });
    connect(recorder_, &ScreenRecorder::failed, this,
            [this](const QString &error) { notifyError(tr("Recording failed"), {error}); });
    connect(tray_, &TrayController::stopRecordingRequested, this, &App::stopRecording);
    connect(tray_, &TrayController::quitRequested, this, &App::quit);
    connect(tray_, &TrayController::pauseRecordingRequested, this, &App::setRecordingPaused);

    // New or removed monitors change the built-in targets.
    const auto layoutChanged = [this] {
        syncShortcuts();
        Q_EMIT targetsChanged();
    };
    connect(qGuiApp, &QGuiApplication::screenAdded, this, layoutChanged);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, layoutChanged);

    // The Plasma widget replaces the separate tray icon while it is loaded.
    control_ = new ControlService(this);
    connect(control_, &ControlService::widgetActiveChanged, tray_, &TrayController::setStandby);
    // Pins report moves through /Control, so restore them once it is registered.
    pins_->restore();

    syncShortcuts();
    migrateLegacyIds();
    applyAutostart();
    // First start of a version with the Print key: take it over once. Later the user decides,
    // here or in System Settings.
    if (store_.usePrintKey && !PrintKey::isDecided(*config_)) {
        claimPrintKey();
    }
}

void App::claimPrintKey()
{
    PrintKey::claim(*config_);
    shortcuts_->setShortcut(PrintKey::actionId(), QKeySequence(Qt::Key_Print));
}

void App::setUsePrintKey(bool on)
{
    store_.usePrintKey = on;
    if (on) {
        claimPrintKey();
    } else {
        // Clear our own binding first, so the previous owner gets Print without a conflict.
        shortcuts_->setShortcut(PrintKey::actionId(), {});
        PrintKey::release(*config_, false);
    }
    storeChanged();
}

void App::migrateLegacyIds()
{
    if (store_.migratedIds.isEmpty()) {
        return;
    }
    // Areas used to be called regions. Move their global shortcuts to the new action ids, drop the
    // old actions from KGlobalAccel and write the configuration in the new format.
    for (auto it = store_.migratedIds.cbegin(); it != store_.migratedIds.cend(); ++it) {
        const QList<QKeySequence> old = KGlobalAccel::self()->globalShortcut(QCoreApplication::applicationName(), it.key());
        if (!old.isEmpty() && shortcuts_->shortcut(it.value()).isEmpty()) {
            shortcuts_->setShortcut(it.value(), old.first());
        }
        QDBusMessage unregister = QDBusMessage::createMethodCall(QStringLiteral("org.kde.kglobalaccel"),
                                                                 QStringLiteral("/kglobalaccel"),
                                                                 QStringLiteral("org.kde.KGlobalAccel"),
                                                                 QStringLiteral("unregister"));
        unregister.setArguments({QCoreApplication::applicationName(), it.key()});
        QDBusConnection::sessionBus().call(unregister);
    }
    store_.migratedIds.clear();
    store_.save(*config_);
}

App::~App()
{
    delete countdownBanner_;
}

TargetStore &App::store()
{
    return store_;
}

ShortcutManager *App::shortcuts() const
{
    return shortcuts_;
}

void App::storeChanged(const QStringList &deletedIds)
{
    store_.save(*config_);
    if (history_->setLimit(store_.historyLimit)) {
        historyModified(true);
    }
    syncShortcuts(deletedIds);
    applyAutostart();
    Q_EMIT targetsChanged();
}

void App::saveSettings()
{
    store_.save(*config_);
}

QString App::targetLabel(const TargetStore &store, const QString &id)
{
    if (isIntervalShortcutId(id)) {
        return tr("Interval: %1").arg(targetLabel(store, targetIdFromIntervalShortcutId(id)));
    }
    if (id == allMonitorsTargetId()) {
        return tr("All monitors");
    }
    if (id == colorPickerTargetId()) {
        return tr("Pick screen color");
    }
    if (id == recordToggleActionId()) {
        return tr("Start or stop screen recording");
    }
    if (id == recordPauseActionId()) {
        return tr("Pause or resume screen recording");
    }
    if (isScreenTargetId(id)) {
        return monitorName(store, connectorFromScreenTargetId(id));
    }
    if (const Area *area = store.area(id)) {
        return area->name;
    }
    if (const Group *group = store.group(id)) {
        return group->name;
    }
    return id;
}

void App::syncShortcuts(const QStringList &deletedIds)
{
    QList<ShortcutManager::Entry> entries;
    entries.append({PrintKey::actionId(), tr("Save the screen and select (Print key)")});
    for (const Group &group : std::as_const(store_.groups)) {
        entries.append({group.id, tr("Capture group %1").arg(group.name)});
        entries.append({intervalShortcutId(group.id), tr("Toggle interval for group %1").arg(group.name)});
    }
    for (const Area &area : std::as_const(store_.areas)) {
        entries.append({area.id, tr("Capture area %1").arg(area.name)});
        entries.append({intervalShortcutId(area.id), tr("Toggle interval for area %1").arg(area.name)});
    }
    for (const ScreenInfo &screen : currentScreens()) {
        const QString screenId = screenTargetId(screen.connector);
        const QString monName = store_.monitorName(screen);
        entries.append({screenId, tr("Capture monitor %1").arg(monName)});
        entries.append({intervalShortcutId(screenId), tr("Toggle interval for monitor %1").arg(monName)});
    }
    entries.append({allMonitorsTargetId(), tr("Capture all monitors")});
    entries.append({intervalShortcutId(allMonitorsTargetId()), tr("Toggle interval for all monitors")});
    entries.append({colorPickerTargetId(), tr("Pick screen color")});
    entries.append({pinLastCaptureActionId(), tr("Pin last capture to the screen")});
    entries.append({pinsToggleActionId(), tr("Show or hide pinned screenshots")});
    entries.append({ocrSelectionActionId(), tr("Copy text from a screen selection")});
    entries.append({recordToggleActionId(), tr("Start or stop screen recording")});
    entries.append({recordPauseActionId(), tr("Pause or resume screen recording")});
    shortcuts_->sync(entries, deletedIds);
}

QList<MenuTarget> App::menuTargets() const
{
    const QList<ScreenInfo> screens = currentScreens();
    const auto connected = [&](const QString &connector) {
        return std::any_of(screens.cbegin(), screens.cend(),
                           [&](const ScreenInfo &s) { return s.connector == connector; });
    };
    const auto makeTarget = [&](MenuTarget::Section section, const QString &id, const QString &name,
                                const QString &detail, bool available) {
        MenuTarget entry;
        entry.section = section;
        entry.id = id;
        entry.name = name;
        entry.detail = detail;
        entry.shortcut = shortcuts_->shortcut(id);
        entry.available = available;
        entry.intervalSeconds = store_.targetIntervalConfig(id).seconds;
        entry.intervalActive = intervals_ && intervals_->isActive(id);
        return entry;
    };

    QList<MenuTarget> targets;
    for (const Group &group : store_.groups) {
        targets.append(makeTarget(MenuTarget::Section::Group, group.id, group.name, {}, !group.targetIds.isEmpty()));
    }
    for (const Area &area : store_.areas) {
        if (area.isCrossMonitor()) {
            int presentCount = 0;
            QStringList missingConnectors;
            for (const AreaMonitorBinding &binding : area.monitors) {
                if (connected(binding.connector)) {
                    ++presentCount;
                } else {
                    missingConnectors.append(binding.connector);
                }
            }
            const bool available = presentCount > 0;
            QString detail;
            if (!available) {
                detail = tr("Monitors not connected");
            } else if (!missingConnectors.isEmpty()) {
                detail = tr("Partial: %1 not connected").arg(missingConnectors.join(QStringLiteral(", ")));
            }
            targets.append(makeTarget(MenuTarget::Section::Area, area.id, area.name, detail, available));
        } else {
            const bool present = connected(area.connector);
            targets.append(makeTarget(MenuTarget::Section::Area, area.id, area.name,
                                      present ? QString() : tr("%1 not connected").arg(area.connector), present));
        }
    }
    for (const ScreenInfo &screen : screens) {
        const QString id = screenTargetId(screen.connector);
        targets.append(makeTarget(MenuTarget::Section::Monitor, id, store_.monitorName(screen), {}, true));
    }
    targets.append(
        makeTarget(MenuTarget::Section::Monitor, allMonitorsTargetId(), tr("All monitors"), {}, !screens.isEmpty()));
    return targets;
}

void App::startPrint()
{
    if (countdown_->isActive()) {
        cancelCountdown();
        return;
    }
    if (auto *session = session_.data()) {
        session->printPressed();
        return;
    }
    if (store_.captureDelaySeconds > 0) {
        runWithDelay(store_.captureDelaySeconds, tr("Screen"), [this] { startPrintNow(); });
        return;
    }
    startPrintNow();
}

void App::startPrintNow(std::optional<bool> forcedFrozen, bool forceOverlay)
{
    if (auto *session = session_.data()) {
        session->printPressed();
        return;
    }
    LatencyTrace::begin(int(QGuiApplication::screens().size()));
    auto *session = new CaptureSession(this, CaptureSession::Purpose::Print);
    session->setForcedFrozen(forcedFrozen);
    session->setForceShowOverlay(forceOverlay);
    session_ = session;
    connect(session, &CaptureSession::finished, session, &QObject::deleteLater);
    startWithPinsHidden(session, [session] { session->start(); }, true);
}

void App::startOcrSelection()
{
    if (session_ || selector_ || colorPicker_) {
        return;
    }
    auto *session = new CaptureSession(this, CaptureSession::Purpose::Print);
    session->setTextMode(true);
    session->setForcedFrozen(true);
    session->setForceShowOverlay(true);
    session_ = session;
    connect(session, &CaptureSession::finished, session, &QObject::deleteLater);
    startWithPinsHidden(session, [session] { session->start(); });
}

TesseractOcrEngine &App::ocrEngine()
{
    return *ocr_;
}

QStringList App::ocrLanguages() const
{
    return effectiveOcrLanguages(store_.ocrLanguages, ocr_->languages());
}

void App::copyTextFromImages(const QList<QImage> &images)
{
    if (images.isEmpty()) {
        return;
    }
    if (!ocr_->available()) {
        notifyError(tr("Text recognition is not available"),
                    {tr("Install tesseract and a language package, for example tesseract-data-eng, then try again.")});
        return;
    }
    const QStringList langs = ocrLanguages();
    const std::shared_ptr<const OcrEngine> engine = ocr_;
    auto *watcher = new QFutureWatcher<OcrResult>(this);
    connect(watcher, &QFutureWatcher<OcrResult>::finished, this, [this, watcher] {
        const OcrResult result = watcher->result();
        watcher->deleteLater();
        if (!result.ok) {
            notifyError(tr("Text recognition failed"), {result.error});
            return;
        }
        if (result.text.isEmpty()) {
            // Keep whatever is on the clipboard instead of replacing it with nothing (Q8).
            notify(QStringLiteral("textCopied"), tr("No text recognized"),
                   {tr("The clipboard was left unchanged.")});
            return;
        }
        QGuiApplication::clipboard()->setText(result.text);
        notify(QStringLiteral("textCopied"), tr("Text copied"), {ocrPreview(result.text)});
    });
    watcher->setFuture(QtConcurrent::run([engine, images, langs] {
        QStringList texts;
        for (const QImage &image : images) {
            const OcrResult part = engine->recognize(image, langs);
            if (!part.ok) {
                return part;
            }
            texts.append(part.text);
        }
        OcrResult joined;
        joined.ok = true;
        joined.text = joinOcrTexts(texts);
        return joined;
    }));
}

void App::startWithPinsHidden(QObject *owner, std::function<void()> start, bool recordLatency)
{
    // Pins stay transparent for as long as the overlay or picker exists (#28).
    connect(owner, &QObject::destroyed, this, [this] { pins_->endCapture(); });
    const qint64 sinceUs = LatencyTrace::nowUs();
    pins_->beginCapture(owner, [start = std::move(start), sinceUs, recordLatency] {
        if (recordLatency) {
            LatencyTrace::setPinWait(LatencyTrace::nowUs() - sinceUs);
        }
        start();
    });
}

bool App::isCountdownActive() const
{
    return countdown_->isActive();
}

int App::captureDelaySeconds() const
{
    return store_.captureDelaySeconds;
}

bool App::keepDelayActive() const
{
    return store_.keepDelayActive;
}

void App::setCaptureDelay(int seconds, bool keepActive)
{
    store_.captureDelaySeconds = std::clamp(seconds, 0, 60);
    store_.keepDelayActive = keepActive && store_.captureDelaySeconds > 0;
    saveSettings();
    tray_->setDelayState(store_.captureDelaySeconds, store_.keepDelayActive);
    Q_EMIT targetsChanged();
}

void App::cancelCountdown()
{
    countdown_->cancel();
}

QString App::startInterval(const QString &id, const std::optional<IntervalConfig> &overrideConfig)
{
    const IntervalConfig cfg = overrideConfig ? *overrideConfig : store_.targetIntervalConfig(id);
    const QString error = intervals_->start(id, targetLabel(store_, id), cfg);
    if (!error.isEmpty()) {
        notifyError(tr("Could not start interval"), {error});
    }
    return error;
}

bool App::stopInterval(const QString &id)
{
    return intervals_->stop(id);
}

void App::stopAllIntervals()
{
    intervals_->stopAll();
}

bool App::toggleInterval(const QString &id)
{
    QString error;
    const bool nowActive = intervals_->toggle(id, targetLabel(store_, id), store_.targetIntervalConfig(id), &error);
    if (!error.isEmpty()) {
        notifyError(tr("Could not start interval"), {error});
    }
    return nowActive;
}

bool App::isIntervalActive(const QString &id) const
{
    return intervals_->isActive(id);
}

QList<ActiveIntervalStatus> App::activeIntervals() const
{
    return intervals_->activeIntervals();
}

void App::runWithDelay(int seconds, const QString &label, std::function<void()> onReady,
                       std::function<void()> onCancelled)
{
    if (seconds <= 0) {
        if (onReady) {
            onReady();
        }
        return;
    }
    countdown_->start(
        seconds, label,
        [this, ready = std::move(onReady)] {
            if (store_.consumeOneShotDelay()) {
                saveSettings();
                tray_->setDelayState(store_.captureDelaySeconds, store_.keepDelayActive);
                Q_EMIT targetsChanged();
            }
            if (ready) {
                ready();
            }
        },
        std::move(onCancelled));
}

void App::addArea()
{
    if (session_ || selector_ || colorPicker_) {
        return;
    }
    auto *session = new CaptureSession(this, CaptureSession::Purpose::DefineArea);
    session_ = session;
    connect(session, &CaptureSession::finished, session, &QObject::deleteLater);
    startWithPinsHidden(session, [session] { session->start(); });
}

void App::startColorPicker()
{
    if (session_ || selector_ || colorPicker_) {
        return;
    }
    auto *picker = new ColorPickerSession(this);
    colorPicker_ = picker;
    connect(picker, &ColorPickerSession::picked, this, [this](const QColor &, const QString &formatted) {
        notify(QStringLiteral("colorPicked"), tr("Color copied"), {tr("%1 copied to clipboard").arg(formatted)});
    });
    connect(picker, &ColorPickerSession::finished, picker, &QObject::deleteLater);
    startWithPinsHidden(picker, [picker] { picker->start(); });
}

void App::recordCapture(const QStringList &paths, const QString &displayName, const QString &targetId,
                        const QString &triggerKind)
{
    if (paths.isEmpty()) {
        return;
    }
    HistoryEntry entry;
    entry.targetId = targetId;
    entry.targetName = displayName;
    entry.triggerKind = triggerKind;
    entry.files = paths;
    if (history_->record(entry)) {
        historyModified(true);
    }
    lastPaths_ = paths;
    lastCaptureTargetId_ = targetId;
    lastCaptureDisplayName_ = displayName;
    lastCaptureTimestamp_ = QDateTime::currentDateTime();
    Q_EMIT captured(true);
    // Every frame is in memory by now, so the blink cannot end up in a capture (A2).
    tray_->blink();
    tray_->setLastCaptureAvailable(true);
    tray_->setStatusText(tr("Last capture: %1 at %2")
                             .arg(displayName, QLocale().toString(lastCaptureTimestamp_.time(),
                                                                  QLocale::ShortFormat)));
}

void App::forgetCapturePath(const QString &path)
{
    if (history_->removeFile(path)) {
        historyModified(true);
    }
    if (path.isEmpty() || lastPaths_.removeAll(path) == 0) {
        return;
    }
    const bool hasAny = !lastPaths_.isEmpty();
    tray_->setLastCaptureAvailable(hasAny);
    if (!hasAny) {
        tray_->setStatusText(tr("Ready"));
    }
    Q_EMIT targetsChanged();
}

void App::notifySaved(const QString &title, const QStringList &paths, const QStringList &errors)
{
    if (paths.isEmpty()) {
        if (errors.isEmpty() && !title.isEmpty()) {
            notify(QStringLiteral("captureSaved"), title, {});
        } else {
            notifyError(title.isEmpty() ? tr("Capture failed") : title, errors);
        }
        return;
    }
    QStringList lines;
    for (const QString &path : paths.mid(0, 3)) {
        lines.append(QFileInfo(path).fileName());
    }
    if (paths.size() > 3) {
        lines.append(tr("and %n more", nullptr, int(paths.size() - 3)));
    }
    lines += errors;
    const QString heading = !title.isEmpty() ? title
                            : paths.size() == 1 ? tr("Screenshot saved")
                                                : tr("%n screenshots saved", nullptr, int(paths.size()));
    static const bool haveEvents = !QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                                           QStringLiteral("knotifications6/glimply.notifyrc"))
                                        .isEmpty();
    auto *notification = new KNotification(haveEvents ? QStringLiteral("captureSaved") : QStringLiteral("notification"));
    if (haveEvents) {
        notification->setComponentName(QStringLiteral("io.github.m00sfett.glimply"));
    }
    notification->setTitle(heading);
    notification->setText(lines.join(QLatin1Char('\n')));
    QList<QUrl> urls;
    for (const QString &path : paths) {
        urls.append(QUrl::fromLocalFile(path));
    }
    notification->setUrls(urls);
    const QStringList files = paths;
    KNotificationAction *open = notification->addAction(paths.size() == 1 ? tr("Open") : tr("Show Files"));
    connect(open, &KNotificationAction::activated, this, [this, files] {
        lastPaths_ = files;
        openLastCapture();
    });
    if (!files.isEmpty()) {
        KNotificationAction *edit = notification->addAction(tr("Edit"));
        const QString targetFile = files.first();
        connect(edit, &KNotificationAction::activated, this, [this, targetFile] {
            openEditorForFile(targetFile);
        });
    }
    KNotificationAction *folder = notification->addAction(tr("Show in Folder"));
    connect(folder, &KNotificationAction::activated, this, [files] {
        QStringList uris;
        for (const QString &path : files) {
            uris.append(QUrl::fromLocalFile(path).toString());
        }
        QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.FileManager1"),
                                                           QStringLiteral("/org/freedesktop/FileManager1"),
                                                           QStringLiteral("org.freedesktop.FileManager1"),
                                                           QStringLiteral("ShowItems"));
        call.setArguments({uris, QString()});
        QDBusConnection::sessionBus().asyncCall(call);
    });
    notification->sendEvent();
}

void App::trigger(const QString &id)
{
    if (countdown_->isActive()) {
        cancelCountdown();
        return;
    }
    if (isIntervalShortcutId(id)) {
        toggleInterval(targetIdFromIntervalShortcutId(id));
        return;
    }
    if (id == PrintKey::actionId()) {
        startPrint();
        return;
    }
    if (id == colorPickerTargetId()) {
        startColorPicker();
        return;
    }
    if (id == pinLastCaptureActionId()) {
        pinLastCapture();
        return;
    }
    if (id == pinsToggleActionId()) {
        pins_->setPinsShown(!pins_->pinsShown());
        return;
    }
    if (id == ocrSelectionActionId()) {
        startOcrSelection();
        return;
    }
    if (id == recordToggleActionId()) {
        toggleRecording();
        return;
    }
    if (id == recordPauseActionId()) {
        setRecordingPaused(recorder_->state() == ScreenRecorder::State::Recording);
        return;
    }
    const int delay = store_.effectiveDelaySeconds(id);
    if (delay > 0) {
        runWithDelay(delay, targetLabel(store_, id), [this, id] { executeTriggerNow(id); });
        return;
    }
    executeTriggerNow(id);
}

void App::executeTriggerNow(const QString &id)
{
    TriggerPlan plan = planTrigger(store_, id, currentScreens());
    if (plan.outputs.isEmpty()) {
        if (!plan.partialNotifications.isEmpty()) {
            notifyPartialCapture(tr("Area not captured"), plan.partialNotifications);
        }
        if (!plan.errors.isEmpty()) {
            notifyError(tr("Nothing captured"), plan.errors);
        }
        return;
    }
    pins_->beginCapture(this, [this, plan = std::move(plan)]() mutable {
        engine_->run(std::move(plan), store_.effectiveCaptureDirectory(), store_.effectiveFilenameTemplate());
    });
}

void App::onTriggerFinished(const TriggerResult &result)
{
    pins_->endCapture();
    if (result.writtenPaths.isEmpty()) {
        Q_EMIT captured(false);
    } else {
        recordCapture(result.writtenPaths, result.displayName, result.triggerId);
        const QString fmt =
            result.outputInfos.isEmpty() ? formatConfigKey(store_.effectiveExportOptions(result.triggerId).format)
                                         : result.outputInfos.first().format;
        const QString groupId = store_.group(result.triggerId) ? result.triggerId : QString();
        runPostCaptureHook(result.triggerId, result.displayName, result.writtenPaths, result.timestamp, fmt,
                           QStringLiteral("trigger"), false, groupId);
    }
    if (!result.errors.isEmpty()) {
        notifyError(result.writtenPaths.isEmpty() ? tr("Capture failed") : tr("Capture incomplete"), result.errors);
    }
    if (!result.warnings.isEmpty()) {
        notify(QStringLiteral("monitorChanged"), tr("Check area monitor"), result.warnings);
    }
    if (!result.partialNotifications.isEmpty()) {
        notifyPartialCapture(result.writtenPaths.isEmpty() ? tr("Area not captured") : tr("Partial capture"),
                             result.partialNotifications);
    }
}

void App::notifyPartialCapture(const QString &title, const QStringList &messages)
{
    notify(QStringLiteral("partialCapture"), title, messages);
}

void App::notify(const QString &eventId, const QString &title, const QStringList &messages)
{
    const QString text = messages.join(QLatin1Char('\n'));
    // Installed builds ship glimply.notifyrc, so notifications carry glimply's name and can be
    // configured in System Settings. Uninstalled development builds fall back to the system events.
    static const bool haveEvents = !QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                                           QStringLiteral("knotifications6/glimply.notifyrc"))
                                        .isEmpty();
    if (haveEvents) {
        KNotification::event(eventId, title, text, QStringLiteral("io.github.m00sfett.glimply"));
    } else {
        KNotification::StandardEvent fallback = KNotification::Error;
        if (eventId == QLatin1String("monitorChanged")) {
            fallback = KNotification::Warning;
        } else if (eventId == QLatin1String("partialCapture") || eventId == QLatin1String("captureSaved")
                   || eventId == QLatin1String("colorPicked") || eventId == QLatin1String("intervalStopped")
                   || eventId == QLatin1String("mcpCapture") || eventId == QLatin1String("historyAction")
                   || eventId == QLatin1String("textCopied")) {
            fallback = KNotification::Notification;
        }
        KNotification::event(fallback, title, text);
    }
}

void App::notifyError(const QString &title, const QStringList &messages)
{
    notify(QStringLiteral("captureFailed"), title, messages);
}

void App::openLastCapture()
{
    const QStringList paths = lastPaths_;
    if (paths.isEmpty()) {
        return;
    }
    if (paths.size() == 1) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(paths.first()));
        return;
    }
    showInFolder(paths);
}

void App::showInFolder(const QStringList &paths)
{
    QStringList uris;
    for (const QString &path : paths) {
        uris.append(QUrl::fromLocalFile(path).toString());
    }
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.FileManager1"),
                                                       QStringLiteral("/org/freedesktop/FileManager1"),
                                                       QStringLiteral("org.freedesktop.FileManager1"),
                                                       QStringLiteral("ShowItems"));
    call.setArguments({uris, QString()});
    QDBusConnection::sessionBus().asyncCall(call);
}

void App::pruneHistory()
{
    if (history_->prune()) {
        historyModified(true);
    }
}

const QList<HistoryEntry> &App::historyEntries()
{
    pruneHistory();
    return history_->entries();
}

QJsonArray App::historyJson()
{
    pruneHistory();
    requestThumbnails();
    return history_->toJson([this](const QString &path) { return thumbnails_->existingThumbnail(path); });
}

QString App::historyAction(const QString &action, const QString &entryId, const QString &path)
{
    const std::optional<HistoryEntry> entry = history_->entry(entryId);
    if (!entry) {
        return tr("This capture is no longer in the history.");
    }
    // Only files recorded in this entry are accepted, so the D-Bus API cannot reach other files.
    if (!path.isEmpty() && !entry->files.contains(path)) {
        return tr("This file does not belong to the capture.");
    }
    const QStringList files = path.isEmpty() ? entry->files : QStringList{path};
    QStringList existing;
    for (const QString &file : files) {
        if (QFileInfo::exists(file)) {
            existing.append(file);
        }
    }
    if (existing.isEmpty()) {
        pruneHistory();
        return tr("The file was moved or deleted.");
    }

    if (action == QLatin1String("open")) {
        if (existing.size() == 1) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(existing.first()));
        } else {
            showInFolder(existing);
        }
        return {};
    }
    if (action == QLatin1String("edit")) {
        openEditorForFile(existing.first());
        return {};
    }
    if (action == QLatin1String("folder")) {
        showInFolder(existing);
        return {};
    }
    if (action == QLatin1String("pin")) {
        QImageReader reader(existing.first());
        reader.setAutoTransform(true);
        const QImage image = reader.read();
        if (image.isNull()) {
            return tr("Could not read %1: %2").arg(QFileInfo(existing.first()).fileName(), reader.errorString());
        }
        pins_->pinImage(image);
        return {};
    }
    if (action == QLatin1String("copy") && isRecordingFile(existing.first())) {
        // A recording goes to the clipboard as a file, the way file managers copy it.
        auto *mime = new QMimeData;
        mime->setUrls({QUrl::fromLocalFile(existing.first())});
        QGuiApplication::clipboard()->setMimeData(mime);
        notify(QStringLiteral("historyAction"), tr("Recording copied"),
               {tr("%1 copied to the clipboard").arg(QFileInfo(existing.first()).fileName())});
        return {};
    }
    if (action == QLatin1String("copy")) {
        QImageReader reader(existing.first());
        reader.setAutoTransform(true);
        const QImage image = reader.read();
        if (image.isNull()) {
            return tr("Could not read %1: %2").arg(QFileInfo(existing.first()).fileName(), reader.errorString());
        }
        QGuiApplication::clipboard()->setImage(image);
        notify(QStringLiteral("historyAction"), tr("Image copied"),
               {tr("%1 copied to the clipboard").arg(QFileInfo(existing.first()).fileName())});
        return {};
    }
    if (action == QLatin1String("trash")) {
        QStringList trashed;
        QStringList failed;
        for (const QString &file : std::as_const(existing)) {
            if (QFile::moveToTrash(file)) {
                trashed.append(QFileInfo(file).fileName());
                forgetCapturePath(file);
            } else {
                failed.append(QFileInfo(file).fileName());
            }
        }
        if (!trashed.isEmpty()) {
            notify(QStringLiteral("historyAction"), tr("Moved to trash"), {trashed.join(QStringLiteral(", "))});
        }
        if (!failed.isEmpty()) {
            return tr("Could not move to the trash: %1").arg(failed.join(QStringLiteral(", ")));
        }
        return {};
    }
    return tr("Unknown action \"%1\".").arg(action);
}

void App::pinImage(const QImage &image, const QRect &logicalRect)
{
    pins_->pinImage(image, logicalRect);
}

void App::pinLastCapture()
{
    for (const QString &path : std::as_const(lastPaths_)) {
        if (isRecordingFile(path)) {
            continue;
        }
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QImage image = reader.read();
        if (!image.isNull()) {
            pins_->pinImage(image);
            return;
        }
    }
    notifyError(tr("Nothing to pin"), {tr("There is no recent capture file to pin.")});
}

void App::pinMoved(const QString &id, int x, int y)
{
    pins_->pinMoved(id, x, y);
}

void App::savePinImageAs(const QImage &image)
{
    const ExportOptions defaults = store_.exportOptions;
    const QString suggested = QDir(store_.effectiveCaptureDirectory())
                                  .filePath(filenameFor(store_.effectiveFilenameTemplate(), QStringLiteral("pin"),
                                                        QDateTime::currentDateTime(), defaults.format));
    const QString path = QFileDialog::getSaveFileName(nullptr, tr("Save Pinned Screenshot"), suggested);
    if (path.isEmpty()) {
        return;
    }
    ExportOptions options = defaults;
    if (const std::optional<ImageFormat> format = imageFormatFromPath(path); format && isImageFormatSupported(*format)) {
        options.format = *format;
    }
    QString error;
    if (!writeImageToFile(path, image, options, &error)) {
        notifyError(tr("Could not save the pinned screenshot"), {error});
        return;
    }
    recordCapture({path}, tr("Pin"), QString(), QStringLiteral("editor"));
}

bool App::restart()
{
    // A second instance would only forward to this one (KDBusService::Unique), so a small
    // shell waits until this process is gone and then starts the same executable again.
    // PID and path are passed as arguments, never pasted into the script.
    const QString script = QStringLiteral("while kill -0 \"$1\" 2>/dev/null; do sleep 0.1; done; exec \"$2\"");
    const bool started = QProcess::startDetached(
        QStringLiteral("/bin/sh"), {QStringLiteral("-c"), script, QStringLiteral("glimply-restart"),
                                    QString::number(QCoreApplication::applicationPid()),
                                    QCoreApplication::applicationFilePath()});
    if (started) {
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    }
    return started;
}

void App::clearHistory()
{
    if (history_->entries().isEmpty()) {
        return;
    }
    history_->clear();
    historyModified(true);
}

void App::historyModified(bool sweepThumbnails)
{
    QString error;
    if (!history_->save(&error)) {
        qWarning("glimply: could not save the capture history: %s", qPrintable(error));
    }
    if (sweepThumbnails) {
        QStringList files;
        for (const HistoryEntry &entry : history_->entries()) {
            files += entry.files;
        }
        thumbnails_->sweep(files);
    }
    requestThumbnails();
    historyChangedTimer_->start();
}

void App::requestThumbnails()
{
    for (const HistoryEntry &entry : history_->entries()) {
        for (const QString &file : entry.files) {
            if (thumbnailsPending_.contains(file) || !thumbnails_->existingThumbnail(file).isEmpty()) {
                continue;
            }
            thumbnailsPending_.insert(file);
            auto *watcher = new QFutureWatcher<QString>(this);
            connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, file] {
                const bool made = !watcher->result().isEmpty();
                watcher->deleteLater();
                // A file that cannot be decoded stays marked, so it is not retried on every query.
                if (made) {
                    thumbnailsPending_.remove(file);
                    historyChangedTimer_->start();
                }
            });
            const ThumbnailCache cache(thumbnails_->directory());
            watcher->setFuture(QtConcurrent::run([cache, file] { return cache.generate(file); }));
        }
    }
}

void App::editLastCapture()
{
    if (lastPaths_.isEmpty()) {
        return;
    }
    for (const QString &path : std::as_const(lastPaths_)) {
        if (!path.isEmpty() && QFileInfo::exists(path)) {
            openEditorForFile(path);
            return;
        }
    }
    notifyError(tr("No capture to edit"), {tr("The most recent screenshot file is missing or was moved.")});
}

void App::openEditorForFile(const QString &path)
{
    const QString absPath = QFileInfo(path).absoluteFilePath();
    if (absPath.isEmpty()) {
        return;
    }
    if (isRecordingFile(absPath)) {
        // Recordings have no editor here; the default player opens them.
        QDesktopServices::openUrl(QUrl::fromLocalFile(absPath));
        return;
    }
    editors_.removeIf([](const QPointer<QuickEditorWindow> &ptr) { return ptr.isNull(); });
    for (const QPointer<QuickEditorWindow> &existing : std::as_const(editors_)) {
        if (existing && existing->filePath() == absPath && existing->isValid()) {
            existing->show();
            existing->raise();
            existing->activateWindow();
            return;
        }
    }
    auto *window = new QuickEditorWindow(store_, absPath);
    if (!window->isValid()) {
        notifyError(tr("Could not open image"), {window->loadError()});
        delete window;
        return;
    }
    attachEditorWindow(window);
}

void App::openEditorFromCapture(const QImage &image, qreal scale, std::unique_ptr<AnnotationDocument> annotations,
                                const QString &baseToken)
{
    auto *window = new QuickEditorWindow(store_, image, scale, std::move(annotations), baseToken);
    if (!window->isValid()) {
        notifyError(tr("Could not open image"), {window->loadError()});
        delete window;
        return;
    }
    attachEditorWindow(window);
}

void App::attachEditorWindow(QuickEditorWindow *window)
{
    window->setAttribute(Qt::WA_DeleteOnClose);
    connect(window, &QuickEditorWindow::savedToFile, this,
            [this](const QString &savedPath, const QString &displayName) {
                recordCapture({savedPath}, displayName, QString(), QStringLiteral("editor"));
                const std::optional<ImageFormat> fmt = imageFormatFromPath(savedPath);
                runPostCaptureHook({}, displayName, {savedPath}, QDateTime::currentDateTime(),
                                   fmt ? formatConfigKey(*fmt) : QStringLiteral("png"), QStringLiteral("editor"));
            });
    connect(window, &QuickEditorWindow::settingsModified, this, &App::saveSettings);
    connect(window, &QuickEditorWindow::pinRequested, this, [this](const QImage &image) { pins_->pinImage(image); });
    connect(window, &QuickEditorWindow::copyTextRequested, this,
            [this](const QImage &image) { copyTextFromImages({image}); });
    editors_.append(window);
    window->show();
    window->raise();
    window->activateWindow();
}

void App::openCaptureFolder()
{
    const QString directory = store_.effectiveCaptureDirectory();
    QDir().mkpath(directory);
    QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
}

void App::showSettings(bool addArea, const QString &targetId)
{
    auto *window = qobject_cast<SettingsWindow *>(settings_.data());
    if (!window) {
        window = new SettingsWindow(this);
        settings_ = window;
    }
    if (addArea) {
        window->addArea();
        return;
    }
    if (!targetId.isEmpty()) {
        window->showTarget(targetId);
    }
    window->show();
    window->raise();
    window->activateWindow();
}

QString App::renameTarget(const QString &id, const QString &name)
{
    const QString trimmed = name.trimmed();
    if (isScreenTargetId(id)) {
        const QString connector = connectorFromScreenTargetId(id);
        const QList<ScreenInfo> screens = currentScreens();
        const auto screen = std::find_if(screens.cbegin(), screens.cend(),
                                         [&](const ScreenInfo &s) { return s.connector == connector; });
        if (screen == screens.cend()) {
            return tr("Monitor %1 is not connected.").arg(connector);
        }
        if (trimmed == store_.monitorName(*screen)) {
            return {};
        }
        if (!trimmed.isEmpty() && trimmed != connector) {
            if (sanitizeFilenameToken(trimmed).isEmpty()) {
                return tr("Enter a name that can be used in a file name.");
            }
            QStringList otherConnectors;
            for (const ScreenInfo &other : screens) {
                if (other.connector != connector) {
                    otherConnectors.append(other.connector);
                }
            }
            if (!store_.isNameAvailable(trimmed, TargetStore::monitorKey(*screen), otherConnectors)) {
                return tr("“%1” is already used by another target or group.").arg(trimmed);
            }
        }
        store_.setMonitorName(*screen, trimmed);
        storeChanged();
        return {};
    }
    Area *area = store_.area(id);
    Group *group = store_.group(id);
    if (!area && !group) {
        return tr("Only areas, groups and monitors can be renamed.");
    }
    if ((area && trimmed == area->name) || (group && trimmed == group->name)) {
        return {};
    }
    if (trimmed.isEmpty() || (area && sanitizeFilenameToken(trimmed).isEmpty())) {
        return tr("Enter a name that can be used in a file name.");
    }
    QStringList connectors;
    for (const ScreenInfo &screen : currentScreens()) {
        connectors.append(screen.connector);
    }
    if (!store_.isNameAvailable(trimmed, id, connectors)) {
        return tr("“%1” is already used by another target or group.").arg(trimmed);
    }
    if (area) {
        area->name = trimmed;
    } else {
        group->name = trimmed;
    }
    storeChanged();
    return {};
}

void App::renameMonitorWithDialog(const QString &id)
{
    const QString connector = connectorFromScreenTargetId(id);
    QString name = monitorName(store_, connector);
    QString error;
    for (;;) {
        QString label = tr("Name for monitor %1 (leave empty for %1):").arg(connector);
        if (!error.isEmpty()) {
            label = error + QStringLiteral("\n\n") + label;
        }
        bool ok = false;
        name = QInputDialog::getText(nullptr, tr("Rename Monitor"), label, QLineEdit::Normal,
                                     name == connector ? QString() : name, &ok);
        if (!ok) {
            return;
        }
        error = renameTarget(id, name);
        if (error.isEmpty()) {
            return;
        }
    }
}

void App::removeTarget(const QString &id)
{
    if (!store_.area(id) && !store_.group(id)) {
        return;
    }
    intervals_->stop(id);
    store_.remove(id);
    storeChanged({id, intervalShortcutId(id)});
}

bool App::hasLastCapture() const
{
    return !lastPaths_.isEmpty();
}

CliResolveResult App::resolveTarget(const QString &query) const
{
    const QString q = query.trimmed();
    if (q.isEmpty()) {
        return {false, {}, {}, {}, tr("Target query is empty.")};
    }

    const QList<ScreenInfo> screens = currentScreens();
    if (q.compare(allMonitorsTargetId(), Qt::CaseInsensitive) == 0) {
        if (screens.isEmpty()) {
            return {false, {}, {}, {}, tr("No monitor is connected.")};
        }
        return {true, allMonitorsTargetId(), QStringLiteral("all"), tr("All monitors"), {}};
    }

    if (const Area *area = store_.area(q)) {
        return {true, area->id, QStringLiteral("area"), area->name, {}};
    }
    if (const Group *group = store_.group(q)) {
        return {true, group->id, QStringLiteral("group"), group->name, {}};
    }
    if (isScreenTargetId(q)) {
        const QString conn = connectorFromScreenTargetId(q);
        for (const ScreenInfo &s : screens) {
            if (s.connector == conn) {
                return {true, q, QStringLiteral("monitor"), store_.monitorName(s), {}};
            }
        }
        return {false, {}, {}, {}, tr("Monitor %1 is not connected.").arg(conn)};
    }

    if (q.startsWith(QLatin1String("monitor:"), Qt::CaseInsensitive)) {
        const QString want = q.mid(8).trimmed();
        for (const ScreenInfo &s : screens) {
            if (s.connector.compare(want, Qt::CaseInsensitive) == 0
                || store_.monitorName(s).compare(want, Qt::CaseInsensitive) == 0) {
                return {true, screenTargetId(s.connector), QStringLiteral("monitor"), store_.monitorName(s), {}};
            }
        }
        return {false, {}, {}, {}, tr("Monitor “%1” was not found or is not connected.").arg(want)};
    }

    QList<CliResolveResult> matches;
    for (const Group &group : store_.groups) {
        if (group.name.compare(q, Qt::CaseInsensitive) == 0) {
            matches.append({true, group.id, QStringLiteral("group"), group.name, {}});
        }
    }
    for (const Area &area : store_.areas) {
        if (area.name.compare(q, Qt::CaseInsensitive) == 0) {
            matches.append({true, area.id, QStringLiteral("area"), area.name, {}});
        }
    }
    for (const ScreenInfo &s : screens) {
        if (s.connector.compare(q, Qt::CaseInsensitive) == 0
            || store_.monitorName(s).compare(q, Qt::CaseInsensitive) == 0) {
            matches.append({true, screenTargetId(s.connector), QStringLiteral("monitor"), store_.monitorName(s), {}});
        }
    }
    if (matches.size() == 1) {
        return matches.first();
    }
    if (matches.size() > 1) {
        return {false, {}, {}, {}, tr("Target “%1” matches multiple targets.").arg(q)};
    }
    return {false, {}, {}, {}, tr("Target “%1” was not found.").arg(q)};
}

QJsonObject App::addAreaRect(const QString &name, const QString &connector, const QRect &rect)
{
    if (!rect.isValid() || rect.width() <= 0 || rect.height() <= 0) {
        return {{QStringLiteral("ok"), false}, {QStringLiteral("exitCode"), 2},
                {QStringLiteral("error"), tr("Area width and height must be positive.")}};
    }
    const QList<ScreenInfo> screens = currentScreens();
    if (screens.isEmpty()) {
        return {{QStringLiteral("ok"), false}, {QStringLiteral("exitCode"), 3},
                {QStringLiteral("error"), tr("No monitor is connected.")}};
    }

    const QString trimmedName = name.trimmed();
    QStringList connectors;
    for (const ScreenInfo &s : screens) {
        connectors.append(s.connector);
    }
    if (!trimmedName.isEmpty()) {
        if (sanitizeFilenameToken(trimmedName).isEmpty()) {
            return {{QStringLiteral("ok"), false}, {QStringLiteral("exitCode"), 2},
                    {QStringLiteral("error"), tr("Area name cannot be used in a file name.")}};
        }
        if (!store_.isNameAvailable(trimmedName, QString(), connectors)) {
            return {{QStringLiteral("ok"), false}, {QStringLiteral("exitCode"), 1},
                    {QStringLiteral("error"), tr("“%1” is already used by another target or group.").arg(trimmedName)}};
        }
    }

    QString createdId;
    if (!connector.trimmed().isEmpty()) {
        const auto it = std::find_if(screens.cbegin(), screens.cend(), [&](const ScreenInfo &s) {
            return s.connector.compare(connector.trimmed(), Qt::CaseInsensitive) == 0
                || store_.monitorName(s).compare(connector.trimmed(), Qt::CaseInsensitive) == 0;
        });
        if (it == screens.cend()) {
            return {{QStringLiteral("ok"), false}, {QStringLiteral("exitCode"), 3},
                    {QStringLiteral("error"), tr("Monitor “%1” is not connected.").arg(connector)}};
        }
        if (!QRect(QPoint(), it->geometry.size()).contains(rect)) {
            return {{QStringLiteral("ok"), false}, {QStringLiteral("exitCode"), 2},
                    {QStringLiteral("error"), tr("Rectangle lies outside monitor %1 (%2×%3).")
                                                 .arg(it->connector)
                                                 .arg(it->geometry.width())
                                                 .arg(it->geometry.height())}};
        }
        Area &area = store_.addArea(*it, rect);
        if (!trimmedName.isEmpty()) {
            area.name = trimmedName;
        }
        createdId = area.id;
    } else {
        QRect bounds;
        for (const ScreenInfo &s : screens) {
            bounds = bounds.united(s.geometry);
        }
        if (intersectingScreens(screens, rect).isEmpty()) {
            return {{QStringLiteral("ok"), false}, {QStringLiteral("exitCode"), 2},
                    {QStringLiteral("error"), tr("Global rectangle does not intersect any connected monitor.")}};
        }
        Area &area = store_.addAreaForGlobalRect(screens, rect);
        if (!trimmedName.isEmpty()) {
            area.name = trimmedName;
        }
        createdId = area.id;
    }

    storeChanged();
    const Area *saved = store_.area(createdId);
    return {
        {QStringLiteral("ok"), true},
        {QStringLiteral("targetId"), createdId},
        {QStringLiteral("name"), saved ? saved->name : trimmedName},
    };
}

QString App::openOverlay(const QString &mode, int delaySeconds)
{
    const QString normMode = mode.trimmed().toLower();
    std::optional<bool> forcedFrozen;
    if (normMode == QLatin1String("frozen")) {
        forcedFrozen = true;
    } else if (normMode == QLatin1String("live")) {
        forcedFrozen = false;
    } else if (!normMode.isEmpty()) {
        return tr("Invalid overlay mode: %1").arg(mode);
    }

    const int delay = std::clamp(delaySeconds, 0, 60);
    if (delay > 0) {
        if (countdown_->isActive()) {
            return tr("A capture countdown is already running.");
        }
        runWithDelay(delay, tr("Screen"), [this, forcedFrozen] { startPrintNow(forcedFrozen, true); });
        return {};
    }
    startPrintNow(forcedFrozen, true);
    return {};
}

QJsonObject App::lastCaptureInfo() const
{
    QJsonArray filesArr;
    for (const QString &path : lastPaths_) {
        const QFileInfo fi(path);
        const QSize sz = fi.exists() ? QImageReader(path).size() : QSize();
        const std::optional<ImageFormat> fmt = imageFormatFromPath(path);
        filesArr.append(QJsonObject{
            {QStringLiteral("path"), path},
            {QStringLiteral("exists"), fi.exists()},
            {QStringLiteral("width"), sz.isValid() ? sz.width() : 0},
            {QStringLiteral("height"), sz.isValid() ? sz.height() : 0},
            {QStringLiteral("format"), fmt ? formatConfigKey(*fmt) : fi.suffix().toLower()},
            {QStringLiteral("bytes"), fi.exists() ? fi.size() : 0},
        });
    }
    return {
        {QStringLiteral("hasLastCapture"), !lastPaths_.isEmpty()},
        {QStringLiteral("targetId"), lastCaptureTargetId_},
        {QStringLiteral("displayName"), lastCaptureDisplayName_},
        {QStringLiteral("timestamp"),
         lastCaptureTimestamp_.isValid() ? lastCaptureTimestamp_.toString(Qt::ISODate) : QString()},
        {QStringLiteral("files"), filesArr},
    };
}

void App::captureAsync(const CliCaptureRequest &request, std::function<void(const CliCaptureResult &)> callback)
{
    const auto fail = [&](int exitCode, const QString &message) {
        CliCaptureResult res;
        res.ok = false;
        res.exitCode = exitCode;
        res.error = message;
        if (callback) {
            callback(res);
        }
    };

    const auto applyOverrides = [&](ExportOptions &opts) {
        if (request.format) {
            opts.format = *request.format;
        } else if (!request.outPath.isEmpty()) {
            if (const std::optional<ImageFormat> inferred = imageFormatFromPath(request.outPath);
                inferred && isImageFormatSupported(*inferred)) {
                opts.format = *inferred;
            }
        }
        if (request.quality) {
            opts.quality = std::clamp(*request.quality, 1, 100);
        }
        if (request.pngCompression) {
            opts.pngCompression = std::clamp(*request.pngCompression, 0, 9);
        }
        if (request.lossless) {
            opts.lossless = *request.lossless;
        }
        if (!isImageFormatSupported(opts.format)) {
            opts.format = ImageFormat::Png;
        }
    };

    const QList<ScreenInfo> screens = currentScreens();
    bool isWindowCapture = false;
    WindowInfo targetWindow;
    TriggerPlan plan;
    QString resolvedTargetId;

    const QString targetTrimmed = request.target.trimmed();
    if (targetTrimmed.startsWith(QLatin1String("window:"), Qt::CaseInsensitive)) {
        const QString winQuery = targetTrimmed.mid(7).trimmed();
        if (winQuery.isEmpty()) {
            fail(2, tr("Window specifier after 'window:' cannot be empty."));
            return;
        }
        const QList<WindowInfo> windows = listWindows(QCoreApplication::applicationPid());
        QList<WindowInfo> matches;
        for (const WindowInfo &w : windows) {
            if (w.handle == winQuery) {
                matches = {w};
                break;
            }
            if (w.appName.compare(winQuery, Qt::CaseInsensitive) == 0
                || w.caption.compare(winQuery, Qt::CaseInsensitive) == 0) {
                matches.append(w);
            }
        }
        if (matches.isEmpty()) {
            for (const WindowInfo &w : windows) {
                if (w.appName.contains(winQuery, Qt::CaseInsensitive)
                    || w.caption.contains(winQuery, Qt::CaseInsensitive)) {
                    matches.append(w);
                }
            }
        }
        if (matches.isEmpty()) {
            fail(3, tr("No open window matches “%1”.").arg(winQuery));
            return;
        }
        if (matches.size() > 1) {
            fail(3, tr("Window query “%1” matches multiple open windows.").arg(winQuery));
            return;
        }
        isWindowCapture = true;
        targetWindow = matches.first();
        resolvedTargetId = QStringLiteral("window:") + targetWindow.handle;
        plan.triggerId = resolvedTargetId;
        plan.displayName = targetWindow.caption.isEmpty() ? windowToken(targetWindow) : targetWindow.caption;
        plan.includeCursor = request.cursor.value_or(false);
    } else if (targetTrimmed.startsWith(QLatin1String("region:"), Qt::CaseInsensitive)) {
        const QString body = targetTrimmed.mid(7);
        const qsizetype sep = body.lastIndexOf(QLatin1Char(':'));
        if (sep <= 0) {
            fail(2, tr("Invalid region specification (expected region:<monitor>:<x>,<y>,<w>,<h>)."));
            return;
        }
        const QString connQuery = body.left(sep).trimmed();
        const QStringList nums = body.mid(sep + 1).split(QLatin1Char(','));
        if (nums.size() != 4) {
            fail(2, tr("Invalid region geometry (expected <x>,<y>,<w>,<h>)."));
            return;
        }
        int coords[4]{};
        for (int i = 0; i < 4; ++i) {
            bool ok = false;
            coords[i] = nums.at(i).trimmed().toInt(&ok);
            if (!ok) {
                fail(2, tr("Invalid region coordinate value."));
                return;
            }
        }
        const QRect rect(coords[0], coords[1], coords[2], coords[3]);
        if (rect.width() <= 0 || rect.height() <= 0) {
            fail(2, tr("Region width and height must be positive."));
            return;
        }
        const auto sIt = std::find_if(screens.cbegin(), screens.cend(), [&](const ScreenInfo &s) {
            return s.connector.compare(connQuery, Qt::CaseInsensitive) == 0
                || store_.monitorName(s).compare(connQuery, Qt::CaseInsensitive) == 0;
        });
        if (sIt == screens.cend()) {
            fail(3, tr("Monitor “%1” is not connected.").arg(connQuery));
            return;
        }
        if (!QRect(QPoint(), sIt->geometry.size()).contains(rect)) {
            fail(2, tr("Region lies outside monitor %1 (%2×%3).")
                        .arg(sIt->connector)
                        .arg(sIt->geometry.width())
                        .arg(sIt->geometry.height()));
            return;
        }
        ExportOptions opts = store_.effectiveExportOptions(screenTargetId(sIt->connector));
        applyOverrides(opts);
        resolvedTargetId = QStringLiteral("region:%1:%2,%3,%4,%5")
                               .arg(sIt->connector)
                               .arg(rect.x())
                               .arg(rect.y())
                               .arg(rect.width())
                               .arg(rect.height());
        plan.triggerId = resolvedTargetId;
        plan.displayName = QStringLiteral("%1-region").arg(store_.monitorName(*sIt));
        plan.connectors = {sIt->connector};
        plan.screens = screens;
        plan.includeCursor = request.cursor.value_or(false);
        plan.outputs.append(
            {OutputSpec::Kind::Area, resolvedTargetId, store_.monitorName(*sIt) + QStringLiteral("-region"),
             sIt->connector, {}, rect, opts});
    } else {
        const CliResolveResult resolved = resolveTarget(targetTrimmed);
        if (!resolved.ok) {
            fail(3, resolved.error);
            return;
        }
        resolvedTargetId = resolved.targetId;
        plan = planTrigger(store_, resolvedTargetId, screens);
        if (plan.outputs.isEmpty()) {
            const QString errMsg = !plan.errors.isEmpty() ? plan.errors.join(QLatin1Char('\n'))
                                                          : (!plan.partialNotifications.isEmpty()
                                                                 ? plan.partialNotifications.join(QLatin1Char('\n'))
                                                                 : tr("Target is not available."));
            fail(3, errMsg);
            return;
        }
        if (plan.outputs.size() > 1 && (!request.outPath.isEmpty() || request.toStdout || request.clipboard)) {
            fail(2, tr("Groups produce multiple images and cannot be written to a single --out path, --stdout, or "
                       "--clipboard."));
            return;
        }
        if (request.cursor) {
            plan.includeCursor = *request.cursor;
        }
        for (OutputSpec &out : plan.outputs) {
            applyOverrides(out.exportOptions);
        }
    }

    ExportOptions windowOpts = store_.exportOptions;
    applyOverrides(windowOpts);

    const auto executeCapture = [this, request, plan, isWindowCapture, targetWindow, windowOpts, resolvedTargetId,
                                 callback] {
        const QDateTime timestamp = QDateTime::currentDateTime();
        const QString directory = store_.effectiveCaptureDirectory();
        const QString filenameTemplate = store_.effectiveFilenameTemplate();

        struct WorkerOutput
        {
            CliCaptureResult cliResult;
            QImage clipboardImage;
            QStringList writtenPaths;
        };

        auto *watcher = new QFutureWatcher<WorkerOutput>(this);
        connect(watcher, &QFutureWatcher<WorkerOutput>::finished, this,
                [this, watcher, request, resolvedTargetId, plan, callback] {
                    pins_->endCapture();
                    WorkerOutput out = watcher->result();
                    watcher->deleteLater();
                    if (out.cliResult.ok && request.clipboard && !out.clipboardImage.isNull()) {
                        QGuiApplication::clipboard()->setImage(out.clipboardImage);
                    }
                    // Files that reached the disk are recorded even when a later group member
                    // failed, so they are not left behind untracked.
                    if (!out.writtenPaths.isEmpty()) {
                        recordCapture(out.writtenPaths, out.cliResult.displayName, resolvedTargetId,
                                      request.triggerKind.isEmpty() ? QStringLiteral("cli") : request.triggerKind);
                        const QString fmt = out.cliResult.files.isEmpty()
                                                ? formatConfigKey(store_.effectiveExportOptions(resolvedTargetId).format)
                                                : out.cliResult.files.first().format;
                        const QString groupId = store_.group(resolvedTargetId) ? resolvedTargetId : QString();
                        const QString trigger =
                            request.triggerKind.isEmpty() ? QStringLiteral("cli") : request.triggerKind;
                        runPostCaptureHook(resolvedTargetId, out.cliResult.displayName, out.writtenPaths,
                                           QDateTime::currentDateTime(), fmt, trigger, false, groupId);
                    } else if (out.cliResult.ok) {
                        tray_->blink();
                        Q_EMIT captured(true);
                    }
                    if (!out.cliResult.ok && out.writtenPaths.isEmpty()) {
                        Q_EMIT captured(false);
                    }
                    if (callback) {
                        callback(out.cliResult);
                    }
                });

        const std::shared_ptr<const OcrEngine> ocrEngine = request.ocr ? ocr_ : nullptr;
        const QStringList ocrLangs = request.ocr ? ocrLanguages() : QStringList();
        // The worker starts once the compositor shows the now transparent pins.
        pins_->beginCapture(watcher, [=] {
            watcher->setFuture(QtConcurrent::run([=] {
                WorkerOutput out;
                out.cliResult.targetId = resolvedTargetId;
                out.cliResult.displayName = plan.displayName;
                out.cliResult.timestamp = timestamp.toString(Qt::ISODate);

                QList<RenderedOutput> outputs;
                if (isWindowCapture) {
                    const CaptureResult cr = CaptureService::captureWindow(targetWindow.handle, plan.includeCursor);
                    if (cr.image.isNull()) {
                        out.cliResult.exitCode = 1;
                        out.cliResult.error = cr.error.isEmpty() ? App::tr("Window capture failed.") : cr.error;
                        return out;
                    }
                    outputs.append({windowToken(targetWindow), cr.image, {}, windowOpts, resolvedTargetId});
                } else {
                    QHash<QString, CaptureResult> frames;
                    std::mutex framesMutex;
                    QtConcurrent::blockingMap(plan.connectors, [&](const QString &connector) {
                        CaptureResult frame = CaptureService::captureScreen(connector, plan.includeCursor);
                        const std::lock_guard lock(framesMutex);
                        frames.insert(connector, std::move(frame));
                    });
                    outputs = renderOutputs(plan, frames);
                }

                const OutputWriter writer(directory, filenameTemplate);
                for (const RenderedOutput &rendered : std::as_const(outputs)) {
                    if (rendered.image.isNull()) {
                        out.cliResult.exitCode = 1;
                        out.cliResult.error = rendered.error;
                        return out;
                    }
                    if (out.clipboardImage.isNull()) {
                        out.clipboardImage = rendered.image;
                    }

                    if (request.noSave) {
                        if (request.toStdout) {
                            QByteArray encoded;
                            QBuffer buf(&encoded);
                            buf.open(QIODevice::WriteOnly);
                            QString encErr;
                            if (!encodeImageToDevice(&buf, rendered.image, rendered.exportOptions, &encErr)) {
                                out.cliResult.exitCode = 1;
                                out.cliResult.error = encErr;
                                return out;
                            }
                            const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
                            const QString tmpBase = runtimeDir.isEmpty() ? QDir::tempPath() : runtimeDir;
                            QTemporaryFile tmp(
                                QDir(tmpBase).filePath(QStringLiteral("glimply-stdout-XXXXXX.%1")
                                                           .arg(canonicalExtension(rendered.exportOptions.format))));
                            tmp.setAutoRemove(false);
                            if (!tmp.open()) {
                                out.cliResult.exitCode = 1;
                                out.cliResult.error = tmp.errorString();
                                return out;
                            }
                            tmp.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                            if (tmp.write(encoded) != encoded.size() || !tmp.flush()) {
                                out.cliResult.exitCode = 1;
                                out.cliResult.error = tmp.errorString();
                                tmp.close();
                                QFile::remove(tmp.fileName());
                                return out;
                            }
                            out.cliResult.stdoutFile = tmp.fileName();
                            out.cliResult.tempStdoutFile = true;
                            tmp.close();
                        }
                        continue;
                    }

                    QString savedPath;
                    QString writeErr;
                    bool wrote = false;
                    if (!request.outPath.isEmpty()) {
                        const QString absOut = QFileInfo(request.outPath).absoluteFilePath();
                        wrote = writeImageToFile(absOut, rendered.image, rendered.exportOptions, &writeErr);
                        savedPath = absOut;
                    } else {
                        wrote = writer.writeImage(rendered.image, rendered.token, timestamp, rendered.exportOptions,
                                                  &savedPath, &writeErr);
                    }
                    if (!wrote) {
                        out.cliResult.exitCode = 1;
                        out.cliResult.error = writeErr;
                        return out;
                    }
                    out.writtenPaths.append(savedPath);
                    out.cliResult.files.append({rendered.targetId, savedPath, rendered.image.width(),
                                                rendered.image.height(),
                                                formatConfigKey(rendered.exportOptions.format), QFileInfo(savedPath).size(),
                                                QString()});
                    if (request.toStdout && out.cliResult.stdoutFile.isEmpty()) {
                        out.cliResult.stdoutFile = savedPath;
                        out.cliResult.tempStdoutFile = false;
                    }
                }

                if (request.ocr) {
                    out.cliResult.ocrRequested = true;
                    if (!ocrEngine || !ocrEngine->available()) {
                        out.cliResult.ocrError = App::tr("Text recognition needs tesseract. Install tesseract and a "
                                                         "language package such as tesseract-data-eng.");
                    } else {
                        for (qsizetype i = 0; i < outputs.size(); ++i) {
                            const OcrResult text = ocrEngine->recognize(outputs.at(i).image, ocrLangs);
                            if (!text.ok) {
                                out.cliResult.ocrError = text.error;
                                break;
                            }
                            out.cliResult.ocrTexts.append({outputs.at(i).token, text.text});
                            if (i < out.cliResult.files.size()) {
                                out.cliResult.files[i].text = text.text;
                            }
                        }
                    }
                }

                out.cliResult.ok = true;
                out.cliResult.exitCode = 0;
                return out;
            }));
        });
    };

    if (request.delaySeconds > 0) {
        if (countdown_->isActive()) {
            fail(1, tr("A capture countdown is already running."));
            return;
        }
        runWithDelay(request.delaySeconds, plan.displayName, executeCapture,
                     [fail] { fail(1, tr("Capture countdown was cancelled.")); });
        return;
    }

    executeCapture();
}

void App::runPostCaptureHook(const QString &targetId, const QString &displayName, const QStringList &files,
                             const QDateTime &timestamp, const QString &format, const QString &triggerKind,
                             bool isInterval, const QString &groupId)
{
    if (files.isEmpty() || !hooks_) {
        return;
    }
    const HookConfig cfg = store_.effectiveHookConfig(targetId, groupId, isInterval);
    if (!cfg.enabled || cfg.command.trimmed().isEmpty()) {
        return;
    }
    HookInvocation inv;
    inv.command = cfg.command;
    inv.timeoutSeconds = cfg.timeoutSeconds;
    inv.files = files;
    inv.targetId = targetId;
    inv.targetName = displayName;
    inv.triggerKind = triggerKind;
    inv.timestamp = timestamp;
    inv.format = format;
    hooks_->run(inv);
}

void App::testPostCaptureHook(const QString &command, int timeoutSeconds,
                              std::function<void(const HookResult &)> onDone)
{
    if (!hooks_) {
        return;
    }
    hooks_->runTest(command, timeoutSeconds, std::move(onDone));
}

void App::editArea(const QString &areaId)
{
    const Area *area = store_.area(areaId);
    if (!area || selector_ || session_) {
        return;
    }
    // An open settings window has to step aside first; it runs the same overlay itself.
    if (auto *window = qobject_cast<SettingsWindow *>(settings_.data()); window && window->isVisible()) {
        window->editArea(areaId);
        return;
    }
    QRect initialGlobal;
    if (area->isCrossMonitor()) {
        initialGlobal = area->rect;
    } else if (const QScreen *screen = findScreen(area->connector)) {
        initialGlobal = area->rect.translated(screen->geometry().topLeft());
    }
    auto *selector = new AreaSelector(this);
    selector_ = selector;
    connect(selector, &AreaSelector::selected, this, [this, selector, areaId](const QRect &globalRect) {
        selector->deleteLater();
        Area *edited = store_.area(areaId);
        if (!edited) {
            return;
        }
        const QList<ScreenInfo> hit = intersectingScreens(currentScreens(), globalRect);
        if (hit.isEmpty()) {
            return;
        }
        if (hit.size() == 1 && hit.first().geometry.contains(globalRect)) {
            edited->connector = hit.first().connector;
            edited->monitorIdentity = hit.first().identity();
            edited->monitors.clear();
            edited->rect = globalRect.translated(-hit.first().geometry.topLeft());
        } else {
            edited->connector.clear();
            edited->monitorIdentity.clear();
            edited->monitors.clear();
            for (const ScreenInfo &s : hit) {
                edited->monitors.append({s.connector, s.identity()});
            }
            edited->rect = globalRect;
        }
        storeChanged();
    });
    connect(selector, &AreaSelector::cancelled, selector, &QObject::deleteLater);
    selector->start(initialGlobal, store_.showSelectionLoupe);
}

void App::applyAutostart()
{
    if (!store_.autostartInitialized) {
        // First start: autostart is on by default (decision), and the choice is written down.
        store_.autostart = true;
        store_.autostartInitialized = true;
        store_.save(*config_);
    }

    const QString fileName = QGuiApplication::desktopFileName() + QStringLiteral(".desktop");
    const QString autostartDir =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/autostart");
    const QString autostartPath = autostartDir + QLatin1Char('/') + fileName;

    if (!store_.autostart) {
        if (QFile::exists(autostartPath) && !QFile::remove(autostartPath)) {
            notify(QStringLiteral("settingsProblem"), tr("Autostart"), {tr("Could not remove %1.").arg(autostartPath)});
        }
        return;
    }

    // Start the executable that is running now, so a moved or reinstalled binary stays consistent.
    QByteArray content;
    const QString installed = QStandardPaths::locate(QStandardPaths::ApplicationsLocation, fileName);
    QFile source(installed);
    if (!installed.isEmpty() && source.open(QIODevice::ReadOnly)) {
        content = source.readAll();
    } else {
        content = QByteArrayLiteral("[Desktop Entry]\nType=Application\nName=glimply\nExec=@EXEC@\n"
                                    "Icon=io.github.m00sfett.glimply\nStartupNotify=false\n"
                                    "X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2\n");
    }
    QStringList lines = QString::fromUtf8(content).split(QLatin1Char('\n'));
    for (QString &line : lines) {
        if (line.startsWith(QLatin1String("Exec="))) {
            line = QStringLiteral("Exec=") + QCoreApplication::applicationFilePath();
        }
    }
    content = lines.join(QLatin1Char('\n')).toUtf8();

    QFile existing(autostartPath);
    if (existing.open(QIODevice::ReadOnly) && existing.readAll() == content) {
        return;
    }
    existing.close();
    QDir().mkpath(autostartDir);
    QSaveFile file(autostartPath);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit()) {
        notify(QStringLiteral("settingsProblem"), tr("Autostart"), {tr("Could not write %1.").arg(autostartPath)});
    }
}

namespace
{

std::optional<McpImagePayload> buildMcpImagePayload(const QString &filePath, int maxDimension, QString *errorOut)
{
    QImageReader reader(filePath);
    reader.setAutoTransform(true);
    QImage img = reader.read();
    if (img.isNull()) {
        if (errorOut) {
            *errorOut = App::tr("Could not read saved image for inline MCP output: %1").arg(reader.errorString());
        }
        return std::nullopt;
    }
    if (maxDimension > 0 && (img.width() > maxDimension || img.height() > maxDimension)) {
        img = img.scaled(maxDimension, maxDimension, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    ExportOptions opts;
    opts.format = imageFormatFromPath(filePath).value_or(ImageFormat::Png);
    if (!isImageFormatSupported(opts.format)) {
        opts.format = ImageFormat::Png;
    }

    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    QString encErr;
    if (!encodeImageToDevice(&buffer, img, opts, &encErr)) {
        if (errorOut) {
            *errorOut = encErr;
        }
        return std::nullopt;
    }

    return McpImagePayload{
        encoded.toBase64(),
        QStringLiteral("image/") + formatConfigKey(opts.format),
    };
}

} // namespace

void App::mcpDispatchAsync(const QString &toolName, const QString &argumentsJson,
                           std::function<void(const McpToolResponse &)> callback)
{
    const auto fail = [callback](const QString &message) {
        if (callback) {
            callback({false, {}, message, std::nullopt});
        }
    };

    QJsonObject args;
    if (!argumentsJson.trimmed().isEmpty()) {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(argumentsJson.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            fail(tr("Invalid MCP tool arguments JSON: %1").arg(err.errorString()));
            return;
        }
        args = doc.object();
    }

    // Enforce the global opt-in toggle before inspecting targets, windows, or paths.
    const McpPolicyResult basePolicy = evaluateMcpAccess(store_.mcp, toolName);
    if (basePolicy.decision == McpDecision::Deny && !store_.mcp.enabled) {
        fail(basePolicy.reason);
        return;
    }

    const auto runWithOptionalConfirm = [this, callback, fail](const McpPolicyResult &policy,
                                                               const QString &confirmSummary,
                                                               bool delayAfterHide,
                                                               std::function<void()> action) {
        if (policy.decision == McpDecision::Deny) {
            fail(policy.reason);
            return;
        }
        if (policy.decision == McpDecision::Allow) {
            action();
            return;
        }
        auto *dialog = new McpConfirmDialog(confirmSummary, 30);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        QPointer<App> safeApp(this);
        QPointer<McpConfirmDialog> safeDialog(dialog);
        connect(dialog, &McpConfirmDialog::decided, this,
                [safeApp, safeDialog, fail, delayAfterHide, action = std::move(action)](bool allowed) mutable {
                    if (safeDialog) {
                        safeDialog->deleteLater();
                    }
                    if (!allowed) {
                        fail(App::tr("MCP request was denied by user or timed out."));
                        return;
                    }
                    if (!safeApp) {
                        fail(App::tr("glimply is shutting down."));
                        return;
                    }
                    if (delayAfterHide) {
                        QTimer::singleShot(300, safeApp.data(), std::move(action));
                    } else {
                        action();
                    }
                });
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
    };

    const auto finishCaptureResult = [this, callback, fail](const CliCaptureResult &res, bool includeImage,
                                                            int maxDimension) {
        if (!res.ok) {
            fail(res.error.isEmpty() ? tr("Capture failed.") : res.error);
            return;
        }
        if (store_.mcp.notifyOnCapture) {
            QStringList names;
            names.reserve(res.files.size());
            for (const CliCaptureFile &f : res.files) {
                names.append(QFileInfo(f.path).fileName());
            }
            notify(QStringLiteral("mcpCapture"), tr("MCP captured %1").arg(res.displayName), names);
        }

        QJsonObject meta = captureResultToJson(res);
        std::optional<McpImagePayload> imagePayload;
        if (includeImage && !res.files.isEmpty()) {
            QString imgErr;
            imagePayload = buildMcpImagePayload(res.files.first().path, maxDimension, &imgErr);
            if (!imagePayload) {
                fail(imgErr);
                return;
            }
        }
        if (callback) {
            callback({true, meta, {}, std::move(imagePayload)});
        }
    };

    const auto parseFormatAndQuality = [&](std::optional<ImageFormat> *outFormat, std::optional<int> *outQuality,
                                           QString *errMsg) -> bool {
        if (args.contains(QStringLiteral("format"))) {
            const QString rawFmt = args.value(QStringLiteral("format")).toString().trimmed();
            const std::optional<ImageFormat> parsed = imageFormatFromKey(rawFmt);
            if (!parsed || !isImageFormatSupported(*parsed)) {
                *errMsg = tr("Unsupported image format '%1'.").arg(rawFmt);
                return false;
            }
            *outFormat = *parsed;
        }
        if (args.contains(QStringLiteral("quality"))) {
            const QJsonValue qVal = args.value(QStringLiteral("quality"));
            if (!qVal.isDouble()) {
                *errMsg = tr("'quality' must be an integer between 1 and 100.");
                return false;
            }
            *outQuality = std::clamp(qVal.toInt(), 1, 100);
        }
        return true;
    };

    if (toolName == QLatin1String("list_targets")) {
        if (basePolicy.decision == McpDecision::Deny) {
            fail(basePolicy.reason);
            return;
        }
        QJsonArray arr;
        const bool filterAllowlist = (store_.mcp.accessMode == McpAccessMode::Allowlist);
        for (const MenuTarget &mt : menuTargets()) {
            if (filterAllowlist && !store_.mcp.allowedTargetIds.contains(mt.id)) {
                continue;
            }
            QString kind;
            switch (mt.section) {
            case MenuTarget::Section::Group:
                kind = QStringLiteral("group");
                break;
            case MenuTarget::Section::Area:
                kind = QStringLiteral("area");
                break;
            case MenuTarget::Section::Monitor:
                kind = (mt.id == allMonitorsTargetId()) ? QStringLiteral("all") : QStringLiteral("monitor");
                break;
            }
            arr.append(QJsonObject{
                {QStringLiteral("id"), mt.id},
                {QStringLiteral("name"), mt.name},
                {QStringLiteral("kind"), kind},
                {QStringLiteral("detail"), mt.detail},
                {QStringLiteral("available"), mt.available},
            });
        }
        callback({true, QJsonObject{{QStringLiteral("targets"), arr}}, {}, std::nullopt});
        return;
    }

    if (toolName == QLatin1String("list_monitors")) {
        if (basePolicy.decision == McpDecision::Deny) {
            fail(basePolicy.reason);
            return;
        }
        const bool filterAllowlist = (store_.mcp.accessMode == McpAccessMode::Allowlist);
        bool allowAllConnectors = false;
        QSet<QString> allowedConnectors;
        if (filterAllowlist) {
            const auto addTargetConnectors = [&](const QString &tid, auto &self) -> void {
                if (tid == allMonitorsTargetId()) {
                    allowAllConnectors = true;
                } else if (isScreenTargetId(tid)) {
                    allowedConnectors.insert(connectorFromScreenTargetId(tid));
                } else if (const Area *a = store_.area(tid)) {
                    for (const QString &conn : a->involvedConnectors()) {
                        allowedConnectors.insert(conn);
                    }
                } else if (const Group *g = store_.group(tid)) {
                    for (const QString &memberId : g->targetIds) {
                        if (!store_.group(memberId)) {
                            self(memberId, self);
                        }
                    }
                }
            };
            for (const QString &id : store_.mcp.allowedTargetIds) {
                addTargetConnectors(id, addTargetConnectors);
            }
        }

        QJsonArray arr;
        for (const ScreenInfo &s : currentScreens()) {
            if (filterAllowlist && !allowAllConnectors && !allowedConnectors.contains(s.connector)) {
                continue;
            }
            arr.append(QJsonObject{
                {QStringLiteral("connector"), s.connector},
                {QStringLiteral("name"), store_.monitorName(s)},
                {QStringLiteral("identity"), s.identity()},
                {QStringLiteral("targetId"), screenTargetId(s.connector)},
                {QStringLiteral("x"), s.geometry.x()},
                {QStringLiteral("y"), s.geometry.y()},
                {QStringLiteral("width"), s.geometry.width()},
                {QStringLiteral("height"), s.geometry.height()},
            });
        }
        callback({true, QJsonObject{{QStringLiteral("monitors"), arr}}, {}, std::nullopt});
        return;
    }

    if (toolName == QLatin1String("list_windows")) {
        if (basePolicy.decision == McpDecision::Deny) {
            fail(basePolicy.reason);
            return;
        }
        QJsonArray arr;
        for (const WindowInfo &win : listWindows(QCoreApplication::applicationPid())) {
            arr.append(QJsonObject{
                {QStringLiteral("handle"), win.handle},
                {QStringLiteral("caption"), win.caption},
                {QStringLiteral("appName"), win.appName},
                {QStringLiteral("x"), win.geometry.x()},
                {QStringLiteral("y"), win.geometry.y()},
                {QStringLiteral("width"), win.geometry.width()},
                {QStringLiteral("height"), win.geometry.height()},
                {QStringLiteral("minimized"), win.minimized},
            });
        }
        callback({true, QJsonObject{{QStringLiteral("windows"), arr}}, {}, std::nullopt});
        return;
    }

    if (toolName == QLatin1String("capture_target")) {
        const QString query = args.value(QStringLiteral("target")).toString().trimmed();
        if (query.isEmpty()) {
            fail(tr("'target' is required."));
            return;
        }
        std::optional<ImageFormat> fmt;
        std::optional<int> quality;
        QString optErr;
        if (!parseFormatAndQuality(&fmt, &quality, &optErr)) {
            fail(optErr);
            return;
        }
        const bool includeImage = args.value(QStringLiteral("include_image")).toBool(false);
        const int maxDim = std::max(0, args.value(QStringLiteral("max_dimension")).toInt(0));

        const CliResolveResult resolved = resolveTarget(query);
        const McpPolicyResult policy =
            evaluateMcpAccess(store_.mcp, toolName, resolved.ok ? resolved.targetId : query);
        if (policy.decision == McpDecision::Deny) {
            fail(policy.reason);
            return;
        }
        if (!resolved.ok) {
            fail(resolved.error);
            return;
        }

        CliCaptureRequest req;
        req.target = resolved.targetId;
        req.format = fmt;
        req.quality = quality;
        if (args.contains(QStringLiteral("include_cursor"))) {
            req.cursor = args.value(QStringLiteral("include_cursor")).toBool();
        }
        req.triggerKind = QStringLiteral("mcp");

        const QString summary =
            tr("An external MCP client wants to capture \"%1\" (%2).").arg(resolved.name, resolved.targetId);
        QPointer<App> safeApp(this);
        runWithOptionalConfirm(policy, summary, true, [safeApp, req, finishCaptureResult, includeImage, maxDim, fail] {
            if (!safeApp) {
                fail(App::tr("glimply is shutting down."));
                return;
            }
            safeApp->captureAsync(req, [finishCaptureResult, includeImage, maxDim](const CliCaptureResult &res) {
                finishCaptureResult(res, includeImage, maxDim);
            });
        });
        return;
    }

    if (toolName == QLatin1String("capture_region")) {
        if (basePolicy.decision == McpDecision::Deny) {
            fail(basePolicy.reason);
            return;
        }
        const QString monitor = args.value(QStringLiteral("monitor")).toString().trimmed();
        const QJsonValue xv = args.value(QStringLiteral("x"));
        const QJsonValue yv = args.value(QStringLiteral("y"));
        const QJsonValue wv = args.value(QStringLiteral("width"));
        const QJsonValue hv = args.value(QStringLiteral("height"));
        if (monitor.isEmpty() || !xv.isDouble() || !yv.isDouble() || !wv.isDouble() || !hv.isDouble()) {
            fail(tr("capture_region requires monitor, x, y, width, and height."));
            return;
        }
        const int x = xv.toInt();
        const int y = yv.toInt();
        const int w = wv.toInt();
        const int h = hv.toInt();
        if (w <= 0 || h <= 0) {
            fail(tr("Region width and height must be positive."));
            return;
        }
        std::optional<ImageFormat> fmt;
        std::optional<int> quality;
        QString optErr;
        if (!parseFormatAndQuality(&fmt, &quality, &optErr)) {
            fail(optErr);
            return;
        }
        const bool includeImage = args.value(QStringLiteral("include_image")).toBool(false);
        const int maxDim = std::max(0, args.value(QStringLiteral("max_dimension")).toInt(0));

        CliCaptureRequest req;
        req.target = QStringLiteral("region:%1:%2,%3,%4,%5").arg(monitor).arg(x).arg(y).arg(w).arg(h);
        req.format = fmt;
        req.quality = quality;
        if (args.contains(QStringLiteral("include_cursor"))) {
            req.cursor = args.value(QStringLiteral("include_cursor")).toBool();
        }
        req.triggerKind = QStringLiteral("mcp");

        const QString summary =
            tr("An external MCP client wants to capture region %1x%2 at (%3, %4) on %5.")
                .arg(w)
                .arg(h)
                .arg(x)
                .arg(y)
                .arg(monitor);
        QPointer<App> safeApp(this);
        runWithOptionalConfirm(basePolicy, summary, true,
                               [safeApp, req, finishCaptureResult, includeImage, maxDim, fail] {
                                   if (!safeApp) {
                                       fail(App::tr("glimply is shutting down."));
                                       return;
                                   }
                                   safeApp->captureAsync(
                                       req, [finishCaptureResult, includeImage, maxDim](const CliCaptureResult &res) {
                                           finishCaptureResult(res, includeImage, maxDim);
                                       });
                               });
        return;
    }

    if (toolName == QLatin1String("capture_window")) {
        if (basePolicy.decision == McpDecision::Deny) {
            fail(basePolicy.reason);
            return;
        }
        const QString winId = args.value(QStringLiteral("window_id")).toString().trimmed();
        if (winId.isEmpty()) {
            fail(tr("'window_id' is required."));
            return;
        }
        std::optional<ImageFormat> fmt;
        std::optional<int> quality;
        QString optErr;
        if (!parseFormatAndQuality(&fmt, &quality, &optErr)) {
            fail(optErr);
            return;
        }
        const bool includeImage = args.value(QStringLiteral("include_image")).toBool(false);
        const int maxDim = std::max(0, args.value(QStringLiteral("max_dimension")).toInt(0));

        CliCaptureRequest req;
        req.target = QStringLiteral("window:") + winId;
        req.format = fmt;
        req.quality = quality;
        if (args.contains(QStringLiteral("include_cursor"))) {
            req.cursor = args.value(QStringLiteral("include_cursor")).toBool();
        }
        req.triggerKind = QStringLiteral("mcp");

        const QString summary = tr("An external MCP client wants to capture window \"%1\".").arg(winId);
        QPointer<App> safeApp(this);
        runWithOptionalConfirm(basePolicy, summary, true,
                               [safeApp, req, finishCaptureResult, includeImage, maxDim, fail] {
                                   if (!safeApp) {
                                       fail(App::tr("glimply is shutting down."));
                                       return;
                                   }
                                   safeApp->captureAsync(
                                       req, [finishCaptureResult, includeImage, maxDim](const CliCaptureResult &res) {
                                           finishCaptureResult(res, includeImage, maxDim);
                                       });
                               });
        return;
    }

    if (toolName == QLatin1String("get_last_capture")) {
        const McpPolicyResult policy = evaluateMcpAccess(store_.mcp, toolName, lastCaptureTargetId_);
        if (policy.decision == McpDecision::Deny) {
            fail(policy.reason);
            return;
        }
        if (!hasLastCapture()) {
            fail(tr("No capture has been recorded yet in this glimply session."));
            return;
        }
        const bool includeImage = args.value(QStringLiteral("include_image")).toBool(false);
        const int maxDim = std::max(0, args.value(QStringLiteral("max_dimension")).toInt(0));
        const QString label = !lastCaptureDisplayName_.isEmpty() ? lastCaptureDisplayName_ : lastPaths_.first();
        const QString summary = tr("An external MCP client wants to read the last capture (%1).").arg(label);
        QPointer<App> safeApp(this);
        runWithOptionalConfirm(policy, summary, false, [safeApp, includeImage, maxDim, callback, fail] {
            if (!safeApp) {
                fail(App::tr("glimply is shutting down."));
                return;
            }
            const QJsonObject meta = safeApp->lastCaptureInfo();
            std::optional<McpImagePayload> img;
            if (includeImage && !safeApp->lastPaths_.isEmpty()) {
                QString imgErr;
                img = buildMcpImagePayload(safeApp->lastPaths_.first(), maxDim, &imgErr);
                if (!img) {
                    fail(imgErr);
                    return;
                }
            }
            callback({true, meta, {}, std::move(img)});
        });
        return;
    }

    if (toolName == QLatin1String("start_interval")) {
        const QString query = args.value(QStringLiteral("target")).toString().trimmed();
        if (query.isEmpty()) {
            fail(tr("'target' is required."));
            return;
        }
        const CliResolveResult resolved = resolveTarget(query);
        const McpPolicyResult policy =
            evaluateMcpAccess(store_.mcp, toolName, resolved.ok ? resolved.targetId : query);
        if (policy.decision == McpDecision::Deny) {
            fail(policy.reason);
            return;
        }
        if (!resolved.ok) {
            fail(resolved.error);
            return;
        }
        IntervalConfig cfg = store_.targetIntervalConfig(resolved.targetId);
        if (args.contains(QStringLiteral("seconds"))) {
            cfg.seconds = std::clamp(args.value(QStringLiteral("seconds")).toInt(cfg.seconds), 1, 86400);
        }
        if (args.contains(QStringLiteral("max_captures"))) {
            cfg.maxCaptures = std::max(0, args.value(QStringLiteral("max_captures")).toInt(0));
        }
        if (args.contains(QStringLiteral("max_minutes"))) {
            cfg.maxMinutes = std::max(0, args.value(QStringLiteral("max_minutes")).toInt(0));
        }
        if (args.contains(QStringLiteral("only_when_changed"))) {
            cfg.onlyWhenChanged = args.value(QStringLiteral("only_when_changed")).toBool();
        }
        const QString summary =
            tr("An external MCP client wants to start interval captures every %1 s for \"%2\".")
                .arg(cfg.seconds)
                .arg(resolved.name);
        QPointer<App> safeApp(this);
        runWithOptionalConfirm(policy, summary, true, [safeApp, resolved, cfg, callback, fail] {
            if (!safeApp) {
                fail(App::tr("glimply is shutting down."));
                return;
            }
            const QString err = safeApp->startInterval(resolved.targetId, cfg);
            if (!err.isEmpty()) {
                fail(err);
                return;
            }
            callback({true,
                      QJsonObject{
                          {QStringLiteral("targetId"), resolved.targetId},
                          {QStringLiteral("displayName"), resolved.name},
                          {QStringLiteral("active"), true},
                          {QStringLiteral("seconds"), cfg.seconds},
                          {QStringLiteral("maxCaptures"), cfg.maxCaptures},
                          {QStringLiteral("maxMinutes"), cfg.maxMinutes},
                          {QStringLiteral("onlyWhenChanged"), cfg.onlyWhenChanged},
                      },
                      {},
                      std::nullopt});
        });
        return;
    }

    if (toolName == QLatin1String("stop_interval")) {
        const bool stopAll = args.value(QStringLiteral("all")).toBool(false);
        if (stopAll) {
            const McpPolicyResult policy = evaluateMcpAccess(store_.mcp, toolName, allMonitorsTargetId());
            if (policy.decision == McpDecision::Deny) {
                fail(policy.reason);
                return;
            }
            stopAllIntervals();
            callback({true, QJsonObject{{QStringLiteral("stoppedAll"), true}}, {}, std::nullopt});
            return;
        }
        const QString query = args.value(QStringLiteral("target")).toString().trimmed();
        if (query.isEmpty()) {
            fail(tr("Specify 'target' or set 'all': true."));
            return;
        }
        const CliResolveResult resolved = resolveTarget(query);
        const McpPolicyResult policy =
            evaluateMcpAccess(store_.mcp, toolName, resolved.ok ? resolved.targetId : query);
        if (policy.decision == McpDecision::Deny) {
            fail(policy.reason);
            return;
        }
        if (!resolved.ok) {
            fail(resolved.error);
            return;
        }
        const bool stopped = stopInterval(resolved.targetId);
        callback({true,
                  QJsonObject{
                      {QStringLiteral("targetId"), resolved.targetId},
                      {QStringLiteral("displayName"), resolved.name},
                      {QStringLiteral("stopped"), stopped},
                  },
                  {},
                  std::nullopt});
        return;
    }

    if (toolName == QLatin1String("annotate_image")) {
        const QString inputPath = args.value(QStringLiteral("input_path")).toString().trimmed();
        const QString outputPath = args.value(QStringLiteral("output_path")).toString().trimmed();
        const QJsonValue opsVal = args.value(QStringLiteral("operations"));
        if (inputPath.isEmpty() || !opsVal.isArray() || opsVal.toArray().isEmpty()) {
            fail(tr("annotate_image requires a non-empty input_path and operations array."));
            return;
        }
        std::optional<ImageFormat> fmt;
        std::optional<int> quality;
        QString optErr;
        if (!parseFormatAndQuality(&fmt, &quality, &optErr)) {
            fail(optErr);
            return;
        }
        if (basePolicy.decision == McpDecision::Deny) {
            fail(basePolicy.reason);
            return;
        }

        if (store_.mcp.accessMode == McpAccessMode::Allowlist) {
            const QString canonicalCaptureDir = QFileInfo(store_.effectiveCaptureDirectory()).canonicalFilePath();
            const auto isInsideCaptureDir = [&](const QString &candidate, bool existingFile) -> bool {
                if (canonicalCaptureDir.isEmpty()) {
                    return false;
                }
                const QFileInfo fi(candidate);
                const QString canon =
                    existingFile ? fi.canonicalFilePath() : QFileInfo(fi.absolutePath()).canonicalFilePath();
                if (canon.isEmpty()) {
                    return false;
                }
                if (canon == canonicalCaptureDir) {
                    return !existingFile;
                }
                const QString prefix = canonicalCaptureDir.endsWith(QLatin1Char('/'))
                                           ? canonicalCaptureDir
                                           : canonicalCaptureDir + QLatin1Char('/');
                return canon.startsWith(prefix);
            };
            if (!isInsideCaptureDir(inputPath, true)) {
                fail(tr("In MCP Allowlist mode, input_path must be an existing file inside glimply's capture "
                        "directory."));
                return;
            }
            if (!outputPath.isEmpty() && !isInsideCaptureDir(outputPath, false)) {
                fail(tr("In MCP Allowlist mode, output_path must be inside glimply's capture directory."));
                return;
            }
        }

        AnnotateRequest req;
        req.inputPath = inputPath;
        req.outputPath = outputPath;
        req.operations = opsVal.toArray();
        req.overwrite = args.value(QStringLiteral("overwrite")).toBool(false);
        req.exportOptions = store_.exportOptions;
        if (fmt) {
            req.exportOptions.format = *fmt;
            req.formatExplicit = true;
        }
        if (quality) {
            req.exportOptions.quality = *quality;
        }

        const QString summary =
            tr("An external MCP client wants to annotate \"%1\" (%2 operation(s)).")
                .arg(QFileInfo(inputPath).fileName())
                .arg(req.operations.size());
        QPointer<App> safeApp(this);
        runWithOptionalConfirm(basePolicy, summary, false, [safeApp, req, callback, fail] {
            if (!safeApp) {
                fail(App::tr("glimply is shutting down."));
                return;
            }
            auto *watcher = new QFutureWatcher<AnnotateResult>(safeApp.data());
            QObject::connect(watcher, &QFutureWatcher<AnnotateResult>::finished, safeApp.data(),
                             [watcher, callback, fail] {
                                 const AnnotateResult res = watcher->result();
                                 watcher->deleteLater();
                                 if (!res.ok) {
                                     fail(res.error);
                                     return;
                                 }
                                 callback({true,
                                           QJsonObject{
                                               {QStringLiteral("outputPath"), res.outputPath},
                                               {QStringLiteral("width"), res.width},
                                               {QStringLiteral("height"), res.height},
                                               {QStringLiteral("format"), res.format},
                                               {QStringLiteral("bytes"), res.bytes},
                                               {QStringLiteral("appliedCount"), res.appliedCount},
                                           },
                                           {},
                                           std::nullopt});
                             });
            watcher->setFuture(QtConcurrent::run([req] { return applyProgrammaticAnnotations(req); }));
        });
        return;
    }

    fail(tr("Unknown MCP tool: %1").arg(toolName));
}

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
    if (recorder_->isBusy()) {
        return tr("A recording is already running.");
    }
    QString error;
    if (!recordingTargetFor(targetId, &error)) {
        return error;
    }
    store_.lastRecordingTarget = targetId;
    saveSettings();
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
                qWarning("glimply: no native scale for %s: %s", qPrintable(connector), qPrintable(result.error));
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
    options.path = uniqueRecordingPath(
        directory, recordingFileName(store_.effectiveFilenameTemplate(), target->token, QDateTime::currentDateTime(),
                                     options.format));
    recorder_->start(*target, options); // reports its own errors through failed()
}

void App::stopRecording()
{
    recorder_->stop();
}

void App::setRecordingPaused(bool paused)
{
    recorder_->setPaused(paused);
}

void App::toggleRecording()
{
    if (recorder_->isBusy()) {
        recorder_->stop();
        return;
    }
    if (!store_.lastRecordingTarget.isEmpty() && recordingTargetFor(store_.lastRecordingTarget, nullptr)) {
        const QString error = startRecording(store_.lastRecordingTarget);
        if (!error.isEmpty()) {
            notifyError(tr("Recording failed"), {error});
        }
        return;
    }
    openOverlayForRecording();
}

void App::openOverlayForRecording()
{
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
    recorder_->stop();
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
    if (recorder_->isBusy()) {
        state.insert(QStringLiteral("target"), recorder_->target().targetId);
        state.insert(QStringLiteral("targetName"), recorder_->target().displayName);
        state.insert(QStringLiteral("path"), recorder_->path());
    }
    return state;
}

void App::syncRecordingUi()
{
    tray_->setRecordingState(int(recorder_->state()), recorder_->elapsedMs(), recorder_->target().displayName);
    Q_EMIT recordingChanged();
}

} // namespace glimply
