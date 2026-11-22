#include "windowlist.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QSet>
#include <QVariantMap>

namespace glimply
{
namespace
{

constexpr int callTimeoutMs = 1'000;

// KWin's window runner lists every open window; its match ids are "0_" followed by the UUID.
QStringList windowHandles()
{
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"), QStringLiteral("/WindowsRunner"),
                                                       QStringLiteral("org.kde.krunner1"), QStringLiteral("Match"));
    call.setArguments({QString()});
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, callTimeoutMs);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        return {};
    }
    QStringList handles;
    QSet<QString> seen;
    const QDBusArgument matches = reply.arguments().first().value<QDBusArgument>();
    matches.beginArray();
    while (!matches.atEnd()) {
        // (sssuda{sv}): id, text, icon, type, relevance, properties
        QString id;
        QString text;
        QString icon;
        uint type = 0;
        double relevance = 0;
        QVariantMap properties;
        matches.beginStructure();
        matches >> id >> text >> icon >> type >> relevance >> properties;
        matches.endStructure();
        const qsizetype separator = id.indexOf(QLatin1Char('_'));
        const QString handle = separator < 0 ? id : id.mid(separator + 1);
        if (!handle.isEmpty() && !seen.contains(handle)) {
            seen.insert(handle);
            handles.append(handle);
        }
    }
    matches.endArray();
    return handles;
}

QVariantMap windowInfo(const QString &handle)
{
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"), QStringLiteral("/KWin"),
                                                       QStringLiteral("org.kde.KWin"), QStringLiteral("getWindowInfo"));
    call.setArguments({handle});
    const QDBusReply<QVariantMap> reply = QDBusConnection::sessionBus().call(call, QDBus::Block, callTimeoutMs);
    return reply.isValid() ? reply.value() : QVariantMap();
}

} // namespace

QList<WindowInfo> listWindows(qint64 excludedPid)
{
    QList<WindowInfo> windows;
    for (const QString &handle : windowHandles()) {
        const QVariantMap info = windowInfo(handle);
        if (info.isEmpty() || info.value(QStringLiteral("skipTaskbar")).toBool()
            || info.value(QStringLiteral("excludeFromCapture")).toBool()
            || info.value(QStringLiteral("pid")).toLongLong() == excludedPid) {
            continue;
        }
        WindowInfo window;
        window.handle = handle;
        window.caption = info.value(QStringLiteral("caption")).toString();
        window.appName = info.value(QStringLiteral("resourceName")).toString();
        const QString desktopFile = info.value(QStringLiteral("desktopFile")).toString();
        window.iconName = desktopFile.isEmpty() ? info.value(QStringLiteral("resourceClass")).toString() : desktopFile;
        window.geometry = QRectF(info.value(QStringLiteral("x")).toDouble(), info.value(QStringLiteral("y")).toDouble(),
                                 info.value(QStringLiteral("width")).toDouble(),
                                 info.value(QStringLiteral("height")).toDouble())
                              .toAlignedRect();
        window.minimized = info.value(QStringLiteral("minimized")).toBool();
        if (window.geometry.isEmpty()) {
            continue;
        }
        windows.append(window);
    }
    return windows;
}

QString windowToken(const WindowInfo &window)
{
    QString name = window.appName;
    // Reverse-DNS names such as org.kde.konsole read best by their last part.
    if (const qsizetype dot = name.lastIndexOf(QLatin1Char('.')); dot >= 0) {
        name = name.mid(dot + 1);
    }
    return name.isEmpty() ? QStringLiteral("window") : QStringLiteral("window-") + name.toLower();
}

} // namespace glimply
