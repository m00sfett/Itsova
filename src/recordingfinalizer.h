#pragma once

#include "recordingaudio.h"

#include <QList>
#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

class QProcess;

namespace itsova
{

// A pause, in seconds after the start of the recording. KPipeWire drops the frames of a pause but
// keeps the clock running, so the frames after it are shifted back by its length.
struct RecordingPause
{
    double start = 0;
    double end = 0;
};

// A window size observed at a recording-clock timestamp (seconds, pauses excluded) (#127).
struct WindowResizeSample
{
    double timeSeconds = 0;
    QSize size;
};

struct TailFix
{
    int lastPacket = -1; // index of the last video packet
    double extraSeconds = 0; // new display duration of that packet; 0 keeps it
    QList<RecordingPause> pauses; // pauses to cut out
    QString metadata; // #103: JSON written as the container tag "itsova"; empty writes none
};

// ffprobeCsv: "pts_time,duration_time" per video packet (ffprobe -of csv=p=0). targetSeconds is the
// recorded time without pauses. Returns nothing when the file already lasts that long (within a
// frame), has no pause to cut, or the output cannot be read.
[[nodiscard]] std::optional<TailFix> planTailFix(const QString &ffprobeCsv, double targetSeconds,
                                                 const QList<RecordingPause> &pauses = {});
[[nodiscard]] QStringList ffprobePacketArguments(const QString &path);
[[nodiscard]] QStringList tailFixArguments(const QString &input, const QString &output, const TailFix &fix);
// Builds an ffmpeg filter_complex graph that letterboxes segments where a recorded window's aspect
// ratio diverged from canvasSize (#127). Returns empty when no letterboxing is needed.
[[nodiscard]] QString letterboxFilterGraph(const QSize &canvasSize, const QList<WindowResizeSample> &samples);
[[nodiscard]] QStringList letterboxArguments(const QString &input, const QString &output, const QString &filterGraph,
                                             const QString &metadata);

class RecordingFinalizer : public QObject
{
    Q_OBJECT

public:
    explicit RecordingFinalizer(QObject *parent = nullptr);
    ~RecordingFinalizer() override;

    // Holds sparse video through the recording end and adds audio on the edited video clock.
    // Runs asynchronously; emits done exactly once. Without ffmpeg/ffprobe the file stays as it is.
    // metadata (#103) is written into the file in the same pass; it alone is reason for a pass.
    void finalize(const QString &path, qint64 targetDurationMs, const QList<RecordingPause> &pauses = {},
                  const QString &metadata = QString(), const QList<WindowResizeSample> &resizeSamples = {},
                  const QList<RecordingAudioTrack> &audioTracks = {});

Q_SIGNALS:
    // extended: the last frame now lasts until the end. note: why nothing changed, for the log.
    void done(bool extended, const QString &note);

private:
    void probePackets(bool letterboxed, const QString &note = {});
    void runFix(const TailFix &fix, bool letterboxed, const QString &note);
    void probeSizeAndLetterbox(bool alreadyExtended, const QString &priorNote);
    void runLetterbox(const QString &filterGraph, bool alreadyExtended, const QString &priorNote);
    void muxAudio(bool extended, const QString &note);
    void startProcess(const QString &program, const QStringList &args,
                      const std::function<void(bool, const QByteArray &)> &completed);
    bool openTemporary(const QString &purpose);
    bool replaceWithTemporary(bool success);
    void finish(bool extended, const QString &note);

    QProcess *process_ = nullptr;
    QString path_;
    QString temporary_;
    double targetSeconds_ = 0;
    QList<RecordingPause> pauses_;
    QList<WindowResizeSample> resizeSamples_;
    QList<RecordingAudioTrack> audioTracks_;
    QString metadata_;
    bool finished_ = true;
};

} // namespace itsova
