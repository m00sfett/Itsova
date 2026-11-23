#include "controlservice.h"

#include "app.h"
#include "screens.h"
#include "shortcutmanager.h"
#include "traycontroller.h"
#include "triggerengine.h"

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
            detail = tr("%1 · %2 × %3 at %4, %5")
                         .arg(monitorName(store, area->connector))
                         .arg(area->rect.width())
                         .arg(area->rect.height())
                         .arg(area->rect.x())
                         .arg(area->rect.y());
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
        targets.append(QJsonObject{
            {QStringLiteral("targetId"), target.id},
            {QStringLiteral("kind"), kind},
            {QStringLiteral("name"), target.name},
            {QStringLiteral("detail"), detail},
            {QStringLiteral("shortcut"), target.shortcut.toString(QKeySequence::NativeText)},
            {QStringLiteral("available"), target.available},
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

void ControlService::OpenCaptureFolder()
{
    app_->openCaptureFolder();
}

void ControlService::Configure(const QString &targetId)
{
    app_->showSettings(false, targetId);
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
