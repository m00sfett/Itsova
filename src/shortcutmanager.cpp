#include "shortcutmanager.h"

#include <KGlobalAccel>

#include <QAction>
#include <QSet>

namespace glimply
{

ShortcutManager::ShortcutManager(QObject *parent)
    : QObject(parent)
{
    connect(KGlobalAccel::self(), &KGlobalAccel::globalShortcutChanged, this,
            [this](QAction *action, const QKeySequence &) {
                if (actions_.value(action->objectName()) == action) {
                    Q_EMIT shortcutChanged(action->objectName());
                }
            });
}

void ShortcutManager::sync(const QList<Entry> &entries, const QStringList &deletedIds)
{
    QSet<QString> wanted;
    for (const Entry &entry : entries) {
        wanted.insert(entry.id);
        QAction *action = actions_.value(entry.id);
        if (!action) {
            action = new QAction(entry.text, this);
            action->setObjectName(entry.id);
            connect(action, &QAction::triggered, this, [this, id = entry.id] { Q_EMIT triggered(id); });
            actions_.insert(entry.id, action);
            // No defaults (decision: avoid clashes with Spectacle and the Print key). Autoloading
            // restores what the user assigned earlier, here or in System Settings.
            KGlobalAccel::self()->setDefaultShortcut(action, {});
            KGlobalAccel::self()->setShortcut(action, {}, KGlobalAccel::Autoloading);
        } else if (action->text() != entry.text) {
            action->setText(entry.text);
            // Re-register to update the friendly name shown in System Settings.
            KGlobalAccel::self()->setShortcut(action, KGlobalAccel::self()->shortcut(action), KGlobalAccel::NoAutoloading);
        }
    }

    for (auto it = actions_.begin(); it != actions_.end();) {
        if (wanted.contains(it.key())) {
            ++it;
            continue;
        }
        if (deletedIds.contains(it.key())) {
            KGlobalAccel::self()->removeAllShortcuts(it.value());
        }
        it.value()->deleteLater();
        it = actions_.erase(it);
    }
}

QKeySequence ShortcutManager::shortcut(const QString &id) const
{
    QAction *action = actions_.value(id);
    if (!action) {
        return {};
    }
    const QList<QKeySequence> sequences = KGlobalAccel::self()->shortcut(action);
    return sequences.isEmpty() ? QKeySequence() : sequences.first();
}

bool ShortcutManager::setShortcut(const QString &id, const QKeySequence &sequence)
{
    QAction *action = actions_.value(id);
    if (!action) {
        return false;
    }
    const QList<QKeySequence> sequences = sequence.isEmpty() ? QList<QKeySequence>() : QList<QKeySequence>{sequence};
    const bool ok = KGlobalAccel::self()->setShortcut(action, sequences, KGlobalAccel::NoAutoloading);
    Q_EMIT shortcutChanged(id);
    return ok;
}

} // namespace glimply
