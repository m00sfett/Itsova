#include "app.h"

#include "annotations/annotationdocument.h"
#include "areaselector.h"
#include "captureoverlay.h"
#include "cli.h"
#include "colorpicker.h"
#include "countdowntimer.h"
#include "hookrunner.h"
#include "intervalcontroller.h"
#include "printkey.h"
#include "quickeditorwindow.h"
#include "controlservice.h"

#include <algorithm>
#include <mutex>
#include <utility>
#include "imageoutput.h"
#include "screens.h"
#include "settingswindow.h"
#include "shortcutmanager.h"
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
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QImageReader>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QLocale>
#include <QSaveFile>
#include <QScreen>
#include <QStandardPaths>
#include <QTemporaryFile>
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
              engine_->runIntervalTick(std::move(plan), dir, tmpl, std::move(prev), std::move(done));
          },
          this))
    , hooks_(new HookRunner(this))
{
    store_.load(*config_);
    tray_->setDelayState(store_.captureDelaySeconds, store_.keepDelayActive);

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
        recordCapture(result.writtenPaths, result.displayName, targetId);
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
    auto *session = new CaptureSession(this, CaptureSession::Purpose::Print);
    session->setForcedFrozen(forcedFrozen);
    session->setForceShowOverlay(forceOverlay);
    session_ = session;
    connect(session, &CaptureSession::finished, session, &QObject::deleteLater);
    session->start();
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
    session->start();
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
    picker->start();
}

void App::recordCapture(const QStringList &paths, const QString &displayName, const QString &targetId)
{
    if (paths.isEmpty()) {
        return;
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
    engine_->run(std::move(plan), store_.effectiveCaptureDirectory(), store_.effectiveFilenameTemplate());
}

void App::onTriggerFinished(const TriggerResult &result)
{
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
                   || eventId == QLatin1String("colorPicked") || eventId == QLatin1String("intervalStopped")) {
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
                recordCapture({savedPath}, displayName);
                const std::optional<ImageFormat> fmt = imageFormatFromPath(savedPath);
                runPostCaptureHook({}, displayName, {savedPath}, QDateTime::currentDateTime(),
                                   fmt ? formatConfigKey(*fmt) : QStringLiteral("png"), QStringLiteral("editor"));
            });
    connect(window, &QuickEditorWindow::settingsModified, this, &App::saveSettings);
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
                    WorkerOutput out = watcher->result();
                    watcher->deleteLater();
                    if (out.cliResult.ok) {
                        if (request.clipboard && !out.clipboardImage.isNull()) {
                            QGuiApplication::clipboard()->setImage(out.clipboardImage);
                        }
                        if (!out.writtenPaths.isEmpty()) {
                            recordCapture(out.writtenPaths, out.cliResult.displayName, resolvedTargetId);
                            const QString fmt = out.cliResult.files.isEmpty()
                                                    ? formatConfigKey(store_.effectiveExportOptions(resolvedTargetId).format)
                                                    : out.cliResult.files.first().format;
                            const QString groupId = store_.group(resolvedTargetId) ? resolvedTargetId : QString();
                            runPostCaptureHook(resolvedTargetId, out.cliResult.displayName, out.writtenPaths,
                                               QDateTime::currentDateTime(), fmt, QStringLiteral("cli"), false, groupId);
                        } else {
                            tray_->blink();
                            Q_EMIT captured(true);
                        }
                    } else {
                        Q_EMIT captured(false);
                    }
                    if (callback) {
                        callback(out.cliResult);
                    }
                });

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
                                            formatConfigKey(rendered.exportOptions.format), QFileInfo(savedPath).size()});
                if (request.toStdout && out.cliResult.stdoutFile.isEmpty()) {
                    out.cliResult.stdoutFile = savedPath;
                    out.cliResult.tempStdoutFile = false;
                }
            }

            out.cliResult.ok = true;
            out.cliResult.exitCode = 0;
            return out;
        }));
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

} // namespace glimply
