#include "app.h"

#include "areaselector.h"
#include "captureoverlay.h"
#include "printkey.h"
#include "controlservice.h"
#include "imageoutput.h"
#include "screens.h"
#include "settingswindow.h"
#include "shortcutmanager.h"
#include "traycontroller.h"
#include "triggerengine.h"

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
{
    store_.load(*config_);

    connect(shortcuts_, &ShortcutManager::triggered, this, &App::trigger);
    connect(engine_, &TriggerEngine::finished, this, &App::onTriggerFinished);
    connect(tray_, &TrayController::triggerRequested, this, &App::trigger);
    connect(tray_, &TrayController::openLastCaptureRequested, this, &App::openLastCapture);
    connect(tray_, &TrayController::openFolderRequested, this, &App::openCaptureFolder);
    connect(tray_, &TrayController::configureRequested, this, [this] { showSettings(); });
    connect(tray_, &TrayController::addAreaRequested, this, &App::addArea);
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

App::~App() = default;

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
    if (id == allMonitorsTargetId()) {
        return tr("All monitors");
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
    }
    for (const Area &area : std::as_const(store_.areas)) {
        entries.append({area.id, tr("Capture area %1").arg(area.name)});
    }
    for (const ScreenInfo &screen : currentScreens()) {
        entries.append({screenTargetId(screen.connector), tr("Capture monitor %1").arg(store_.monitorName(screen))});
    }
    entries.append({allMonitorsTargetId(), tr("Capture all monitors")});
    shortcuts_->sync(entries, deletedIds);
}

QList<MenuTarget> App::menuTargets() const
{
    const QList<ScreenInfo> screens = currentScreens();
    const auto connected = [&](const QString &connector) {
        return std::any_of(screens.cbegin(), screens.cend(),
                           [&](const ScreenInfo &s) { return s.connector == connector; });
    };

    QList<MenuTarget> targets;
    for (const Group &group : store_.groups) {
        targets.append({MenuTarget::Section::Group, group.id, group.name, {}, shortcuts_->shortcut(group.id),
                        !group.targetIds.isEmpty()});
    }
    for (const Area &area : store_.areas) {
        const bool present = connected(area.connector);
        targets.append({MenuTarget::Section::Area, area.id, area.name,
                        present ? QString() : tr("%1 not connected").arg(area.connector),
                        shortcuts_->shortcut(area.id), present});
    }
    for (const ScreenInfo &screen : screens) {
        const QString id = screenTargetId(screen.connector);
        targets.append({MenuTarget::Section::Monitor, id, store_.monitorName(screen), {}, shortcuts_->shortcut(id),
                        true});
    }
    targets.append({MenuTarget::Section::Monitor, allMonitorsTargetId(), tr("All monitors"), {},
                    shortcuts_->shortcut(allMonitorsTargetId()), !screens.isEmpty()});
    return targets;
}

void App::startPrint()
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

void App::addArea()
{
    if (session_ || selector_) {
        return;
    }
    auto *session = new CaptureSession(this, CaptureSession::Purpose::DefineArea);
    session_ = session;
    connect(session, &CaptureSession::finished, session, &QObject::deleteLater);
    session->start();
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
    if (id == PrintKey::actionId()) {
        startPrint();
        return;
    }
    TriggerPlan plan = planTrigger(store_, id, currentScreens());
    if (plan.outputs.isEmpty()) {
        notifyError(tr("Nothing captured"), plan.errors);
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
        KNotification::event(eventId == QLatin1String("monitorChanged") ? KNotification::Warning : KNotification::Error,
                             title, text);
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
    store_.remove(id);
    storeChanged({id});
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
    auto *selector = new AreaSelector(this);
    selector_ = selector;
    connect(selector, &AreaSelector::selected, this, [this, selector, areaId](const QString &connector, const QRect &rect) {
        selector->deleteLater();
        Area *edited = store_.area(areaId);
        if (!edited) {
            return;
        }
        edited->rect = rect;
        if (const QScreen *screen = findScreen(connector)) {
            edited->monitorIdentity = screenInfo(screen).identity();
        }
        storeChanged();
    });
    connect(selector, &AreaSelector::cancelled, selector, &QObject::deleteLater);
    selector->start(area->connector, area->rect);
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
