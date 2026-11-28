#pragma once

#include "historystore.h"
#include "targets.h"

#include <KSharedConfig>

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QPair>
#include <QRect>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

class QImage;
class QJsonObject;
class QTimer;

namespace itsova
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
class HookRunner;
class RecordingHint;
struct HookResult;
class IntervalController;
class PinManager;
class ReplayManager;
class TesseractOcrEngine;
class ThumbnailCache;
struct McpToolResponse;
class QuickEditorWindow;
class ScreenRecorder;
struct RecordingTarget;
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
    // triggerKind feeds the capture history ("interval" captures are not recorded there).
    void recordCapture(const QStringList &paths, const QString &displayName, const QString &targetId = QString(),
                       const QString &triggerKind = QStringLiteral("manual"));
    // Removes a deleted path (for example a purged unredacted Print snapshot) from last-capture state.
    void forgetCapturePath(const QString &path);
    [[nodiscard]] CliResolveResult resolveTarget(const QString &query) const;
    void captureAsync(const CliCaptureRequest &request, std::function<void(const CliCaptureResult &)> callback);
    void mcpDispatchAsync(const QString &toolName, const QString &argumentsJson,
                          std::function<void(const McpToolResponse &)> callback);
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
                               const QString &baseToken, const QString &deferredPrintPath = QString(),
                               bool deferredClearClipboard = false);
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
    void runPostCaptureHook(const QString &targetId, const QString &displayName, const QStringList &files,
                            const QDateTime &timestamp, const QString &format, const QString &triggerKind,
                            bool isInterval = false, const QString &groupId = QString());
    void testPostCaptureHook(const QString &command, int timeoutSeconds,
                             std::function<void(const HookResult &)> onDone);
    [[nodiscard]] QList<MenuTarget> menuTargets() const;

    // Capture history (#26). historyEntries() prunes files that are gone before returning.
    [[nodiscard]] const QList<HistoryEntry> &historyEntries();
    [[nodiscard]] QJsonArray historyJson();
    // action: "open", "edit", "copy", "folder" or "trash". path must belong to the history entry;
    // an empty path means every file of the entry. Returns an error message or an empty string.
    QString historyAction(const QString &action, const QString &entryId, const QString &path);
    // Pin-to-Screen (#28). logicalRect is where the content was on screen (global, logical).
    void pinImage(const QImage &image, const QRect &logicalRect = {});
    void pinLastCapture();
    void pinMoved(const QString &id, int x, int y);
    // Text recognition (#27): recognizes the images in the given order, copies the joined text to
    // the clipboard and reports the result in a notification.
    void copyTextFromImages(const QList<QImage> &images);
    // Opens the overlay in text mode: Enter copies the text of the selection instead of saving.
    void startOcrSelection();
    [[nodiscard]] TesseractOcrEngine &ocrEngine();
    [[nodiscard]] QStringList ocrLanguages() const;
    void clearHistory();
    // Quits and starts a fresh instance once this one has exited (language change, #53).
    // Returns false when the new instance could not be scheduled; itsova keeps running then.
    bool restart();

    [[nodiscard]] static QString targetLabel(const TargetStore &store, const QString &id);

    // Screen recording (#30). targetId: "screen:<connector>", "all", an area id,
    // "window:<uuid>" or "region:x,y,w,h". Starts after delaySeconds (0: at once) and remembers
    // the target for the start/stop shortcut. Returns an error message, or an empty string.
    QString startRecording(const QString &targetId, int delaySeconds = 0, RecordingAudioSelection audio = {});
    [[nodiscard]] bool recordingAudioAvailable() const;
    void stopRecording(const QString &origin);
    // `itsova --verbose` adds debug logging on top of the setting (#92).
    void setVerboseFromCommandLine(bool on);
    // "Click to stop" names this shortcut (#98).
    [[nodiscard]] QString recordingStopShortcutText() const;
    void setRecordingPaused(bool paused);
    // The start/stop shortcut: stops a running recording, records the last target again, or opens
    // the overlay to choose one.
    void toggleRecording(const QString &origin);
    // The target the recording shortcut records (#124); an empty id clears it.
    void setDefaultRecordingTarget(const QString &targetId);
    // A recording target as the settings name it (#124).
    [[nodiscard]] QString recordingTargetName(const QString &targetId) const;
    [[nodiscard]] bool isRecording() const;
    // {state: idle|starting|recording|paused|finishing, elapsedMs, target, targetName, format}
    [[nodiscard]] QJsonObject recordingState() const;
    // Replay (#104): plays any video fullscreen (all-monitor recordings across all monitors).
    QString startReplay(const QString &filePath, const QString &explicitScreen = QString());
    void stopReplay();
    [[nodiscard]] bool isReplayActive() const;
    // Opens the overlay with the Record drawer, to choose what to record.
    void openOverlayForRecording();
    // Quits; a running recording is stopped and written first (at most a few seconds).
    void quit();
    // A capture showed the real scale of a monitor (see nativeScales_).
    void noteNativeScale(const ScreenInfo &screen, qreal scale);

