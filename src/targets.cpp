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
constexpr auto exportOverrideGroupPrefix = "ExportOverride ";
constexpr auto builtInDelaysGroup = "BuiltInDelays";

ExportOptions readExportOptions(const KConfigGroup &group, const QString &prefix, const ExportOptions &fallback = {})
{
    ExportOptions opts = fallback;
    const QString fmtKey = group.readEntry(prefix + QStringLiteral("Format"), QString());
    if (const std::optional<ImageFormat> parsed = imageFormatFromKey(fmtKey)) {
        opts.format = *parsed;
    }
    opts.quality = std::clamp(group.readEntry(prefix + QStringLiteral("Quality"), fallback.quality), 1, 100);
    const int pngComp = group.readEntry(prefix + QStringLiteral("PngCompression"), fallback.pngCompression);
    opts.pngCompression = pngComp < 0 ? -1 : std::clamp(pngComp, 0, 9);
    opts.lossless = group.readEntry(prefix + QStringLiteral("Lossless"), fallback.lossless);
    return opts;
}

void writeExportOptions(KConfigGroup &group, const QString &prefix, const ExportOptions &opts)
{
    group.writeEntry(prefix + QStringLiteral("Format"), formatConfigKey(opts.format));
    group.writeEntry(prefix + QStringLiteral("Quality"), std::clamp(opts.quality, 1, 100));
    group.writeEntry(prefix + QStringLiteral("PngCompression"),
                     opts.pngCompression < 0 ? -1 : std::clamp(opts.pngCompression, 0, 9));
    group.writeEntry(prefix + QStringLiteral("Lossless"), opts.lossless);
}

ExportOverride readExportOverride(const KConfigGroup &group)
{
    ExportOverride ov;
    ov.enabled = group.hasKey("ExportFormat");
    if (ov.enabled) {
        ov.options = readExportOptions(group, QStringLiteral("Export"));
    }
    return ov;
}

void writeExportOverride(KConfigGroup &group, const ExportOverride &ov)
{
    if (!ov.enabled) {
        group.deleteEntry("ExportFormat");
        group.deleteEntry("ExportQuality");
        group.deleteEntry("ExportPngCompression");
        group.deleteEntry("ExportLossless");
        return;
    }
    writeExportOptions(group, QStringLiteral("Export"), ov.options);
}

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

QStringList Area::involvedConnectors() const
{
    if (isCrossMonitor()) {
        QStringList connectors;
        connectors.reserve(monitors.size());
        for (const AreaMonitorBinding &binding : monitors) {
            if (!binding.connector.isEmpty() && !connectors.contains(binding.connector)) {
                connectors.append(binding.connector);
            }
        }
        return connectors;
    }
    return connector.isEmpty() ? QStringList{} : QStringList{connector};
}

QString screenTargetId(const QString &connector)
{
    return QLatin1String(screenPrefix) + connector;
}

QString allMonitorsTargetId()
{
    return QStringLiteral("all");
}

