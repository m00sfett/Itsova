#include "recordingfinalizer.h"

#include "recordingmetadata.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <cstdio>

namespace glimply
{

namespace
{
constexpr int processTimeoutMs = 120000;
// A tail shorter than this is left alone: it is within the last frame's own duration.
constexpr double toleranceSeconds = 0.1;
} // namespace

namespace
{

// A pause takes effect from its middle: frames before it are earlier, frames after it later, so
// a small offset between glimply's clock and the stream's does not matter.
double pauseThreshold(const RecordingPause &pause)
{
    return (pause.start + pause.end) / 2;
}

double shifted(double pts, const QList<RecordingPause> &pauses)
{
    double shift = 0;
    for (const RecordingPause &pause : pauses) {
        if (pts >= pauseThreshold(pause)) {
            shift += pause.end - pause.start;
        }
    }
    return pts - shift;
}

} // namespace

std::optional<TailFix> planTailFix(const QString &ffprobeCsv, double targetSeconds, const QList<RecordingPause> &pauses)
{
    int count = 0;
    double lastPts = -1;
    double lastDuration = 0;
    const QStringList lines = ffprobeCsv.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &raw : lines) {
        const QStringList fields = raw.trimmed().split(QLatin1Char(','));
        bool ok = false;
        const double pts = fields.value(0).toDouble(&ok);
        if (!ok) {
            continue; // packets without a timestamp ("N/A")
        }
        if (pts < lastPts) {
            // Packets out of display order (B-frames): the last packet is not the last picture,
            // and changing its duration would not hold it. glimply's encoders write none.
            return std::nullopt;
        }
        bool durationOk = false;
        const double duration = fields.value(1).toDouble(&durationOk);
        lastPts = pts;
        lastDuration = durationOk ? duration : 0;
        ++count;
    }
    if (count == 0 || lastPts < 0 || targetSeconds <= 0) {
        return std::nullopt;
    }
    TailFix fix;
    fix.lastPacket = count - 1;
    // Only pauses that frames follow need cutting.
    for (const RecordingPause &pause : pauses) {
        if (pause.end > pause.start && lastPts >= pauseThreshold(pause)) {
            fix.pauses.append(pause);
        }
    }
    const double extra = targetSeconds - shifted(lastPts, fix.pauses);
    if (extra > lastDuration + toleranceSeconds) {
        fix.extraSeconds = extra;
    }
    if (fix.extraSeconds <= 0 && fix.pauses.isEmpty()) {
        return std::nullopt;
    }
    return fix;
}

QStringList ffprobePacketArguments(const QString &path)
{
    return {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"), QStringLiteral("v:0"),
            QStringLiteral("-show_entries"), QStringLiteral("packet=pts_time,duration_time"), QStringLiteral("-of"),
            QStringLiteral("csv=p=0"), path};
}

QStringList tailFixArguments(const QString &input, const QString &output, const TailFix &fix)
{
    // setts rewrites timestamps and the duration of one packet; with stream copy the time base
    // stays, so TB_OUT converts seconds. QString::number always writes a dot; printf would follow
    // the locale ("3,1" breaks ffmpeg).
    const auto number = [](double value) { return QString::number(value, 'f', 6); };
    QStringList parts;
    if (!fix.pauses.isEmpty()) {
        const auto shift = [&](const QString &ts) {
            QStringList terms;
            for (const RecordingPause &pause : fix.pauses) {
                terms.append(QStringLiteral("gte(%1*TB_OUT\\,%2)*%3")
                                 .arg(ts, number(pauseThreshold(pause)), number(pause.end - pause.start)));
            }
            return QStringLiteral("%1-(%2)/TB_OUT").arg(ts, terms.join(QLatin1Char('+')));
        };
        parts.append(QStringLiteral("pts=") + shift(QStringLiteral("PTS")));
        parts.append(QStringLiteral("dts=") + shift(QStringLiteral("DTS")));
    }
    if (fix.extraSeconds > 0) {
        parts.append(QStringLiteral("duration=if(eq(N\\,%1)\\,%2/TB_OUT\\,DURATION)")
                         .arg(fix.lastPacket)
                         .arg(number(fix.extraSeconds)));
    }
    QStringList args{QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-y"), QStringLiteral("-i"), input,
                     QStringLiteral("-map"), QStringLiteral("0"), QStringLiteral("-c"), QStringLiteral("copy")};
    if (!parts.isEmpty()) {
        args << QStringLiteral("-bsf:v") << QStringLiteral("setts=") + parts.join(QLatin1Char(':'));
    }
    args += recordingMetadataArguments(output, fix.metadata);
    args << output;
    return args;
}

