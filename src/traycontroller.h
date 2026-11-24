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

namespace glimply
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
    void setLastCaptureAvailable(bool available);
    void setDelayState(int seconds, bool keepActive);
    void setCountdownState(bool active, int remainingSeconds);
    void setActiveIntervals(const QList<ActiveIntervalStatus> &active);
    void setStatusText(const QString &text);
    // Standby: Plasma keeps the icon in the hidden part of the tray (used while the widget is shown).
    void setStandby(bool standby);
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

private:
    void rebuildMenu();
    QMenu *buildHistoryMenu(QMenu *parent);
    void updateIcon();
    void updateToolTip();

    TargetProvider provider_;
    HistoryProvider historyProvider_;
    KStatusNotifierItem *item_;
    QMenu *menu_;
    QTimer *blinkTimer_;
    QList<ActiveIntervalStatus> activeIntervals_;
    QString statusText_;
    bool flashing_ = false;
    bool lastCaptureAvailable_ = false;
    int delaySeconds_ = 0;
    bool keepDelayActive_ = false;
    bool countdownActive_ = false;
    int countdownRemaining_ = 0;
};

} // namespace glimply