QString colorPickerTargetId()
{
    return QStringLiteral("pick-color");
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
    builtInExportOverrides.clear();
    builtInDelays.clear();

    const KConfigGroup general = config.group(QStringLiteral("General"));
    exportOptions = readExportOptions(config.group(QStringLiteral("Export")), QString());
    keepDelayActive = general.readEntry("KeepDelayActive", false);
    captureDelaySeconds = keepDelayActive ? std::clamp(general.readEntry("CaptureDelaySeconds", 0), 0, 60) : 0;
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
    showSelectionLoupe = general.readEntry("ShowSelectionLoupe", true);
    colorPickerFormat = colorFormatFromKey(general.readEntry("ColorPickerFormat", QStringLiteral("hex")));

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
    const KConfigGroup builtInDelayCfg = config.group(QLatin1String(builtInDelaysGroup));
    for (const QString &key : builtInDelayCfg.keyList()) {
        const int secs = std::clamp(builtInDelayCfg.readEntry(key, 0), 0, 60);
        if (secs > 0) {
            builtInDelays.insert(migrateId(key), secs);
        }
    }

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
            area.rect = entry.readEntry("Rect", QRect());
            if (entry.readEntry("CrossMonitor", false)) {
                const int monitorCount = std::max(0, entry.readEntry("MonitorCount", 0));
                for (int i = 0; i < monitorCount; ++i) {
                    const QString monConn = entry.readEntry(QStringLiteral("Monitor%1Connector").arg(i), QString());
                    const QString monId = entry.readEntry(QStringLiteral("Monitor%1Identity").arg(i), QString());
                    if (!monConn.isEmpty()) {
                        area.monitors.append({monConn, monId});
                    }
                }
            } else {
                area.connector = entry.readEntry("Connector", QString());
                area.monitorIdentity = entry.readEntry("MonitorIdentity", QString());
            }
            area.includeCursor = entry.readEntry("IncludeCursor", false);
            area.delaySeconds = std::clamp(entry.readEntry("DelaySeconds", 0), 0, 60);
            area.exportOverride = readExportOverride(entry);
            const bool validSingle = !area.isCrossMonitor() && !area.connector.isEmpty();
            const bool validCross = area.isCrossMonitor() && !area.monitors.isEmpty() && area.rect.isValid();
            if (!area.id.isEmpty() && !area.name.isEmpty() && (validSingle || validCross)) {
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
            group.delaySeconds = std::clamp(entry.readEntry("DelaySeconds", 0), 0, 60);
            group.exportOverride = readExportOverride(entry);
            if (!group.id.isEmpty() && !group.name.isEmpty()) {
                groups.append(group);
            }
        } else if (name.startsWith(QLatin1String(exportOverrideGroupPrefix))) {
            const QString id = migrateId(name.mid(int(qstrlen(exportOverrideGroupPrefix))));
            ExportOverride ov;
            ov.enabled = true;
            ov.options = readExportOptions(entry, QString());
            if (!id.isEmpty()) {
                builtInExportOverrides.insert(id, ov);
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
            || name.startsWith(QLatin1String(groupGroupPrefix))
            || name.startsWith(QLatin1String(exportOverrideGroupPrefix))) {
            config.deleteGroup(name);
        }
    }

    KConfigGroup exportGroup = config.group(QStringLiteral("Export"));
    writeExportOptions(exportGroup, QString(), exportOptions);

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
    general.writeEntry("ShowSelectionLoupe", showSelectionLoupe);
    general.writeEntry("ColorPickerFormat", colorFormatConfigKey(colorPickerFormat));
    general.writeEntry("KeepDelayActive", keepDelayActive);
    general.writeEntry("CaptureDelaySeconds", keepDelayActive ? std::clamp(captureDelaySeconds, 0, 60) : 0);

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

    config.deleteGroup(QLatin1String(builtInDelaysGroup));
    if (!builtInDelays.isEmpty()) {
        KConfigGroup builtInDelayCfg = config.group(QLatin1String(builtInDelaysGroup));
        for (auto it = builtInDelays.cbegin(); it != builtInDelays.cend(); ++it) {
            if (it.value() > 0) {
                builtInDelayCfg.writeEntry(it.key(), std::clamp(it.value(), 0, 60));
            }
        }
    }

    QStringList areaOrder;
    for (const Area &area : areas) {
        KConfigGroup entry = config.group(QLatin1String(areaGroupPrefix) + area.id);
        entry.writeEntry("Name", area.name);
        if (area.isCrossMonitor()) {
            entry.writeEntry("CrossMonitor", true);
            entry.writeEntry("MonitorCount", int(area.monitors.size()));
            for (int i = 0; i < area.monitors.size(); ++i) {
                entry.writeEntry(QStringLiteral("Monitor%1Connector").arg(i), area.monitors.at(i).connector);
                entry.writeEntry(QStringLiteral("Monitor%1Identity").arg(i), area.monitors.at(i).monitorIdentity);
            }
        } else {
            entry.writeEntry("Connector", area.connector);
            entry.writeEntry("MonitorIdentity", area.monitorIdentity);
        }
        entry.writeEntry("Rect", area.rect);
        entry.writeEntry("IncludeCursor", area.includeCursor);
        if (area.delaySeconds > 0) {
            entry.writeEntry("DelaySeconds", std::clamp(area.delaySeconds, 0, 60));
        }
        writeExportOverride(entry, area.exportOverride);
        areaOrder.append(area.id);
    }
    QStringList groupOrder;
    for (const Group &group : groups) {
        KConfigGroup entry = config.group(QLatin1String(groupGroupPrefix) + group.id);
        entry.writeEntry("Name", group.name);
        entry.writeEntry("Targets", group.targetIds);
        entry.writeEntry("IncludeCursor", group.includeCursor);
        if (group.delaySeconds > 0) {
            entry.writeEntry("DelaySeconds", std::clamp(group.delaySeconds, 0, 60));
        }
        writeExportOverride(entry, group.exportOverride);
        groupOrder.append(group.id);
    }
    for (auto it = builtInExportOverrides.cbegin(); it != builtInExportOverrides.cend(); ++it) {
        if (it.value().enabled) {
            KConfigGroup entry = config.group(QLatin1String(exportOverrideGroupPrefix) + it.key());
            writeExportOptions(entry, QString(), it.value().options);
        }
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
        if (area.id == exceptId) {
            continue;
        }
        if (area.name.compare(trimmed, Qt::CaseInsensitive) == 0
            || (!area.connector.isEmpty() && area.connector.compare(trimmed, Qt::CaseInsensitive) == 0)) {
            return false;
        }
        for (const AreaMonitorBinding &binding : area.monitors) {
            if (binding.connector.compare(trimmed, Qt::CaseInsensitive) == 0) {
                return false;
            }
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

Area &TargetStore::addCrossMonitorArea(const QList<ScreenInfo> &screens, const QRect &globalRect)
{
    Area area;
    area.id = newId(areaIdPrefix);
    area.name = nextAreaName(QStringLiteral("area"));
    area.rect = globalRect;
    for (const ScreenInfo &screen : screens) {
        area.monitors.append({screen.connector, screen.identity()});
    }
    areas.append(area);
    return areas.last();
}

Area &TargetStore::addAreaForGlobalRect(const QList<ScreenInfo> &allScreens, const QRect &globalRect)
{
    QList<ScreenInfo> hit;
    for (const ScreenInfo &screen : allScreens) {
        if (!screen.geometry.intersected(globalRect).isEmpty()) {
            hit.append(screen);
        }
    }
    if (hit.size() == 1 && hit.first().geometry.contains(globalRect)) {
        return addArea(hit.first(), globalRect.translated(-hit.first().geometry.topLeft()));
    }
    if (hit.isEmpty() && !allScreens.isEmpty()) {
        return addCrossMonitorArea({allScreens.first()}, globalRect);
    }
    return addCrossMonitorArea(hit, globalRect);
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
    builtInExportOverrides.remove(id);
    builtInDelays.remove(id);
    for (Group &group : groups) {
        group.targetIds.removeAll(id);
    }
}

int TargetStore::targetDelaySeconds(const QString &targetId) const
{
    if (const Area *r = area(targetId)) {
        return std::clamp(r->delaySeconds, 0, 60);
    }
    if (const Group *g = group(targetId)) {
        return std::clamp(g->delaySeconds, 0, 60);
    }
    return std::clamp(builtInDelays.value(targetId, 0), 0, 60);
}

void TargetStore::setTargetDelaySeconds(const QString &targetId, int seconds)
{
    const int clamped = std::clamp(seconds, 0, 60);
    if (Area *r = area(targetId)) {
        r->delaySeconds = clamped;
        return;
    }
    if (Group *g = group(targetId)) {
        g->delaySeconds = clamped;
        return;
    }
    if (clamped > 0) {
        builtInDelays.insert(targetId, clamped);
    } else {
        builtInDelays.remove(targetId);
    }
}

int TargetStore::effectiveDelaySeconds(const QString &targetId) const
{
    if (captureDelaySeconds > 0) {
        return std::clamp(captureDelaySeconds, 0, 60);
    }
    return targetId.isEmpty() ? 0 : targetDelaySeconds(targetId);
}

bool TargetStore::consumeOneShotDelay()
{
    if (!keepDelayActive && captureDelaySeconds > 0) {
        captureDelaySeconds = 0;
        return true;
    }
    return false;
}

ExportOverride TargetStore::targetExportOverride(const QString &targetId) const
{
    if (const Area *r = area(targetId)) {
        return r->exportOverride;
    }
    if (const Group *g = group(targetId)) {
        return g->exportOverride;
    }
    return builtInExportOverrides.value(targetId);
}

void TargetStore::setTargetExportOverride(const QString &targetId, const ExportOverride &overrideValue)
{
    if (Area *r = area(targetId)) {
        r->exportOverride = overrideValue;
        return;
    }
    if (Group *g = group(targetId)) {
        g->exportOverride = overrideValue;
        return;
    }
    if (overrideValue.enabled) {
        builtInExportOverrides.insert(targetId, overrideValue);
    } else {
        builtInExportOverrides.remove(targetId);
    }
}

ExportOptions TargetStore::effectiveExportOptions(const QString &targetId, const QString &groupId) const
{
    ExportOptions resolved = exportOptions;
    if (const Group *g = groupId.isEmpty() ? nullptr : group(groupId); g && g->exportOverride.enabled) {
        resolved = g->exportOverride.options;
    }
    if (!targetId.isEmpty()) {
        const ExportOverride targetOv = targetExportOverride(targetId);
        if (targetOv.enabled) {
            resolved = targetOv.options;
        }
    }
    if (!isImageFormatSupported(resolved.format)) {
        resolved.format = ImageFormat::Png;
    }
    return resolved;
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
