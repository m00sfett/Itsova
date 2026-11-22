#pragma once

#include <QHash>
#include <QKeySequence>
#include <QObject>
#include <QString>

class QAction;

namespace glimply
{

// Owns one global shortcut action per trigger (target or group) and registers it with KGlobalAccel.
// KGlobalAccel is the single source of truth for the key sequences, so shortcuts edited in Plasma's
// System Settings show up here and survive restarts.
class ShortcutManager : public QObject
{
    Q_OBJECT

public:
    explicit ShortcutManager(QObject *parent = nullptr);

    struct Entry
    {
        QString id;
        QString text; // shown in System Settings, e.g. "Capture region DP-1-1"
    };

    // Registers actions for all entries and drops actions that are no longer listed. Ids in
    // deletedIds also lose their stored shortcut (deleted regions and groups); other dropped ids
    // (for example disconnected monitors) keep theirs for later.
    void sync(const QList<Entry> &entries, const QStringList &deletedIds = {});

    [[nodiscard]] QKeySequence shortcut(const QString &id) const;
    bool setShortcut(const QString &id, const QKeySequence &sequence);

Q_SIGNALS:
    void triggered(const QString &id);
    void shortcutChanged(const QString &id);

private:
    QHash<QString, QAction *> actions_;
};

} // namespace glimply
