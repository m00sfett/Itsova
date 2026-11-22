#include "app.h"

#include "screens.h"
#include "shortcutmanager.h"
#include "traycontroller.h"
#include "triggerengine.h"

#include <KConfig>
#include <KNotification>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDesktopServices>
#include <QDir>
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
    connect(tray_, &TrayController::addRegionRequested, this, [this] { showSettings(true); });

    // New or removed monitors change the built-in targets.
    const auto layoutChanged = [this] {
        syncShortcuts();
        Q_EMIT targetsChanged();
    };
    connect(qGuiApp, &QGuiApplication::screenAdded, this, layoutChanged);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, layoutChanged);

    syncShortcuts();
    applyAutostart();
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
    if (const Region *region = store.region(id)) {
        return region->name;
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
    for (const Region &region : std::as_const(store_.regions)) {
        entries.append({region.id, tr("Capture region %1").arg(region.name)});
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
    for (const Region &region : store_.regions) {
        const bool present = connected(region.connector);
        targets.append({MenuTarget::Section::Region, region.id, region.name,
                        present ? QString() : tr("%1 not connected").arg(region.connector),
                        shortcuts_->shortcut(region.id), present});
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
        KNotification::event(KNotification::Warning, tr("Check region monitor"), result.warnings.join(QLatin1Char('\n')));
    }
}

void App::notifyError(const QString &title, const QStringList &messages)
{
    KNotification::event(KNotification::Error, title, messages.join(QLatin1Char('\n')));
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

void App::showSettings(bool addRegion)
{
    Q_UNUSED(addRegion)
    // Milestone 5 adds the settings window.
}

void App::applyAutostart()
{
    // Milestone 6 adds autostart handling.
}

} // namespace glimply
