#include "app.h"

#include "areaselector.h"
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
#include <QFile>
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
    connect(tray_, &TrayController::addAreaRequested, this, [this] { showSettings(true); });

    // New or removed monitors change the built-in targets.
    const auto layoutChanged = [this] {
        syncShortcuts();
        Q_EMIT targetsChanged();
    };
    connect(qGuiApp, &QGuiApplication::screenAdded, this, layoutChanged);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, layoutChanged);

    syncShortcuts();
    migrateLegacyIds();
    applyAutostart();
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

QString App::targetLabel(const TargetStore &store, const QString &id)
{
    if (id == allMonitorsTargetId()) {
        return tr("All monitors");
    }
    if (isScreenTargetId(id)) {
        return connectorFromScreenTargetId(id);
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
    for (const Group &group : std::as_const(store_.groups)) {
        entries.append({group.id, tr("Capture group %1").arg(group.name)});
    }
    for (const Area &area : std::as_const(store_.areas)) {
        entries.append({area.id, tr("Capture area %1").arg(area.name)});
    }
    for (const ScreenInfo &screen : currentScreens()) {
        entries.append({screenTargetId(screen.connector), tr("Capture monitor %1").arg(screen.connector)});
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
        targets.append({MenuTarget::Section::Monitor, id, screen.connector, {}, shortcuts_->shortcut(id), true});
    }
    targets.append({MenuTarget::Section::Monitor, allMonitorsTargetId(), tr("All monitors"), {},
                    shortcuts_->shortcut(allMonitorsTargetId()), !screens.isEmpty()});
    return targets;
}

void App::trigger(const QString &id)
{
    TriggerPlan plan = planTrigger(store_, id, currentScreens());
    if (plan.outputs.isEmpty()) {
        notifyError(tr("Nothing captured"), plan.errors);
        return;
    }
    engine_->run(std::move(plan), store_.effectiveCaptureDirectory(), store_.effectiveFilenameTemplate());
}

void App::onTriggerFinished(const TriggerResult &result)
{
    if (!result.writtenPaths.isEmpty()) {
        // Every frame is in memory by now, so the blink cannot end up in a capture (A2).
        tray_->blink();
        tray_->setLastCaptureAvailable(true);
        tray_->setStatusText(tr("Last capture: %1 at %2")
                                 .arg(result.displayName,
                                      QLocale().toString(result.timestamp.time(), QLocale::ShortFormat)));
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
    const QStringList paths = engine_->lastPaths();
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

void App::showSettings(bool addArea)
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
    window->show();
    window->raise();
    window->activateWindow();
}

void App::editArea(const QString &areaId)
{
    const Area *area = store_.area(areaId);
    if (!area || selector_) {
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
