#include "recordingfinalizer.h"

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

std::optional<TailFix> planTailFix(const QString &ffprobeCsv, double targetSeconds)
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
        bool durationOk = false;
        const double duration = fields.value(1).toDouble(&durationOk);
        if (pts < lastPts) {
            // Packets out of display order (B-frames): the last packet is not the last picture,
            // and changing its duration would not hold it. glimply's encoders write none.
            return std::nullopt;
        }
        lastPts = pts;
        lastDuration = durationOk ? duration : 0;
        ++count;
    }
    if (count == 0 || lastPts < 0 || targetSeconds <= 0) {
        return std::nullopt;
    }
    const double extra = targetSeconds - lastPts;
    if (extra <= lastDuration + toleranceSeconds) {
        return std::nullopt;
    }
    return TailFix{count - 1, extra};
}

QStringList ffprobePacketArguments(const QString &path)
{
    return {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"), QStringLiteral("v:0"),
            QStringLiteral("-show_entries"), QStringLiteral("packet=pts_time,duration_time"), QStringLiteral("-of"),
            QStringLiteral("csv=p=0"), path};
}

QStringList tailFixArguments(const QString &input, const QString &output, const TailFix &fix)
{
    // setts rewrites the duration of one packet; the muxer keeps it for the last frame.
    // QString::number always writes a dot; printf would follow the locale ("3,1" breaks ffmpeg).
    const QString filter = QStringLiteral("setts=duration=if(eq(N\\,%1)\\,%2/TB_OUT\\,DURATION)")
                               .arg(fix.lastPacket)
                               .arg(QString::number(fix.extraSeconds, 'f', 6));
    return {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-y"), QStringLiteral("-i"), input,
            QStringLiteral("-map"), QStringLiteral("0"), QStringLiteral("-c"), QStringLiteral("copy"),
            QStringLiteral("-bsf:v"), filter, output};
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

void RecordingFinalizer::finalize(const QString &path, qint64 targetDurationMs)
{
    path_ = path;
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
        const std::optional<TailFix> fix = planTailFix(output, targetSeconds_);
        if (!fix) {
            finish(false, QStringLiteral("nothing to extend"));
            return;
        }
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
        finish(true, {});
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
