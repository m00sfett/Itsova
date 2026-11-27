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
constexpr int recordingHintMs = 1500;
constexpr int recordingSavedHintMs = 4000;
} // namespace

App::App(QObject *parent)
    : QObject(parent)
    , config_(KSharedConfig::openConfig(QStringLiteral("itsovarc"), KConfig::SimpleConfig))
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
    setVerboseLogging(store_.verboseLogging);
    qCInfo(lcApp) << "itsova" << ITSOVA_VERSION << "started";
    tray_->setDelayState(store_.captureDelaySeconds, store_.keepDelayActive);
    tray_->setVisibilityMode(int(store_.trayVisibility));

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
        qWarning("itsova: ignoring unreadable capture history %s: %s", qPrintable(history_->filePath()),
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
    tray_->setRecordingPauseAvailable(ScreenRecorder::canPause());
    recordingTicker_ = new QTimer(this);
    recordingTicker_->setInterval(1000);
    connect(recordingTicker_, &QTimer::timeout, this, &App::syncRecordingUi);
    connect(recorder_, &ScreenRecorder::stateChanged, this, [this](ScreenRecorder::State state) {
        if (recorder_->isBusy()) {
            recordingTicker_->start();
        } else {
            recordingTicker_->stop();
        }
        // #125: pause and resume say so; the start hint went out with the start request.
        if (state == ScreenRecorder::State::Paused) {
            showRecordingHint(QStringLiteral("media-playback-pause"), tr("Recording paused"),
                              recorder_->target().displayName, recordingHintMs);
        } else if (state == ScreenRecorder::State::Recording && previousRecorderState_ == int(ScreenRecorder::State::Paused)
                   && !std::exchange(resumeHintShown_, false)) {
            showRecordingHint(QStringLiteral("media-record"), tr("Recording resumed"), recordingStopInstruction(),
                              recordingHintMs);
        }
        previousRecorderState_ = int(state);
        syncRecordingUi();
    });
    connect(recorder_, &ScreenRecorder::finished, this,
            [this](const QString &path, const RecordingTarget &target, qint64) {
                const QDateTime now = QDateTime::currentDateTime();
                recordCapture({path}, target.displayName, target.targetId, QStringLiteral("record"));
                if (store_.recordingHints) {
                    // #125: the stop hint names the file; a click opens it.
                    showRecordingHint(QStringLiteral("document-save"), tr("Recording saved"),
                                      QFileInfo(path).fileName(), recordingSavedHintMs,
                                      [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });
                } else {
                    notifySaved(tr("Recording saved"), {path}, {});
                }
                runPostCaptureHook(target.targetId, target.displayName, {path}, now,
                                   recordingFormatKey(store_.recordingFormat), QStringLiteral("record"));
            });
    connect(recorder_, &ScreenRecorder::interrupted, this, [this](const QString &reason) {
        qCWarning(lcRecord) << "recording ended early:" << reason;
        notify(QStringLiteral("recordingInterrupted"), tr("Recording ended early"),
               {reason, tr("What was recorded until then is saved.")});
    });
    connect(recorder_, &ScreenRecorder::failed, this, [this](const QString &error) {
        showRecordingHint(QStringLiteral("dialog-error"), tr("Recording failed"), error, recordingHintMs);
        notifyError(tr("Recording failed"), {error});
    });
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
    // First launch without an existing config: ask before taking over the Print key (#137).
    // Existing users with a saved config keep their previous behavior without being prompted again.
    if (shouldPromptForPrintKey(*config_, store_.hasExistingConfig)) {
        QTimer::singleShot(0, this, &App::promptPrintKeyOnFirstLaunch);
    } else if (store_.usePrintKey && !PrintKey::isDecided(*config_)) {
        claimPrintKey();
    }
}

void App::claimPrintKey()
{
    PrintKey::claim(*config_);
    shortcuts_->setShortcut(PrintKey::actionId(), QKeySequence(Qt::Key_Print));
}

