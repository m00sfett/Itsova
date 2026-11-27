#pragma once

#include "replayplan.h"
#include "targets.h"

#include <QList>
#include <QObject>
#include <QPoint>
#include <QString>
#include <QStringList>

#include <memory>

namespace itsova
{

class ReplaySession;

// Manages active fullscreen Replay sessions (#104):
// - One decoder per video, synchronized borderless fullscreen windows per monitor.
// - Multiple replays may run concurrently on different monitors; starting on a busy monitor replaces it.
// - Inhibits screen lock and standby while any replay is running.
class ReplayManager : public QObject
{
    Q_OBJECT

public:
    explicit ReplayManager(TargetStore &store, QObject *parent = nullptr);
    ~ReplayManager() override;

    // Starts replaying filePath. Returns an error message on failure, or empty string on success.
    // missingMonitorsOut (optional) is populated when an all-monitor recording has unplugged monitors.
    QString startReplay(const QString &filePath,
                        const QString &explicitScreen,
                        const QPoint &cursorPos,
                        QStringList *missingMonitorsOut = nullptr);

    void stopAll();
    void setShowCursor(bool show);
    void setLoop(bool loop);

    [[nodiscard]] bool isRunning() const { return !sessions_.isEmpty(); }

Q_SIGNALS:
    void activeChanged(bool active);
    void settingsChanged();

private:
    void updateInhibition();
    void removeSession(ReplaySession *session);

    TargetStore &store_;
    QList<ReplaySession *> sessions_;
    quint32 screenSaverCookie_ = 0;
    quint32 powerInhibitCookie_ = 0;
};

} // namespace itsova
