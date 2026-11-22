#pragma once

#include <QHash>
#include <QList>
#include <QRect>
#include <QString>
#include <QStringList>

class KConfig;

namespace glimply
{

// A connected monitor as seen at trigger or setup time. Geometry is logical (compositor space).
struct ScreenInfo
{
    QString connector;
    QRect geometry;
    QString manufacturer;
    QString model;
    QString serialNumber;

    [[nodiscard]] QString identity() const;
};

// A fixed rectangle on one monitor, stored in logical coordinates relative to that monitor.
struct Area
{
    QString id; // stable, e.g. "area:<uuid>"; used as shortcut action id
    QString name; // unique, user visible, filename token
    QString connector;
    QString monitorIdentity; // ScreenInfo::identity() when the area was defined
    QRect rect;
    bool includeCursor = false;
};

struct Group
{
    QString id; // "group:<uuid>"
    QString name;
    QStringList targetIds;
    bool includeCursor = false;
};

// Ids of built-in targets.
[[nodiscard]] QString screenTargetId(const QString &connector);
[[nodiscard]] QString allMonitorsTargetId();
[[nodiscard]] bool isScreenTargetId(const QString &id);
[[nodiscard]] QString connectorFromScreenTargetId(const QString &id);

// The persistent configuration: areas, groups and per-target settings of built-in targets.
class TargetStore
{
public:
    QList<Area> areas;
    QList<Group> groups;
    QStringList cursorScreens; // connectors whose built-in full-screen target includes the cursor
    bool allMonitorsCursor = false;

    // Ids renamed while loading an older configuration (old id -> new id), so callers can move
    // state such as global shortcuts.
    QHash<QString, QString> migratedIds;

    QString captureDirectory; // empty = default
    QString filenameTemplate; // empty = default
    bool autostart = true;
    bool autostartInitialized = false;

    [[nodiscard]] static QString defaultCaptureDirectory();
    [[nodiscard]] static QString defaultFilenameTemplate();
    [[nodiscard]] QString effectiveCaptureDirectory() const;
    [[nodiscard]] QString effectiveFilenameTemplate() const;

    void load(const KConfig &config);
    void save(KConfig &config) const;

    [[nodiscard]] const Area *area(const QString &id) const;
    [[nodiscard]] Area *area(const QString &id);
    [[nodiscard]] const Group *group(const QString &id) const;
    [[nodiscard]] Group *group(const QString &id);

    // Next free automatic name "<connector>-<n>".
    [[nodiscard]] QString nextAreaName(const QString &connector) const;
    // True if no area or group other than exceptId uses name (case-insensitive) and it is not a
    // built-in token ("all" or a connector name in reservedTokens).
    [[nodiscard]] bool isNameAvailable(const QString &name, const QString &exceptId = QString(),
                                       const QStringList &reservedTokens = {}) const;

    Area &addArea(const ScreenInfo &screen, const QRect &rect);
    Group &addGroup(const QString &name);
    // Removes an area or group and drops it from all groups.
    void remove(const QString &id);

    [[nodiscard]] bool targetIncludesCursor(const QString &targetId) const;
};

} // namespace glimply