void App::promptPrintKeyOnFirstLaunch()
{
    if (!shouldPromptForPrintKey(*config_, store_.hasExistingConfig)) {
        return;
    }
    QMessageBox box(QMessageBox::Question, tr("Use the Print key with Itsova?"),
                    tr("Should Itsova take over the global Print key shortcut from its current application "
                       "(usually Spectacle)?"));
    box.setInformativeText(tr("You can change this anytime in Settings → General. Before uninstalling Itsova, "
                              "turn it off there or run \"itsova --release-print-key\" to restore your previous "
                              "shortcut."));
    QPushButton *takeOverButton = box.addButton(tr("Take over Print key"), QMessageBox::AcceptRole);
    box.addButton(tr("Keep current shortcut"), QMessageBox::RejectRole);
    box.setDefaultButton(takeOverButton);
    box.exec();
    if (box.clickedButton() == takeOverButton) {
        setUsePrintKey(true);
    } else {
        store_.usePrintKey = false;
        markPrintKeyDeclined(*config_);
        storeChanged();
    }
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

void App::setVerboseFromCommandLine(bool on)
{
    verboseFromCommandLine_ = on;
    setVerboseLogging(store_.verboseLogging || verboseFromCommandLine_);
}

void App::storeChanged(const QStringList &deletedIds)
{
    store_.save(*config_);
    setVerboseLogging(store_.verboseLogging || verboseFromCommandLine_);
    tray_->setVisibilityMode(int(store_.trayVisibility));
    if (history_->setLimit(store_.historyLimit)) {
        historyModified(true);
    }
    syncShortcuts(deletedIds);
    applyAutostart();
    Q_EMIT targetsChanged();
}

void App::saveSettings()
{
    qCDebug(lcSettings) << "settings saved";
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
    if (ScreenRecorder::canPause()) {
        entries.append({recordPauseActionId(), tr("Pause or resume screen recording")});
    }
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
    qCInfo(lcOverlay) << "Print overlay requested, mode"
                      << (!forcedFrozen ? "default" : *forcedFrozen ? "frozen" : "live") << "force overlay"
                      << forceOverlay;
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
    qCInfo(lcCapture) << "interval start" << id << cfg.seconds << "s";
    const QString error = intervals_->start(id, targetLabel(store_, id), cfg);
    if (!error.isEmpty()) {
        notifyError(tr("Could not start interval"), {error});
    }
    return error;
}

bool App::stopInterval(const QString &id)
{
    qCInfo(lcCapture) << "interval stop" << id;
    return intervals_->stop(id);
}

void App::stopAllIntervals()
{
    qCInfo(lcCapture) << "interval stop all";
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
    qCInfo(lcCapture) << "saved" << triggerKind << targetId << paths;
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
    const bool haveEvents = haveNotifyEvents();
    auto *notification = new KNotification(haveEvents ? QStringLiteral("captureSaved") : QStringLiteral("notification"));
    if (haveEvents) {
        notification->setComponentName(notifyComponentName());
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
    qCInfo(lcApp) << "trigger" << id;
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
        toggleRecording(QStringLiteral("shortcut"));
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
    // Installed builds ship io.github.m00sfett.itsova.notifyrc, so notifications carry itsova's name and can be
    // configured in System Settings. Uninstalled development builds fall back to the system events.
    const bool haveEvents = haveNotifyEvents();
    if (haveEvents) {
        KNotification::event(eventId, title, text, notifyComponentName());
    } else {
        KNotification::StandardEvent fallback = KNotification::Error;
        if (eventId == QLatin1String("monitorChanged") || eventId == QLatin1String("recordingInterrupted")) {
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
    qCWarning(lcApp).noquote() << title << ":" << messages.join(QStringLiteral(" | "));
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
        QStringLiteral("/bin/sh"), {QStringLiteral("-c"), script, QStringLiteral("itsova-restart"),
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
        qWarning("itsova: could not save the capture history: %s", qPrintable(error));
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
                                const QString &baseToken, const QString &deferredPrintPath,
                                bool deferredClearClipboard)
{
    auto *window = new QuickEditorWindow(store_, image, scale, std::move(annotations), baseToken);
    if (!window->isValid()) {
        notifyError(tr("Could not open image"), {window->loadError()});
        delete window;
        return;
    }
    attachEditorWindow(window, deferredPrintPath, deferredClearClipboard);
}

void App::attachEditorWindow(QuickEditorWindow *window, const QString &deferredPrintPath, bool deferredClearClipboard)
{
    window->setAttribute(Qt::WA_DeleteOnClose);
    auto pendingPrint = std::make_shared<QString>(deferredPrintPath);
    connect(window, &QuickEditorWindow::savedToFile, this,
            [this, pendingPrint, deferredClearClipboard](const QString &savedPath, const QString &displayName) {
                if (shouldPurgeDeferredPrintOnEditorSave(*pendingPrint, savedPath)) {
                    const QString oldPrint = *pendingPrint;
                    pendingPrint->clear();
                    purgeUnredactedPrintFile(oldPrint, deferredClearClipboard);
                    forgetCapturePath(oldPrint);
                }
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

} // namespace itsova