RecordingFinalizer::RecordingFinalizer(QObject *parent)
    : QObject(parent)
{
}

RecordingFinalizer::~RecordingFinalizer()
{
    if (process_) {
        process_->disconnect(this);
        process_->kill();
        process_->waitForFinished(1000);
    }
    if (!temporary_.isEmpty()) {
        QFile::remove(temporary_);
    }
}

void RecordingFinalizer::finalize(const QString &path, qint64 targetDurationMs, const QList<RecordingPause> &pauses,
                                  const QString &metadata)
{
    path_ = path;
    pauses_ = pauses;
    metadata_ = metadata;
    targetSeconds_ = double(targetDurationMs) / 1000.0;
    finished_ = false;
    const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    if (ffprobe.isEmpty() || QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
        finish(false, QStringLiteral("ffmpeg/ffprobe not found"));
        return;
    }
    process_ = new QProcess(this);
    connect(process_, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        const QString output = QString::fromUtf8(process_->readAllStandardOutput());
        process_->deleteLater();
        process_ = nullptr;
        if (status != QProcess::NormalExit || exitCode != 0) {
            finish(false, QStringLiteral("ffprobe failed"));
            return;
        }
        std::optional<TailFix> fix = planTailFix(output, targetSeconds_, pauses_);
        const bool tagged = !recordingMetadataArguments(path_, metadata_).isEmpty();
        if (!fix && !tagged) {
            finish(false, QStringLiteral("nothing to extend"));
            return;
        }
        if (!fix) {
            fix = TailFix{}; // only the metadata (#103)
        }
        fix->metadata = metadata_;
        runFix(*fix);
    });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            finish(false, QStringLiteral("ffprobe did not start"));
        }
    });
    QTimer::singleShot(processTimeoutMs, this, [this, process = process_] {
        if (process_ == process && process_) {
            process_->kill();
        }
    });
    process_->start(ffprobe, ffprobePacketArguments(path_));
}

void RecordingFinalizer::runFix(const TailFix &fix)
{
    currentFix_ = fix;
    const QFileInfo info(path_);
    temporary_ = info.dir().filePath(QStringLiteral(".%1.part.%2").arg(info.completeBaseName(), info.suffix()));
    process_ = new QProcess(this);
    connect(process_, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        process_->deleteLater();
        process_ = nullptr;
        const QFileInfo temporary(temporary_);
        if (status != QProcess::NormalExit || exitCode != 0 || !temporary.exists() || temporary.size() == 0) {
            QFile::remove(temporary_);
            temporary_.clear();
            finish(false, QStringLiteral("ffmpeg failed"));
            return;
        }
        // rename(2) replaces the original in one step.
        if (std::rename(QFile::encodeName(temporary_).constData(), QFile::encodeName(path_).constData()) != 0) {
            QFile::remove(temporary_);
            temporary_.clear();
            finish(false, QStringLiteral("could not replace the file"));
            return;
        }
        temporary_.clear();
        const bool extended = currentFix_.extraSeconds > 0 || !currentFix_.pauses.isEmpty();
        finish(extended, extended ? QString() : QStringLiteral("metadata written, nothing to extend"));
    });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            finish(false, QStringLiteral("ffmpeg did not start"));
        }
    });
    QTimer::singleShot(processTimeoutMs, this, [this, process = process_] {
        if (process_ == process && process_) {
            process_->kill();
        }
    });
    process_->start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")),
                    tailFixArguments(path_, temporary_, fix));
}

void RecordingFinalizer::finish(bool extended, const QString &note)
{
    if (finished_) {
        return;
    }
    finished_ = true;
    Q_EMIT done(extended, note);
}

} // namespace glimply
