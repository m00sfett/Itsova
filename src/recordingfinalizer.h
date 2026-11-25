#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <optional>

class QProcess;

namespace glimply
{

// KWin sends a frame only when the screen changes, so a recording that ends on a still picture
// would stop at the last change (#30). The finalizer holds the last frame until the end of the
// recording by rewriting the container timing with ffmpeg (stream copy, never re-encoding).
struct TailFix
{
    int lastPacket = -1; // index of the last video packet
    double extraSeconds = 0; // new display duration of that packet
};

// ffprobeCsv: "pts_time,duration_time" per video packet (ffprobe -of csv=p=0). Returns nothing when
// the file already lasts targetSeconds (within a frame) or the output cannot be read.
[[nodiscard]] std::optional<TailFix> planTailFix(const QString &ffprobeCsv, double targetSeconds);
[[nodiscard]] QStringList ffprobePacketArguments(const QString &path);
[[nodiscard]] QStringList tailFixArguments(const QString &input, const QString &output, const TailFix &fix);

class RecordingFinalizer : public QObject
{
    Q_OBJECT

public:
    explicit RecordingFinalizer(QObject *parent = nullptr);
    ~RecordingFinalizer() override;

    // Runs asynchronously; emits done exactly once. Without ffmpeg/ffprobe the file stays as it is.
    void finalize(const QString &path, qint64 targetDurationMs);

Q_SIGNALS:
    // extended: the last frame now lasts until the end. note: why nothing changed, for the log.
    void done(bool extended, const QString &note);

private:
    void runFix(const TailFix &fix);
    void finish(bool extended, const QString &note);

    QProcess *process_ = nullptr;
    QString path_;
    QString temporary_;
    double targetSeconds_ = 0;
    bool finished_ = true;
};

} // namespace glimply
