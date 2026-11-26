#pragma once

#include "recordingplan.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>

class PipeWireRecord;

namespace glimply
{

class RecordingFinalizer;
class ScreencastManager;
class ScreencastStream;

// Records one target to a file (#30). KWin hands out a PipeWire stream of a region or a window
// (zkde_screencast_unstable_v1, granted to glimply through its desktop file) and KPipeWire encodes
// it. Nothing here runs before the first recording, so still captures and the Print path never
// wait for PipeWire.
class ScreenRecorder : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Idle,
        Starting, // waiting for KWin's stream and the encoder
        Recording,
        Paused,
        Finishing, // stopped; the encoder writes the last frames
    };
    Q_ENUM(State)

    struct Options
    {
        RecordingFormat format = RecordingFormat::Mp4;
        int frameRate = 30;
        bool includeCursor = true;
        QString path; // the file to write
    };

    explicit ScreenRecorder(QObject *parent = nullptr);
    ~ScreenRecorder() override;

    // Returns false (and emits failed) when a recording is already running or KWin offers no
    // screen streams to glimply.
    bool start(const RecordingTarget &target, const Options &options);
    // origin names who asked (tray, shortcut, D-Bus, quit, …) for the journal (#92).
    void stop(const QString &origin);
    // KPipeWire before 6.4 cannot pause; the tray, widget and shortcut leave pausing out then.
    [[nodiscard]] static bool canPause();
    void setPaused(bool paused);
    void togglePause();

    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] bool isBusy() const { return state_ != State::Idle; }
    [[nodiscard]] qint64 elapsedMs() const;
    [[nodiscard]] const RecordingTarget &target() const { return target_; }
    [[nodiscard]] QString path() const { return options_.path; }

Q_SIGNALS:
    void stateChanged(glimply::ScreenRecorder::State state);
    // The file is complete and playable.
    void finished(const QString &path, const glimply::RecordingTarget &target, qint64 durationMs);
    void failed(const QString &error);

private:
    void onStreamReady(quint64 objectSerial, quint32 nodeId);
    void onStreamFailed(const QString &error);
    void onStreamClosed();
    void onEncoderStateChanged();
    void fail(const QString &error);
    void setState(State state);
    void cleanUp();
    void finalizeFile();

    State state_ = State::Idle;
    RecordingTarget target_;
    Options options_;
    std::unique_ptr<ScreencastManager> manager_;
    std::unique_ptr<ScreencastStream> stream_;
    std::unique_ptr<PipeWireRecord> encoder_;
    std::unique_ptr<RecordingFinalizer> finalizer_;
    QElapsedTimer monotonic_;
    RecordingClock clock_;
    qint64 finalDurationMs_ = 0;
    bool stopRequested_ = false;
    bool finalizing_ = false;
};

} // namespace glimply
