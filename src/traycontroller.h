#pragma once

#include "historystore.h"
#include "intervalcontroller.h"

#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

class KStatusNotifierItem;
class QMenu;
class QTimer;

namespace itsova
{

struct MenuTarget
{
    enum class Section { Group, Area, Monitor };

    Section section = Section::Area;
    QString id;
    QString name;
    QString detail; // e.g. "not connected"
    QKeySequence shortcut;
    bool available = true;
    int intervalSeconds = 5;
    bool intervalActive = false;
};

class TrayController : public QObject
{
    Q_OBJECT

public:
    using TargetProvider = std::function<QList<MenuTarget>()>;

    using HistoryProvider = std::function<QList<HistoryEntry>()>;

    explicit TrayController(TargetProvider provider, QObject *parent = nullptr);

    void setHistoryProvider(HistoryProvider provider);
    void setPinState(int count, bool shown);
    void setLastCaptureAvailable(bool available);
    void setDelayState(int seconds, bool keepActive);
    void setCountdownState(bool active, int remainingSeconds);
    void setActiveIntervals(const QList<ActiveIntervalStatus> &active);
    void setStatusText(const QString &text);
    // Screen recording (#30). state: 0 idle, 1 starting, 2 recording, 3 paused, 4 finishing
    // (ScreenRecorder::State). While a recording runs, the icon shows it and a click stops it.
    void setRecordingState(int state, qint64 elapsedMs, const QString &targetName);
    // Older KPipeWire cannot pause: the menu offers no Pause then.
    void setRecordingPauseAvailable(bool available);
    // The global record shortcut, named in the "Click to stop" tooltip (#98).
    void setRecordingStopShortcut(const QString &shortcut);
    // Replay (#104): shows "Stop Replay" in the tray menu while active.
    void setReplayActive(bool active);
    // Standby: Plasma keeps the icon in the hidden part of the tray (used while the widget is shown).
    void setStandby(bool standby);
    // TargetStore::TrayVisibility as int (#123): 0 always, 1 while something runs, 2 while recording.
    void setVisibilityMode(int mode);
    [[nodiscard]] bool isShown() const;
    // One short blink. Call only after every frame of a trigger has been captured (A2).
    void blink();

Q_SIGNALS:
    void triggerRequested(const QString &id);
    void delayChangedRequested(int seconds, bool keepActive);
    void cancelCountdownRequested();
    void toggleIntervalRequested(const QString &id);
    void stopIntervalRequested(const QString &id);
    void stopAllIntervalsRequested();
    void openLastCaptureRequested();
    void editLastCaptureRequested();
    void openFolderRequested();
    void configureRequested();
    void addAreaRequested();
    void pickColorRequested();
    void renameMonitorRequested(const QString &id);
    // action as in App::historyAction; an empty path means every file of the entry.
    void historyActionRequested(const QString &action, const QString &entryId, const QString &path);
    void clearHistoryRequested();
    void pinsShownRequested(bool shown);
    void removeAllPinsRequested();
    void stopRecordingRequested(const QString &origin);
    void stopReplayRequested();
    void quitRequested();
    void pauseRecordingRequested(bool paused);

private:
    void rebuildMenu();
    QMenu *buildHistoryMenu(QMenu *parent);
    void updateIcon();
    void applyPresence();
    void createItem();
    void updateToolTip();

    TargetProvider provider_;
    HistoryProvider historyProvider_;
    KStatusNotifierItem *item_ = nullptr; // null while hidden (#123)
    QMenu *menu_ = nullptr; // owned by item_
    int visibilityMode_ = 0;
    bool widgetActive_ = false;
    bool started_ = false;
    QTimer *blinkTimer_;
    QList<ActiveIntervalStatus> activeIntervals_;
    QString statusText_;
    int recordingState_ = 0;
    bool recordingPauseAvailable_ = true;
    qint64 recordingElapsedMs_ = 0;
    QString recordingTarget_;
    bool flashing_ = false;
    bool lastCaptureAvailable_ = false;
    int pinCount_ = 0;
    bool pinsShown_ = true;
    int delaySeconds_ = 0;
    bool keepDelayActive_ = false;
    bool countdownActive_ = false;
    int countdownRemaining_ = 0;
    QString recordingStopShortcut_;
    bool replayActive_ = false;
};

} // namespace itsova
