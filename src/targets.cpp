#include "targets.h"

#include <KConfig>
#include <KConfigGroup>

#include <QDir>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>

namespace glimply
{
namespace
{

constexpr auto screenPrefix = "screen:";
constexpr auto areaGroupPrefix = "Area ";
// Before 0.1.7 areas were called regions; such configurations are migrated on load.
constexpr auto legacyAreaGroupPrefix = "Region ";
constexpr auto legacyAreaIdPrefix = "region:";
constexpr auto areaIdPrefix = "area:";

QString migrateId(const QString &id)
{
    return id.startsWith(QLatin1String(legacyAreaIdPrefix))
        ? QLatin1String(areaIdPrefix) + id.mid(int(qstrlen(legacyAreaIdPrefix)))
        : id;
}
constexpr auto groupGroupPrefix = "Group ";

QString newId(const char *prefix)
{
    return QLatin1String(prefix) + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

} // namespace

QString ScreenInfo::identity() const
{
    QStringList parts;
    for (const QString &part : {manufacturer, model, serialNumber}) {
        if (!part.isEmpty()) {
            parts.append(part);
        }
    }
    return parts.join(QLatin1Char(' '));
}

QString screenTargetId(const QString &connector)
{
    return QLatin1String(screenPrefix) + connector;
}

QString allMonitorsTargetId()
{
    return QStringLiteral("all");
}

bool isScreenTargetId(const QString &id)
{
    return id.startsWith(QLatin1String(screenPrefix));
}

QString connectorFromScreenTargetId(const QString &id)
{
    return isScreenTargetId(id) ? id.mid(int(qstrlen(screenPrefix))) : QString();
}

QString TargetStore::defaultCaptureDirectory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).filePath(QStringLiteral("glimply"));
}

QString TargetStore::defaultFilenameTemplate()
{
    return QStringLiteral("{date}_{time}_{area}.png");
}

bool TargetStore::overlayStartsFrozen() const
{
    switch (overlayStart) {
    case OverlayStart::Frozen:
        return true;
    case OverlayStart::Live:
        return false;
    case OverlayStart::LastUsed:
        break;
    }
    return lastOverlayFrozen;
}

QString TargetStore::effectiveCaptureDirectory() const
{
    return captureDirectory.isEmpty() ? defaultCaptureDirectory() : captureDirectory;
}

QString TargetStore::effectiveFilenameTemplate() const
{
    return filenameTemplate.isEmpty() ? defaultFilenameTemplate() : filenameTemplate;
}

