#include "printkey.h"

#include <KConfig>
#include <KConfigGroup>
#include <KGlobalAccel>
#include <KGlobalShortcutInfo>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QKeySequence>

#include <algorithm>

namespace itsova::PrintKey
{
namespace
{

const QString groupName = QStringLiteral("PrintKey");
constexpr int callTimeoutMs = 2'000;

int printKey()
{
    return QKeySequence(Qt::Key_Print)[0].toCombined();
}

QDBusMessage accelCall(const QString &method, const QVariantList &arguments)
{
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.kde.kglobalaccel"),
                                                       QStringLiteral("/kglobalaccel"),
                                                       QStringLiteral("org.kde.KGlobalAccel"), method);
    call.setArguments(arguments);
    return call;
}

// Single-key shortcuts of an action, as KGlobalAccel stores them.
QList<int> keysOf(const QStringList &action)
{
    qDBusRegisterMetaType<QList<int>>();
    const QDBusReply<QList<int>> reply =
        QDBusConnection::sessionBus().call(accelCall(QStringLiteral("shortcut"), {action}), QDBus::Block, callTimeoutMs);
    return reply.isValid() ? reply.value() : QList<int>();
}

void setKeys(const QStringList &action, const QList<int> &keys)
{
    qDBusRegisterMetaType<QList<int>>();
    QDBusConnection::sessionBus().call(accelCall(QStringLiteral("setForeignShortcut"),
                                                 {action, QVariant::fromValue(keys)}),
                                       QDBus::Block, callTimeoutMs);
}

} // namespace

QString actionId()
{
    return QStringLiteral("print");
}

bool isClaimed(const KConfig &config)
{
    return config.group(groupName).readEntry("Claimed", false);
}

bool isDecided(const KConfig &config)
{
    return config.group(groupName).hasKey("Claimed");
}

QStringList claim(KConfig &config)
{
    const QKeySequence print(Qt::Key_Print);
    KConfigGroup group = config.group(groupName);
    // Owners from an earlier claim stay on record; they are the ones to restore.
    int index = group.readEntry("OwnerCount", 0);
    QStringList names;
    for (const KGlobalShortcutInfo &info : KGlobalAccel::globalShortcutsByKey(print)) {
        if (info.componentUniqueName() == QCoreApplication::applicationName()) {
            continue;
        }
        const QStringList action{info.componentUniqueName(), info.uniqueName(), info.componentFriendlyName(),
                                 info.friendlyName()};
        KConfigGroup owner = group.group(QStringLiteral("Owner%1").arg(index++));
        owner.writeEntry("Action", action);
        owner.writeEntry("Keys", keysOf(action));
        names.append(info.componentFriendlyName().isEmpty()
                         ? info.friendlyName()
                         : QStringLiteral("%1: %2").arg(info.componentFriendlyName(), info.friendlyName()));
    }
    KGlobalAccel::stealShortcutSystemwide(print);
    group.writeEntry("OwnerCount", index);
    group.writeEntry("Claimed", true);
    config.sync();
    return names;
}

void release(KConfig &config, bool clearOwnAction)
{
    KConfigGroup group = config.group(groupName);
    if (clearOwnAction) {
        const QString app = QCoreApplication::applicationName();
        setKeys({app, actionId(), app, QString()}, {});
    }
    const int print = printKey();
    const int count = group.readEntry("OwnerCount", 0);
    for (int i = 0; i < count; ++i) {
        const KConfigGroup owner = group.group(QStringLiteral("Owner%1").arg(i));
        const QStringList action = owner.readEntry("Action", QStringList());
        const QList<int> original = owner.readEntry("Keys", QList<int>());
        if (action.size() != 4) {
            continue;
        }
        QList<int> keys = keysOf(action);
        // Taking a key leaves an empty slot behind; drop it.
        keys.removeAll(0);
        if (keys.contains(print)) {
            continue;
        }
        // Put Print back at its old position, so the action looks exactly as before.
        const qsizetype position = std::clamp<qsizetype>(original.indexOf(print), 0, keys.size());
        keys.insert(original.contains(print) ? position : keys.size(), print);
        setKeys(action, keys);
    }
    for (int i = 0; i < count; ++i) {
        group.deleteGroup(QStringLiteral("Owner%1").arg(i));
    }
    group.writeEntry("OwnerCount", 0);
    group.writeEntry("Claimed", false);
    config.sync();
}

} // namespace itsova::PrintKey