Q_SIGNALS:
    void defaultRecordingTargetChanged();
    void targetsChanged();
    void intervalsChanged();
    void captured(bool success);
    void historyChanged();
    void recordingChanged();

private:
    void executeTriggerNow(const QString &id);
    void startPrintNow(std::optional<bool> forcedFrozen = std::nullopt, bool forceOverlay = false);
    void onTriggerFinished(const TriggerResult &result);
    void syncShortcuts(const QStringList &deletedIds = {});
    void migrateLegacyIds();
    void claimPrintKey();
    void promptPrintKeyOnFirstLaunch();
    void applyAutostart();
    void attachEditorWindow(QuickEditorWindow *window, const QString &deferredPrintPath = QString(),
                            bool deferredClearClipboard = false);
    void notify(const QString &eventId, const QString &title, const QStringList &messages);
    void notifyError(const QString &title, const QStringList &messages);
    void showInFolder(const QStringList &paths);
    void pruneHistory();
    void savePinImageAs(const QImage &image);
    // Makes pins transparent, then calls start; pins come back when owner is destroyed.
    // recordLatency: the wait for the pins counts into the running Print latency trace (#67).
    void startWithPinsHidden(QObject *owner, std::function<void()> start, bool recordLatency = false);
    void historyModified(bool sweepThumbnails);
    void requestThumbnails();
    [[nodiscard]] std::optional<RecordingTarget> recordingTargetFor(const QString &targetId, QString *error) const;
    void beginRecording(const QString &targetId, RecordingAudioSelection audio);
    void startRecorder(const QString &targetId, RecordingAudioSelection audio);
    [[nodiscard]] qreal nativeScale(const ScreenInfo &screen) const;
    void syncRecordingUi();

    KSharedConfigPtr config_;
    TargetStore store_;
    ShortcutManager *shortcuts_;
    TriggerEngine *engine_;
    TrayController *tray_;
    ControlService *control_;
    CountdownController *countdown_;
    CountdownBanner *countdownBanner_;
    IntervalController *intervals_;
    HookRunner *hooks_;
    PinManager *pins_ = nullptr;
    std::shared_ptr<TesseractOcrEngine> ocr_;
    QPointer<QObject> settings_;
    QPointer<QObject> selector_;
    QPointer<CaptureSession> session_;
    QPointer<ColorPickerSession> colorPicker_;
    QList<QPointer<QuickEditorWindow>> editors_;
    std::unique_ptr<HistoryStore> history_;
    std::unique_ptr<ThumbnailCache> thumbnails_;
    QSet<QString> thumbnailsPending_;
    QTimer *historyChangedTimer_ = nullptr;
    QStringList lastPaths_;
    QString lastCaptureTargetId_;
    QString lastCaptureDisplayName_;
    QDateTime lastCaptureTimestamp_;
    ScreenRecorder *recorder_ = nullptr;
    ReplayManager *replay_ = nullptr;
    // Native scale per connector as KWin reports it in captures, with the geometry it belongs to
    // (QScreen has no fractional scale on Wayland). Filled before region recordings.
    QHash<QString, QPair<QRect, qreal>> nativeScales_;
    bool recordingPending_ = false;
    QTimer *recordingTicker_ = nullptr;
    // Recording hints (#125).
    void showRecordingHint(const QString &iconName, const QString &title, const QString &detail, int durationMs,
                           const std::function<void()> &onClick = {});
    [[nodiscard]] QString recordingStopInstruction() const;
    void cancelPendingRecordingStart();
    [[nodiscard]] bool recordingStartPending() const;
    int previousRecorderState_ = 0; // ScreenRecorder::State
    RecordingHint *recordingHint_ = nullptr;
    RecordingHintPlacement recordingHintPlacement_;
    QString recordingHintConnector_;
    QTimer *recordingStartAfterHint_ = nullptr; // a recording that waits for its start hint to go
    QTimer *recordingResumeAfterHint_ = nullptr;
    bool resumeHintShown_ = false;
    bool verboseFromCommandLine_ = false;
};

} // namespace itsova
