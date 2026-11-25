#pragma once

#include <QDBusContext>
#include <QObject>
#include <QString>

class QDBusServiceWatcher;

namespace glimply
{

class App;

// Session bus interface for the Plasma widget (and scripts): lists targets and runs the actions
// the tray offers. Registered at /Control on glimply's unique service name.
class ControlService : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.m00sfett.glimply.Control")

public:
    explicit ControlService(App *app);

    [[nodiscard]] bool widgetActive() const;

public Q_SLOTS:
    // JSON array of {targetId, kind (group|area|monitor|all), name, detail, shortcut, available}.
    Q_SCRIPTABLE QString Targets() const;
    Q_SCRIPTABLE bool HasLastCapture() const;
    Q_SCRIPTABLE void Trigger(const QString &id);
    Q_SCRIPTABLE void AddArea();
    Q_SCRIPTABLE void EditArea(const QString &id);
    // Returns an error message, or an empty string on success.
    Q_SCRIPTABLE QString Rename(const QString &id, const QString &name);
    Q_SCRIPTABLE void Remove(const QString &id);
    Q_SCRIPTABLE void OpenLastCapture();
    Q_SCRIPTABLE void EditLastCapture();
    Q_SCRIPTABLE void OpenCaptureFolder();
    // Capture history (#26): JSON array, newest first; see HistoryStore::toJson.
    Q_SCRIPTABLE QString History();
    // action: open, edit, copy, folder, trash. Returns an error message or an empty string.
    Q_SCRIPTABLE QString HistoryAction(const QString &action, const QString &entryId, const QString &path);
    Q_SCRIPTABLE void ClearHistory();
    // Latency of the last Print captures (#67): JSON array, oldest first; see LatencySample::toJson.
    Q_SCRIPTABLE QString LatencyReport() const;
    // Pin-to-Screen (#28). PinMoved is called by the per-pin KWin script after the user moved a pin.
    Q_SCRIPTABLE void PinMoved(const QString &id, int x, int y);
    Q_SCRIPTABLE void PinLastCapture();
    Q_SCRIPTABLE void Configure(const QString &targetId);
    Q_SCRIPTABLE int DelaySeconds() const;
    Q_SCRIPTABLE bool KeepDelayActive() const;
    Q_SCRIPTABLE bool CountdownActive() const;
    Q_SCRIPTABLE void SetDelay(int seconds, bool keepActive);
    Q_SCRIPTABLE void CancelCountdown();
    Q_SCRIPTABLE void PickColor();
    Q_SCRIPTABLE QString Intervals() const;
    Q_SCRIPTABLE QString StartInterval(const QString &id, int seconds, int maxCaptures, int maxMinutes,
                                       bool onlyWhenChanged);
    Q_SCRIPTABLE void StopInterval(const QString &id);
    Q_SCRIPTABLE void StopAllIntervals();
    Q_SCRIPTABLE QString CaptureSync(const QString &requestJson);
    Q_SCRIPTABLE QString ListMonitors() const;
    Q_SCRIPTABLE QString ListWindows() const;
    Q_SCRIPTABLE QString LastCaptureInfo() const;
    Q_SCRIPTABLE QString ResolveTarget(const QString &query) const;
    Q_SCRIPTABLE QString AddAreaRect(const QString &name, const QString &connector, int x, int y, int width,
                                     int height);
    Q_SCRIPTABLE QString OpenOverlay(const QString &mode, int delaySeconds);
    Q_SCRIPTABLE QString McpDispatch(const QString &toolName, const QString &argumentsJson);
    // Screen recording (#30). targetId as in App::startRecording; an empty id records the last
    // target again or opens the overlay to choose one. Returns an error message or "".
    Q_SCRIPTABLE QString StartRecording(const QString &targetId, int delaySeconds);
    Q_SCRIPTABLE void StopRecording();
    // Opens the overlay with the Record drawer, to choose what to record (the widget's button).
    Q_SCRIPTABLE void OpenRecordOverlay();
    Q_SCRIPTABLE void PauseRecording(bool paused);
    // JSON object, see App::recordingState.
    Q_SCRIPTABLE QString RecordingState() const;
    // The widget calls this when it loads. While it is present, the separate tray icon steps back.
    Q_SCRIPTABLE void RegisterWidget();

Q_SIGNALS:
    Q_SCRIPTABLE void Changed();
    Q_SCRIPTABLE void IntervalsChanged();
    Q_SCRIPTABLE void HistoryChanged();
    Q_SCRIPTABLE void Captured(bool success);
    Q_SCRIPTABLE void RecordingChanged();
    void widgetActiveChanged(bool active);

private:
    App *app_;
    QDBusServiceWatcher *widgetWatcher_;
};

} // namespace glimply
