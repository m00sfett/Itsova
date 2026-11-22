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
constexpr auto regionGroupPrefix = "Region ";
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
    return QStringLiteral("{date}_{time}_{region}.png");
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
    regions.clear();
    groups.clear();

    const KConfigGroup general = config.group(QStringLiteral("General"));
    captureDirectory = general.readEntry("CaptureDirectory", QString());
    filenameTemplate = general.readEntry("FilenameTemplate", QString());
    autostartInitialized = general.hasKey("Autostart");
    autostart = general.readEntry("Autostart", true);

    const KConfigGroup builtIn = config.group(QStringLiteral("BuiltIn"));
    cursorScreens = builtIn.readEntry("CursorScreens", QStringList());
    allMonitorsCursor = builtIn.readEntry("AllMonitorsCursor", false);

    QStringList groupNames = config.groupList();
    std::sort(groupNames.begin(), groupNames.end());
    for (const QString &name : std::as_const(groupNames)) {
        const KConfigGroup entry = config.group(name);
        if (name.startsWith(QLatin1String(regionGroupPrefix))) {
            Region region;
            region.id = name.mid(int(qstrlen(regionGroupPrefix)));
            region.name = entry.readEntry("Name", QString());
            region.connector = entry.readEntry("Connector", QString());
            region.monitorIdentity = entry.readEntry("MonitorIdentity", QString());
            region.rect = entry.readEntry("Rect", QRect());
            region.includeCursor = entry.readEntry("IncludeCursor", false);
            if (!region.id.isEmpty() && !region.name.isEmpty() && !region.connector.isEmpty()) {
                regions.append(region);
            }
        } else if (name.startsWith(QLatin1String(groupGroupPrefix))) {
            Group group;
            group.id = name.mid(int(qstrlen(groupGroupPrefix)));
            group.name = entry.readEntry("Name", QString());
            group.targetIds = entry.readEntry("Targets", QStringList());
            group.includeCursor = entry.readEntry("IncludeCursor", false);
            if (!group.id.isEmpty() && !group.name.isEmpty()) {
                groups.append(group);
            }
        }
    }
    // Keep the order in which the user created entries.
    const QStringList regionOrder = general.readEntry("RegionOrder", QStringList());
    const QStringList groupOrder = general.readEntry("GroupOrder", QStringList());
    std::stable_sort(regions.begin(), regions.end(), [&](const Region &a, const Region &b) {
        return regionOrder.indexOf(a.id) < regionOrder.indexOf(b.id);
    });
    std::stable_sort(groups.begin(), groups.end(), [&](const Group &a, const Group &b) {
        return groupOrder.indexOf(a.id) < groupOrder.indexOf(b.id);
    });
}

void TargetStore::save(KConfig &config) const
{
    for (const QString &name : config.groupList()) {
        if (name.startsWith(QLatin1String(regionGroupPrefix)) || name.startsWith(QLatin1String(groupGroupPrefix))) {
            config.deleteGroup(name);
        }
    }

    KConfigGroup general = config.group(QStringLiteral("General"));
    general.writeEntry("CaptureDirectory", captureDirectory);
    general.writeEntry("FilenameTemplate", filenameTemplate);
    general.writeEntry("Autostart", autostart);

    KConfigGroup builtIn = config.group(QStringLiteral("BuiltIn"));
    builtIn.writeEntry("CursorScreens", cursorScreens);
    builtIn.writeEntry("AllMonitorsCursor", allMonitorsCursor);

    QStringList regionOrder;
    for (const Region &region : regions) {
        KConfigGroup entry = config.group(QLatin1String(regionGroupPrefix) + region.id);
        entry.writeEntry("Name", region.name);
        entry.writeEntry("Connector", region.connector);
        entry.writeEntry("MonitorIdentity", region.monitorIdentity);
        entry.writeEntry("Rect", region.rect);
        entry.writeEntry("IncludeCursor", region.includeCursor);
        regionOrder.append(region.id);
    }
    QStringList groupOrder;
    for (const Group &group : groups) {
        KConfigGroup entry = config.group(QLatin1String(groupGroupPrefix) + group.id);
        entry.writeEntry("Name", group.name);
        entry.writeEntry("Targets", group.targetIds);
        entry.writeEntry("IncludeCursor", group.includeCursor);
        groupOrder.append(group.id);
    }
    general.writeEntry("RegionOrder", regionOrder);
    general.writeEntry("GroupOrder", groupOrder);
    config.sync();
}

const Region *TargetStore::region(const QString &id) const
{
    const auto it = std::find_if(regions.cbegin(), regions.cend(), [&](const Region &r) { return r.id == id; });
    return it == regions.cend() ? nullptr : &*it;
}

Region *TargetStore::region(const QString &id)
{
    return const_cast<Region *>(std::as_const(*this).region(id));
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

QString TargetStore::nextRegionName(const QString &connector) const
{
    for (int n = 1;; ++n) {
        const QString candidate = connector + QLatin1Char('-') + QString::number(n);
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
    for (const Region &region : regions) {
        if (region.id != exceptId
            && (region.name.compare(trimmed, Qt::CaseInsensitive) == 0
                || region.connector.compare(trimmed, Qt::CaseInsensitive) == 0)) {
            return false;
        }
    }
    for (const Group &group : groups) {
        if (group.id != exceptId && group.name.compare(trimmed, Qt::CaseInsensitive) == 0) {
            return false;
        }
    }
    return true;
}

Region &TargetStore::addRegion(const ScreenInfo &screen, const QRect &rect)
{
    Region region;
    region.id = newId("region:");
    region.name = nextRegionName(screen.connector);
    region.connector = screen.connector;
    region.monitorIdentity = screen.identity();
    region.rect = rect;
    regions.append(region);
    return regions.last();
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
    regions.removeIf([&](const Region &r) { return r.id == id; });
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
    if (const Region *r = region(targetId)) {
        return r->includeCursor;
    }
    if (const Group *g = group(targetId)) {
        return g->includeCursor;
    }
    return false;
}

} // namespace glimply