void TargetStore::load(const KConfig &config)
{
    areas.clear();
    migratedIds.clear();
    groups.clear();

    const KConfigGroup general = config.group(QStringLiteral("General"));
    captureDirectory = general.readEntry("CaptureDirectory", QString());
    filenameTemplate = general.readEntry("FilenameTemplate", QString());
    autostartInitialized = general.hasKey("Autostart");
    autostart = general.readEntry("Autostart", true);
    usePrintKey = general.readEntry("UsePrintKey", true);
    printOpensOverlay = general.readEntry("PrintOpensOverlay", true);
    const QString start = general.readEntry("OverlayStart", QString());
    if (start == QLatin1String("frozen")) {
        overlayStart = OverlayStart::Frozen;
    } else if (start == QLatin1String("live")) {
        overlayStart = OverlayStart::Live;
    } else if (start.isEmpty() && !general.readEntry("OverlayStartsFrozen", true)) {
        overlayStart = OverlayStart::Live; // before 0.3.0: "Start frozen" was switched off
    } else {
        overlayStart = OverlayStart::LastUsed;
    }
    lastOverlayFrozen = general.readEntry("LastOverlayFrozen", true);
    overlayHelpExpanded = general.readEntry("OverlayHelpExpanded", true);
    captureOnRelease = general.readEntry("CaptureOnRelease", false);

    monitorNames.clear();
    const KConfigGroup monitors = config.group(QStringLiteral("MonitorNames"));
    for (const QString &key : monitors.keyList()) {
        const QString name = monitors.readEntry(key, QString()).trimmed();
        if (!name.isEmpty()) {
            monitorNames.insert(key, name);
        }
    }

    const KConfigGroup builtIn = config.group(QStringLiteral("BuiltIn"));
    cursorScreens = builtIn.readEntry("CursorScreens", QStringList());
    allMonitorsCursor = builtIn.readEntry("AllMonitorsCursor", false);

    QStringList groupNames = config.groupList();
    std::sort(groupNames.begin(), groupNames.end());
    for (const QString &name : std::as_const(groupNames)) {
        const KConfigGroup entry = config.group(name);
        const bool legacy = name.startsWith(QLatin1String(legacyAreaGroupPrefix));
        if (name.startsWith(QLatin1String(areaGroupPrefix)) || legacy) {
            Area area;
            const QString storedId = name.mid(int(qstrlen(legacy ? legacyAreaGroupPrefix : areaGroupPrefix)));
            area.id = migrateId(storedId);
            if (area.id != storedId) {
                migratedIds.insert(storedId, area.id);
            }
            area.name = entry.readEntry("Name", QString());
            area.connector = entry.readEntry("Connector", QString());
            area.monitorIdentity = entry.readEntry("MonitorIdentity", QString());
            area.rect = entry.readEntry("Rect", QRect());
            area.includeCursor = entry.readEntry("IncludeCursor", false);
            if (!area.id.isEmpty() && !area.name.isEmpty() && !area.connector.isEmpty()) {
                areas.append(area);
            }
        } else if (name.startsWith(QLatin1String(groupGroupPrefix))) {
            Group group;
            group.id = name.mid(int(qstrlen(groupGroupPrefix)));
            group.name = entry.readEntry("Name", QString());
            for (const QString &target : entry.readEntry("Targets", QStringList())) {
                group.targetIds.append(migrateId(target));
            }
            group.includeCursor = entry.readEntry("IncludeCursor", false);
            if (!group.id.isEmpty() && !group.name.isEmpty()) {
                groups.append(group);
            }
        }
    }
    // Keep the order in which the user created entries.
    QStringList areaOrder;
    for (const QString &id : general.readEntry("AreaOrder", general.readEntry("RegionOrder", QStringList()))) {
        areaOrder.append(migrateId(id));
    }
    const QStringList groupOrder = general.readEntry("GroupOrder", QStringList());
    std::stable_sort(areas.begin(), areas.end(), [&](const Area &a, const Area &b) {
        return areaOrder.indexOf(a.id) < areaOrder.indexOf(b.id);
    });
    std::stable_sort(groups.begin(), groups.end(), [&](const Group &a, const Group &b) {
        return groupOrder.indexOf(a.id) < groupOrder.indexOf(b.id);
    });
    annotations.load(config);
}

void TargetStore::save(KConfig &config) const
{
    for (const QString &name : config.groupList()) {
        if (name.startsWith(QLatin1String(areaGroupPrefix)) || name.startsWith(QLatin1String(legacyAreaGroupPrefix))
            || name.startsWith(QLatin1String(groupGroupPrefix))) {
            config.deleteGroup(name);
        }
    }

    KConfigGroup general = config.group(QStringLiteral("General"));
    general.writeEntry("CaptureDirectory", captureDirectory);
    general.writeEntry("FilenameTemplate", filenameTemplate);
    general.writeEntry("Autostart", autostart);
    general.writeEntry("UsePrintKey", usePrintKey);
    general.writeEntry("PrintOpensOverlay", printOpensOverlay);
    switch (overlayStart) {
    case OverlayStart::LastUsed:
        general.writeEntry("OverlayStart", QStringLiteral("last"));
        break;
    case OverlayStart::Frozen:
        general.writeEntry("OverlayStart", QStringLiteral("frozen"));
        break;
    case OverlayStart::Live:
        general.writeEntry("OverlayStart", QStringLiteral("live"));
        break;
    }
    general.deleteEntry("OverlayStartsFrozen");
    general.writeEntry("LastOverlayFrozen", lastOverlayFrozen);
    general.writeEntry("OverlayHelpExpanded", overlayHelpExpanded);
    general.writeEntry("CaptureOnRelease", captureOnRelease);

    config.deleteGroup(QStringLiteral("MonitorNames"));
    if (!monitorNames.isEmpty()) {
        KConfigGroup monitors = config.group(QStringLiteral("MonitorNames"));
        for (auto it = monitorNames.cbegin(); it != monitorNames.cend(); ++it) {
            monitors.writeEntry(it.key(), it.value());
        }
    }

    KConfigGroup builtIn = config.group(QStringLiteral("BuiltIn"));
    builtIn.writeEntry("CursorScreens", cursorScreens);
    builtIn.writeEntry("AllMonitorsCursor", allMonitorsCursor);

    QStringList areaOrder;
    for (const Area &area : areas) {
        KConfigGroup entry = config.group(QLatin1String(areaGroupPrefix) + area.id);
        entry.writeEntry("Name", area.name);
        entry.writeEntry("Connector", area.connector);
        entry.writeEntry("MonitorIdentity", area.monitorIdentity);
        entry.writeEntry("Rect", area.rect);
        entry.writeEntry("IncludeCursor", area.includeCursor);
        areaOrder.append(area.id);
    }
    QStringList groupOrder;
    for (const Group &group : groups) {
        KConfigGroup entry = config.group(QLatin1String(groupGroupPrefix) + group.id);
        entry.writeEntry("Name", group.name);
        entry.writeEntry("Targets", group.targetIds);
        entry.writeEntry("IncludeCursor", group.includeCursor);
        groupOrder.append(group.id);
    }
    general.writeEntry("AreaOrder", areaOrder);
    general.deleteEntry("RegionOrder");
    general.writeEntry("GroupOrder", groupOrder);
    annotations.save(config);
    config.sync();
}

