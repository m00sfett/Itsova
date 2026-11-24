#pragma once

#include "targets.h"

#include <KSharedConfig>

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

class QImage;
class QJsonObject;

namespace glimply
{

struct ActiveIntervalStatus;
class AnnotationDocument;
class CaptureSession;
struct CliCaptureRequest;
struct CliCaptureResult;
struct CliResolveResult;
class ColorPickerSession;
class ControlService;
class CountdownBanner;
class CountdownController;
class IntervalController;
class QuickEditorWindow;
class ShortcutManager;
class TrayController;
class TriggerEngine;
struct MenuTarget;
struct TriggerResult;

// Wires the tray, global shortcuts, the trigger engine and the settings together.
class App : public QObject
{
    Q_OBJECT

public:
    explicit App(QObject *parent = nullptr);
    ~App() override;

    [[nodiscard]] TargetStore &store();
    [[nodiscard]] ShortcutManager *shortcuts() const;

    // Persists the store and refreshes shortcuts and menus. Pass ids of removed areas/groups so
    // their global shortcuts are cleared as well.
    void storeChanged(const QStringList &deletedIds = {});
    // Persists the store only, for state that changes nothing else (overlay mode, help).
    void saveSettings();

    void trigger(const QString &id);
    // Print key: saves the full screenshot, then opens the capture overlay.
    void startPrint();
    // Opens the capture overlay to draw and save new areas, without capturing.
    void addArea();
    // Opens the standalone native color picker across all monitors.
    void startColorPicker();
    [[nodiscard]] bool isCountdownActive() const;
    [[nodiscard]] int captureDelaySeconds() const;
    [[nodiscard]] bool keepDelayActive() const;
    void setCaptureDelay(int seconds, bool keepActive);
    void cancelCountdown();
    QString startInterval(const QString &id, const std::optional<IntervalConfig> &overrideConfig = std::nullopt);
    bool stopInterval(const QString &id);
    void stopAllIntervals();
    bool toggleInterval(const QString &id);
    [[nodiscard]] bool isIntervalActive(const QString &id) const;
    [[nodiscard]] QList<ActiveIntervalStatus> activeIntervals() const;
    void runWithDelay(int seconds,
                      const QString &label,
                      std::function<void()> onReady,
                      std::function<void()> onCancelled = {});
    // Takes the Print key (on) or hands it back to its previous owner (off).
    void setUsePrintKey(bool on);
    // A capture was written: tray blink, status, "Open last capture".
    void recordCapture(const QStringList &paths, const QString &displayName, const QString &targetId = QString());
    // Removes a deleted path (for example a purged unredacted Print snapshot) from last-capture state.
    void forgetCapturePath(const QString &path);
    [[nodiscard]] CliResolveResult resolveTarget(const QString &query) const;
    void captureAsync(const CliCaptureRequest &request, std::function<void(const CliCaptureResult &)> callback);
    [[nodiscard]] QJsonObject addAreaRect(const QString &name, const QString &connector, const QRect &rect);
    [[nodiscard]] QString openOverlay(const QString &mode, int delaySeconds);
    [[nodiscard]] QJsonObject lastCaptureInfo() const;
    // Notification after an overlay session, with Open and Show in Folder.
    void notifySaved(const QString &title, const QStringList &paths, const QStringList &errors);
    void notifyPartialCapture(const QString &title, const QStringList &messages);
    void openLastCapture();
    void editLastCapture();
    void openEditorForFile(const QString &path);
    void openEditorFromCapture(const QImage &image, qreal scale, std::unique_ptr<AnnotationDocument> annotations,
                               const QString &baseToken);
    void openCaptureFolder();
    // addArea starts the selection overlay; targetId selects that target or group in the window.
    void showSettings(bool addArea = false, const QString &targetId = QString());
    // Opens the selection overlay for an existing area, without the settings window.
    void editArea(const QString &areaId);

    // Renames an area, group or connected monitor (an empty name gives a monitor its connector
    // name back). Returns an error message, or an empty string on success.
    QString renameTarget(const QString &id, const QString &name);
    // Asks for a new monitor name in a small dialog (from the tray menu without the widget).
    void renameMonitorWithDialog(const QString &id);
    // Removes an area or group (built-in targets cannot be removed).
    void removeTarget(const QString &id);
    [[nodiscard]] bool hasLastCapture() const;
    [[nodiscard]] QList<MenuTarget> menuTargets() const;

    [[nodiscard]] static QString targetLabel(const TargetStore &store, const QString &id);

Q_SIGNALS:
    void targetsChanged();
    void intervalsChanged();
    void captured(bool success);

private:
    void executeTriggerNow(const QString &id);
    void startPrintNow(std::optional<bool> forcedFrozen = std::nullopt, bool forceOverlay = false);
    void onTriggerFinished(const TriggerResult &result);
    void syncShortcuts(const QStringList &deletedIds = {});
    void migrateLegacyIds();
    void claimPrintKey();
    void applyAutostart();
    void attachEditorWindow(QuickEditorWindow *window);
    void notify(const QString &eventId, const QString &title, const QStringList &messages);
    void notifyError(const QString &title, const QStringList &messages);

    KSharedConfigPtr config_;
    TargetStore store_;
    ShortcutManager *shortcuts_;
    TriggerEngine *engine_;
    TrayController *tray_;
    ControlService *control_;
    CountdownController *countdown_;
    CountdownBanner *countdownBanner_;
    IntervalController *intervals_;
    QPointer<QObject> settings_;
    QPointer<QObject> selector_;
    QPointer<CaptureSession> session_;
    QPointer<ColorPickerSession> colorPicker_;
    QList<QPointer<QuickEditorWindow>> editors_;
    QStringList lastPaths_;
    QString lastCaptureTargetId_;
    QString lastCaptureDisplayName_;
    QDateTime lastCaptureTimestamp_;
};

} // namespace glimply
