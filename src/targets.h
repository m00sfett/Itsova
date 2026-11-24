#pragma once

#include "annotations/annotationsettings.h"
#include "imageoutput.h"

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

struct ExportOverride
{
    bool enabled = false;
    ExportOptions options;

    bool operator==(const ExportOverride &) const = default;
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
    int delaySeconds = 0;
    ExportOverride exportOverride;
};

struct Group
{
    QString id; // "group:<uuid>"
    QString name;
    QStringList targetIds;
    bool includeCursor = false;
    int delaySeconds = 0;
    ExportOverride exportOverride;
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
    // Names the user gave monitors, keyed by monitorKey(). A monitor without one is called by its
    // connector.
    QHash<QString, QString> monitorNames;
    bool allMonitorsCursor = false;

    // Ids renamed while loading an older configuration (old id -> new id), so callers can move
    // state such as global shortcuts.
    QHash<QString, QString> migratedIds;

    QString captureDirectory; // empty = default
    QString filenameTemplate; // empty = default
    bool autostart = true;
    bool autostartInitialized = false;

    // Print key and capture overlay.
    bool usePrintKey = true; // glimply owns the Print key (handed back to its previous owner when off)
    bool printOpensOverlay = true; // false: Print only saves the full screenshot
    enum class OverlayStart { LastUsed, Frozen, Live };
    OverlayStart overlayStart = OverlayStart::LastUsed;
    bool lastOverlayFrozen = true; // the mode the overlay was in when it was last used
    bool overlayHelpExpanded = true; // the control overview in the overlay's top bar
    [[nodiscard]] bool overlayStartsFrozen() const;
    bool captureOnRelease = false; // save as soon as a new rectangle is drawn

    AnnotationSettings annotations;
    ExportOptions exportOptions;
    QHash<QString, ExportOverride> builtInExportOverrides;
    QHash<QString, int> builtInDelays;
    int captureDelaySeconds = 0;
    bool keepDelayActive = false;

    [[nodiscard]] static QString defaultCaptureDirectory();
    [[nodiscard]] static QString defaultFilenameTemplate();
    [[nodiscard]] QString effectiveCaptureDirectory() const;
    [[nodiscard]] QString effectiveFilenameTemplate() const;
    [[nodiscard]] ExportOverride targetExportOverride(const QString &targetId) const;
    void setTargetExportOverride(const QString &targetId, const ExportOverride &overrideValue);
    [[nodiscard]] ExportOptions effectiveExportOptions(const QString &targetId = QString(),
                                                       const QString &groupId = QString()) const;
    [[nodiscard]] int targetDelaySeconds(const QString &targetId) const;
    void setTargetDelaySeconds(const QString &targetId, int seconds);
    [[nodiscard]] int effectiveDelaySeconds(const QString &targetId = QString()) const;
    bool consumeOneShotDelay();

    void load(const KConfig &config);
    void save(KConfig &config) const;

    [[nodiscard]] const Area *area(const QString &id) const;
    [[nodiscard]] Area *area(const QString &id);
    [[nodiscard]] const Group *group(const QString &id) const;
    [[nodiscard]] Group *group(const QString &id);

    // The physical monitor (manufacturer, model, serial), or its connector when KWin reports none.
    [[nodiscard]] static QString monitorKey(const ScreenInfo &screen);
    // The user's name for the monitor, or its connector. Shown in the UI and used as the file
    // name token and as the prefix of new automatic area names.
    [[nodiscard]] QString monitorName(const ScreenInfo &screen) const;
    // An empty name, or the connector itself, removes the user's name.
    void setMonitorName(const ScreenInfo &screen, const QString &name);

    // Next free automatic name "<prefix>-<n>" (the prefix is the monitor name).
    [[nodiscard]] QString nextAreaName(const QString &prefix) const;
    // True if no area, group or named monitor other than exceptId (an area or group id, or a
    // monitorKey) uses name (case-insensitive) and it is not a built-in token ("all" or a
    // connector name in reservedTokens).
    [[nodiscard]] bool isNameAvailable(const QString &name, const QString &exceptId = QString(),
                                       const QStringList &reservedTokens = {}) const;

    Area &addArea(const ScreenInfo &screen, const QRect &rect);
    Group &addGroup(const QString &name);
    // Removes an area or group and drops it from all groups.
    void remove(const QString &id);

    [[nodiscard]] bool targetIncludesCursor(const QString &targetId) const;
};

} // namespace glimply
