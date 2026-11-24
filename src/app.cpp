#include "app.h"

#include "annotations/annotationdocument.h"
#include "areaselector.h"
#include "captureoverlay.h"
#include "colorpicker.h"
#include "countdowntimer.h"
#include "intervalcontroller.h"
#include "printkey.h"
#include "quickeditorwindow.h"
#include "controlservice.h"

#include <algorithm>
#include <utility>
#include "imageoutput.h"
#include "screens.h"
#include "settingswindow.h"
#include "shortcutmanager.h"
#include "traycontroller.h"
#include "triggerengine.h"
#include "triggerplan.h"

#include <KConfig>
#include <KGlobalAccel>
#include <KNotification>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QInputDialog>
#include <QLineEdit>
#include <QSaveFile>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QLocale>
#include <QScreen>
#include <QUrl>

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
{
    store_.load(*config_);
    tray_->setDelayState(store_.captureDelaySeconds, store_.keepDelayActive);

    connect(intervals_, &IntervalController::changed, this, [this] {
        tray_->setActiveIntervals(intervals_->activeIntervals());
        Q_EMIT intervalsChanged();
        Q_EMIT targetsChanged();
    });
    connect(intervals_, &IntervalController::captureSaved, this, [this](const QString &, const TriggerResult &result) {
        recordCapture(result.writtenPaths, result.displayName);
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

void App::startPrintNow()
{
    if (auto *session = session_.data()) {
        session->printPressed();
        return;
    }
    auto *session = new CaptureSession(this, CaptureSession::Purpose::Print);
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

void App::recordCapture(const QStringList &paths, const QString &displayName)
{
    if (paths.isEmpty()) {
        return;
    }
    lastPaths_ = paths;
    Q_EMIT captured(true);
    // Every frame is in memory by now, so the blink cannot end up in a capture (A2).
    tray_->blink();
    tray_->setLastCaptureAvailable(true);
    tray_->setStatusText(tr("Last capture: %1 at %2")
                             .arg(displayName, QLocale().toString(QDateTime::currentDateTime().time(),
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
        recordCapture(result.writtenPaths, result.displayName);
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
