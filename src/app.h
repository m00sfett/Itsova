#pragma once

#include "targets.h"

#include <KSharedConfig>

#include <QObject>
#include <QPointer>
#include <QStringList>

namespace glimply
{

class ShortcutManager;
class TrayController;
class TriggerEngine;
struct MenuTarget;
struct TriggerResult;

// Wires the tray, global shortcuts, the trigger engine and the settings together.
class App : public QObject
{
    Q_OBJECT

public:
    explicit App(QObject *parent = nullptr);
    ~App() override;

    [[nodiscard]] TargetStore &store();
    [[nodiscard]] ShortcutManager *shortcuts() const;

    // Persists the store and refreshes shortcuts and menus. Pass ids of removed regions/groups so
    // their global shortcuts are cleared as well.
    void storeChanged(const QStringList &deletedIds = {});

    void trigger(const QString &id);
    void openLastCapture();
    void openCaptureFolder();
    void showSettings(bool addRegion = false);

    [[nodiscard]] static QString targetLabel(const TargetStore &store, const QString &id);

Q_SIGNALS:
    void targetsChanged();

private:
    void onTriggerFinished(const TriggerResult &result);
    void syncShortcuts(const QStringList &deletedIds = {});
    void applyAutostart();
    [[nodiscard]] QList<MenuTarget> menuTargets() const;
    void notifyError(const QString &title, const QStringList &messages);

    KSharedConfigPtr config_;
    TargetStore store_;
    ShortcutManager *shortcuts_;
    TriggerEngine *engine_;
    TrayController *tray_;
    QPointer<QObject> settings_;
};

} // namespace glimply
