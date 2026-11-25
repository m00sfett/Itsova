#pragma once

#include "annotations/annotationsettings.h"
#include "colorformat.h"
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

struct IntervalConfig
{
    int seconds = 5;        // clamped 1..86400
    int maxCaptures = 0;    // 0 = unlimited, 0..100000
    int maxMinutes = 0;     // 0 = unlimited, 0..10080
    bool onlyWhenChanged = false;

    bool operator==(const IntervalConfig &) const = default;
};

enum class HookOverrideMode {
    UseGlobal,
    Custom,
    Disabled,
};

struct HookConfig
{
    bool enabled = false;
    QString command;
    int timeoutSeconds = 30;
    bool runOnInterval = false;

    bool operator==(const HookConfig &) const = default;
};

struct HookOverride
{
    HookOverrideMode mode = HookOverrideMode::UseGlobal;
    QString command;
    int timeoutSeconds = 30;

    bool operator==(const HookOverride &) const = default;
};

enum class McpAccessMode {
    Allowlist,
    ConfirmPerCapture,
    FullSession,
};

struct McpConfig
{
    bool enabled = false;
    McpAccessMode accessMode = McpAccessMode::Allowlist;
    QStringList allowedTargetIds;
    bool notifyOnCapture = true;

    bool operator==(const McpConfig &) const = default;
};

struct AreaMonitorBinding
{
    QString connector;
    QString monitorIdentity;

    bool operator==(const AreaMonitorBinding &) const = default;
};

// A fixed rectangle: single-monitor areas store monitor-local coordinates in rect with connector
// and monitorIdentity populated (monitors empty); cross-monitor areas store global logical
// coordinates in rect with monitors populated (connector and monitorIdentity empty).
struct Area
{
    QString id; // stable, e.g. "area:<uuid>"; used as shortcut action id
    QString name; // unique, user visible, filename token
    QString connector;
    QString monitorIdentity; // ScreenInfo::identity() when the area was defined
    QRect rect;
    QList<AreaMonitorBinding> monitors; // non-empty for cross-monitor areas
    bool includeCursor = false;
    int delaySeconds = 0;
    ExportOverride exportOverride;
    IntervalConfig interval;
    HookOverride hookOverride;

    [[nodiscard]] bool isCrossMonitor() const { return !monitors.isEmpty(); }
    [[nodiscard]] QStringList involvedConnectors() const;
};

struct Group
{
    QString id; // "group:<uuid>"
    QString name;
    QStringList targetIds;
    bool includeCursor = false;
    int delaySeconds = 0;
    ExportOverride exportOverride;
    IntervalConfig interval;
    HookOverride hookOverride;
};

// Ids of built-in targets and interval shortcuts.
[[nodiscard]] QString screenTargetId(const QString &connector);
[[nodiscard]] QString allMonitorsTargetId();
[[nodiscard]] QString colorPickerTargetId();
// Global shortcut actions for Pin-to-Screen (#28), unbound by default.
[[nodiscard]] QString pinLastCaptureActionId();
[[nodiscard]] QString pinsToggleActionId();
// Global shortcut that opens the overlay in text mode (#27), unbound by default.
[[nodiscard]] QString ocrSelectionActionId();
[[nodiscard]] bool isScreenTargetId(const QString &id);
[[nodiscard]] QString connectorFromScreenTargetId(const QString &id);
[[nodiscard]] QString intervalShortcutId(const QString &targetId);
[[nodiscard]] bool isIntervalShortcutId(const QString &id);
[[nodiscard]] QString targetIdFromIntervalShortcutId(const QString &id);

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
    // The overlay's one-time hint ("Hover over buttons…") was shown and dismissed (#72).
    bool overlayFirstHintShown = false;
    // Where the user dragged the overlay panel, per monitor (monitorKey()), top-left in logical
    // monitor coordinates (#69). No entry: the panel places itself.
    QHash<QString, QPoint> overlayPanelPositions;
    [[nodiscard]] bool overlayStartsFrozen() const;
    bool captureOnRelease = false; // save as soon as a new rectangle is drawn
    bool showSelectionLoupe = true;
    ColorFormat colorPickerFormat = ColorFormat::Hex;

    AnnotationSettings annotations;
    ExportOptions exportOptions;
    QHash<QString, ExportOverride> builtInExportOverrides;
    QHash<QString, int> builtInDelays;
    QHash<QString, IntervalConfig> builtInIntervals;
    HookConfig hook;
    QHash<QString, HookOverride> builtInHookOverrides;
    McpConfig mcp;
    int captureDelaySeconds = 0;
    bool keepDelayActive = false;
    // Number of entries kept in the capture history (5..100).
    int historyLimit = 20;
    // Text recognition languages (backend codes); empty = every installed language except osd.
    QStringList ocrLanguages;
    QString uiLanguage; // [General] Language: a UI language code, empty follows the system (#53)

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
    [[nodiscard]] IntervalConfig targetIntervalConfig(const QString &targetId) const;
    void setTargetIntervalConfig(const QString &targetId, const IntervalConfig &cfg);
    [[nodiscard]] HookOverride targetHookOverride(const QString &targetId) const;
    void setTargetHookOverride(const QString &targetId, const HookOverride &overrideValue);
    [[nodiscard]] HookConfig effectiveHookConfig(const QString &targetId = QString(),
                                                 const QString &groupId = QString(),
                                                 bool isInterval = false) const;

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
    Area &addCrossMonitorArea(const QList<ScreenInfo> &screens, const QRect &globalRect);
    Area &addAreaForGlobalRect(const QList<ScreenInfo> &allScreens, const QRect &globalRect);
    Group &addGroup(const QString &name);
    // Removes an area or group and drops it from all groups.
    void remove(const QString &id);

    [[nodiscard]] bool targetIncludesCursor(const QString &targetId) const;
};

} // namespace glimply