const Area *TargetStore::area(const QString &id) const
{
    const auto it = std::find_if(areas.cbegin(), areas.cend(), [&](const Area &r) { return r.id == id; });
    return it == areas.cend() ? nullptr : &*it;
}

Area *TargetStore::area(const QString &id)
{
    return const_cast<Area *>(std::as_const(*this).area(id));
}

const Group *TargetStore::group(const QString &id) const
{
    const auto it = std::find_if(groups.cbegin(), groups.cend(), [&](const Group &g) { return g.id == id; });
    return it == groups.cend() ? nullptr : &*it;
}

Group *TargetStore::group(const QString &id)
{
    return const_cast<Group *>(std::as_const(*this).group(id));
}

QString TargetStore::monitorKey(const ScreenInfo &screen)
{
    const QString identity = screen.identity();
    return identity.isEmpty() ? QStringLiteral("connector:") + screen.connector : identity;
}

QString TargetStore::monitorName(const ScreenInfo &screen) const
{
    return monitorNames.value(monitorKey(screen), screen.connector);
}

void TargetStore::setMonitorName(const ScreenInfo &screen, const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed == screen.connector) {
        monitorNames.remove(monitorKey(screen));
    } else {
        monitorNames.insert(monitorKey(screen), trimmed);
    }
}

QString TargetStore::nextAreaName(const QString &prefix) const
{
    for (int n = 1;; ++n) {
        const QString candidate = prefix + QLatin1Char('-') + QString::number(n);
        if (isNameAvailable(candidate)) {
            return candidate;
        }
    }
}

bool TargetStore::isNameAvailable(const QString &name, const QString &exceptId, const QStringList &reservedTokens) const
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed.compare(allMonitorsTargetId(), Qt::CaseInsensitive) == 0
        || reservedTokens.contains(trimmed, Qt::CaseInsensitive)) {
        return false;
    }
    for (const Area &area : areas) {
        if (area.id != exceptId
            && (area.name.compare(trimmed, Qt::CaseInsensitive) == 0
                || area.connector.compare(trimmed, Qt::CaseInsensitive) == 0)) {
            return false;
        }
    }
    for (const Group &group : groups) {
        if (group.id != exceptId && group.name.compare(trimmed, Qt::CaseInsensitive) == 0) {
            return false;
        }
    }
    for (auto it = monitorNames.cbegin(); it != monitorNames.cend(); ++it) {
        if (it.key() != exceptId && it.value().compare(trimmed, Qt::CaseInsensitive) == 0) {
            return false;
        }
    }
    return true;
}

Area &TargetStore::addArea(const ScreenInfo &screen, const QRect &rect)
{
    Area area;
    area.id = newId(areaIdPrefix);
    area.name = nextAreaName(monitorName(screen));
    area.connector = screen.connector;
    area.monitorIdentity = screen.identity();
    area.rect = rect;
    areas.append(area);
    return areas.last();
}

Group &TargetStore::addGroup(const QString &name)
{
    Group group;
    group.id = newId("group:");
    group.name = name;
    groups.append(group);
    return groups.last();
}

void TargetStore::remove(const QString &id)
{
    areas.removeIf([&](const Area &r) { return r.id == id; });
    groups.removeIf([&](const Group &g) { return g.id == id; });
    for (Group &group : groups) {
        group.targetIds.removeAll(id);
    }
}

bool TargetStore::targetIncludesCursor(const QString &targetId) const
{
    if (targetId == allMonitorsTargetId()) {
        return allMonitorsCursor;
    }
    if (isScreenTargetId(targetId)) {
        return cursorScreens.contains(connectorFromScreenTargetId(targetId));
    }
    if (const Area *r = area(targetId)) {
        return r->includeCursor;
    }
    if (const Group *g = group(targetId)) {
        return g->includeCursor;
    }
    return false;
}

} // namespace glimply
