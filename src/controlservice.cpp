#include "controlservice.h"

#include "app.h"
#include "cli.h"
#include "intervalcontroller.h"
#include "screens.h"
#include "shortcutmanager.h"
#include "traycontroller.h"
#include "triggerengine.h"
#include "windowlist.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusServiceWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>

namespace glimply
{

ControlService::ControlService(App *app)
    : QObject(app)
    , app_(app)
    , widgetWatcher_(new QDBusServiceWatcher(this))
{
    widgetWatcher_->setConnection(QDBusConnection::sessionBus());
    widgetWatcher_->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(widgetWatcher_, &QDBusServiceWatcher::serviceUnregistered, this, [this](const QString &service) {
        widgetWatcher_->removeWatchedService(service);
        if (widgetWatcher_->watchedServices().isEmpty()) {
            Q_EMIT widgetActiveChanged(false);
        }
    });

    connect(app_, &App::targetsChanged, this, &ControlService::Changed);
    connect(app_, &App::intervalsChanged, this, &ControlService::IntervalsChanged);
    connect(app_->shortcuts(), &ShortcutManager::shortcutChanged, this, &ControlService::Changed);
    connect(app_, &App::captured, this, &ControlService::Captured);

    QDBusConnection::sessionBus().registerObject(QStringLiteral("/Control"), this,
                                                 QDBusConnection::ExportScriptableContents);
}

bool ControlService::widgetActive() const
{
    return !widgetWatcher_->watchedServices().isEmpty();
}

QString ControlService::Targets() const
{
    QJsonArray targets;
    for (const MenuTarget &target : app_->menuTargets()) {
        QString kind;
        switch (target.section) {
        case MenuTarget::Section::Group:
            kind = QStringLiteral("group");
            break;
        case MenuTarget::Section::Area:
            kind = QStringLiteral("area");
            break;
        case MenuTarget::Section::Monitor:
            kind = target.id == allMonitorsTargetId() ? QStringLiteral("all") : QStringLiteral("monitor");
            break;
        }
        // A short second line for the widget.
        QString detail = target.detail;
        const TargetStore &store = app_->store();
        if (const Area *area = store.area(target.id); area && detail.isEmpty()) {
            if (area->isCrossMonitor()) {
                QStringList names;
                names.reserve(area->monitors.size());
                for (const AreaMonitorBinding &binding : area->monitors) {
                    names.append(monitorName(store, binding.connector));
                }
                detail = tr("%1 · %2 × %3")
                             .arg(names.join(QStringLiteral(" + ")))
                             .arg(area->rect.width())
                             .arg(area->rect.height());
            } else {
                detail = tr("%1 · %2 × %3 at %4, %5")
                             .arg(monitorName(store, area->connector))
                             .arg(area->rect.width())
                             .arg(area->rect.height())
                             .arg(area->rect.x())
                             .arg(area->rect.y());
            }
        } else if (const Group *group = store.group(target.id)) {
            QStringList names;
            for (const QString &member : group->targetIds) {
                names.append(App::targetLabel(store, member));
            }
            detail = names.isEmpty() ? tr("No targets") : names.join(QStringLiteral(", "));
        } else if (kind == QLatin1String("monitor")) {
            const QString connector = connectorFromScreenTargetId(target.id);
            if (const QScreen *screen = findScreen(connector)) {
                const QString size = tr("%1 × %2").arg(screen->geometry().width()).arg(screen->geometry().height());
                // A renamed monitor still shows where it is plugged in.
                detail = target.name == connector ? size : tr("%1 · %2").arg(connector, size);
            }
        } else if (kind == QLatin1String("all")) {
            detail = tr("One image of every monitor");
        }
        const IntervalConfig intervalCfg = store.targetIntervalConfig(target.id);
        targets.append(QJsonObject{
            {QStringLiteral("targetId"), target.id},
            {QStringLiteral("kind"), kind},
            {QStringLiteral("name"), target.name},
            {QStringLiteral("detail"), detail},
            {QStringLiteral("shortcut"), target.shortcut.toString(QKeySequence::NativeText)},
            {QStringLiteral("available"), target.available},
            {QStringLiteral("intervalActive"), target.intervalActive},
            {QStringLiteral("intervalSeconds"), intervalCfg.seconds},
            {QStringLiteral("intervalMaxCaptures"), intervalCfg.maxCaptures},
            {QStringLiteral("intervalMaxMinutes"), intervalCfg.maxMinutes},
            {QStringLiteral("intervalOnlyWhenChanged"), intervalCfg.onlyWhenChanged},
        });
    }
    return QString::fromUtf8(QJsonDocument(targets).toJson(QJsonDocument::Compact));
}

bool ControlService::HasLastCapture() const
{
    return app_->hasLastCapture();
}

void ControlService::Trigger(const QString &id)
{
    app_->trigger(id);
}

void ControlService::AddArea()
{
    app_->addArea();
}

void ControlService::EditArea(const QString &id)
{
    app_->editArea(id);
}

QString ControlService::Rename(const QString &id, const QString &name)
{
    return app_->renameTarget(id, name);
}

void ControlService::Remove(const QString &id)
{
    app_->removeTarget(id);
}

void ControlService::OpenLastCapture()
{
    app_->openLastCapture();
}

void ControlService::EditLastCapture()
{
    app_->editLastCapture();
}

void ControlService::OpenCaptureFolder()
{
    app_->openCaptureFolder();
}

void ControlService::Configure(const QString &targetId)
{
    app_->showSettings(false, targetId);
}

int ControlService::DelaySeconds() const
{
    return app_->captureDelaySeconds();
}

bool ControlService::KeepDelayActive() const
{
    return app_->keepDelayActive();
}

bool ControlService::CountdownActive() const
{
    return app_->isCountdownActive();
}

void ControlService::SetDelay(int seconds, bool keepActive)
{
    app_->setCaptureDelay(seconds, keepActive);
}

void ControlService::CancelCountdown()
{
    app_->cancelCountdown();
}

void ControlService::PickColor()
{
    app_->startColorPicker();
}

QString ControlService::Intervals() const
{
    QJsonArray array;
    for (const ActiveIntervalStatus &status : app_->activeIntervals()) {
        array.append(QJsonObject{
            {QStringLiteral("targetId"), status.targetId},
            {QStringLiteral("displayName"), status.displayName},
            {QStringLiteral("seconds"), status.config.seconds},
            {QStringLiteral("maxCaptures"), status.config.maxCaptures},
            {QStringLiteral("maxMinutes"), status.config.maxMinutes},
            {QStringLiteral("onlyWhenChanged"), status.config.onlyWhenChanged},
            {QStringLiteral("captureCount"), status.captureCount},
            {QStringLiteral("skippedUnchanged"), status.skippedUnchanged},
            {QStringLiteral("startedAt"), status.startedAt.toString(Qt::ISODate)},
        });
    }
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QString ControlService::StartInterval(const QString &id, int seconds, int maxCaptures, int maxMinutes,
                                      bool onlyWhenChanged)
{
    if (seconds <= 0 && maxCaptures <= 0 && maxMinutes <= 0 && !onlyWhenChanged) {
        return app_->startInterval(id);
    }
    IntervalConfig cfg = app_->store().targetIntervalConfig(id);
    if (seconds > 0) {
        cfg.seconds = seconds;
    }
    if (maxCaptures >= 0) {
        cfg.maxCaptures = maxCaptures;
    }
    if (maxMinutes >= 0) {
        cfg.maxMinutes = maxMinutes;
    }
    cfg.onlyWhenChanged = onlyWhenChanged;
    return app_->startInterval(id, cfg);
}

void ControlService::StopInterval(const QString &id)
{
    app_->stopInterval(id);
}

void ControlService::StopAllIntervals()
{
    app_->stopAllIntervals();
}

QString ControlService::CaptureSync(const QString &requestJson)
{
    QJsonParseError parseErr{};
    const QJsonDocument doc = QJsonDocument::fromJson(requestJson.toUtf8(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        CliCaptureResult bad;
        bad.ok = false;
        bad.exitCode = 2;
        bad.error = tr("Invalid capture request JSON.");
        return QString::fromUtf8(QJsonDocument(captureResultToJson(bad)).toJson(QJsonDocument::Compact));
    }

    bool ok = false;
    const CliCaptureRequest request = captureRequestFromJson(doc.object(), &ok);
    if (!ok) {
        CliCaptureResult bad;
        bad.ok = false;
        bad.exitCode = 2;
        bad.error = tr("Capture request is missing a valid target or format.");
        return QString::fromUtf8(QJsonDocument(captureResultToJson(bad)).toJson(QJsonDocument::Compact));
    }

    if (!calledFromDBus()) {
        return {};
    }

    setDelayedReply(true);
    const QDBusMessage msg = message();
    const QDBusConnection bus = connection();
    app_->captureAsync(request, [msg, bus](const CliCaptureResult &result) {
        const QString json =
            QString::fromUtf8(QJsonDocument(captureResultToJson(result)).toJson(QJsonDocument::Compact));
        bus.send(msg.createReply(json));
    });
    return {};
}

QString ControlService::ListMonitors() const
{
    const TargetStore &store = app_->store();
    QJsonArray array;
    for (const ScreenInfo &screen : currentScreens()) {
        array.append(QJsonObject{
            {QStringLiteral("connector"), screen.connector},
            {QStringLiteral("name"), store.monitorName(screen)},
            {QStringLiteral("identity"), screen.identity()},
            {QStringLiteral("targetId"), screenTargetId(screen.connector)},
            {QStringLiteral("x"), screen.geometry.x()},
            {QStringLiteral("y"), screen.geometry.y()},
            {QStringLiteral("width"), screen.geometry.width()},
            {QStringLiteral("height"), screen.geometry.height()},
        });
    }
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QString ControlService::ListWindows() const
{
    QJsonArray array;
    for (const WindowInfo &win : listWindows(QCoreApplication::applicationPid())) {
        array.append(QJsonObject{
            {QStringLiteral("handle"), win.handle},
            {QStringLiteral("caption"), win.caption},
            {QStringLiteral("appName"), win.appName},
            {QStringLiteral("iconName"), win.iconName},
            {QStringLiteral("x"), win.geometry.x()},
            {QStringLiteral("y"), win.geometry.y()},
            {QStringLiteral("width"), win.geometry.width()},
            {QStringLiteral("height"), win.geometry.height()},
            {QStringLiteral("minimized"), win.minimized},
        });
    }
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QString ControlService::LastCaptureInfo() const
{
    return QString::fromUtf8(QJsonDocument(app_->lastCaptureInfo()).toJson(QJsonDocument::Compact));
}

QString ControlService::ResolveTarget(const QString &query) const
{
    const CliResolveResult res = app_->resolveTarget(query);
    return QString::fromUtf8(QJsonDocument(QJsonObject{
                                               {QStringLiteral("ok"), res.ok},
                                               {QStringLiteral("targetId"), res.targetId},
                                               {QStringLiteral("kind"), res.kind},
                                               {QStringLiteral("name"), res.name},
                                               {QStringLiteral("error"), res.error},
                                           })
                                 .toJson(QJsonDocument::Compact));
}

QString ControlService::AddAreaRect(const QString &name, const QString &connector, int x, int y, int width, int height)
{
    return QString::fromUtf8(
        QJsonDocument(app_->addAreaRect(name, connector, QRect(x, y, width, height))).toJson(QJsonDocument::Compact));
}

QString ControlService::OpenOverlay(const QString &mode, int delaySeconds)
{
    return app_->openOverlay(mode, delaySeconds);
}

void ControlService::RegisterWidget()
{
    if (!calledFromDBus()) {
        return;
    }
    const QString caller = message().service();
    if (!widgetWatcher_->watchedServices().contains(caller)) {
        const bool wasActive = widgetActive();
        widgetWatcher_->addWatchedService(caller);
        if (!wasActive) {
            Q_EMIT widgetActiveChanged(true);
        }
    }
}

} // namespace glimply
