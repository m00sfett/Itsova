#include "recordingfinalizer.h"

#include "recordingmetadata.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace itsova
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
// a small offset between itsova's clock and the stream's does not matter.
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
            // and changing its duration would not hold it. itsova's encoders write none.
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

QString letterboxFilterGraph(const QSize &canvasSize, const QList<WindowResizeSample> &samples)
{
    if (canvasSize.width() < 2 || canvasSize.height() < 2 || samples.isEmpty()) {
        return {};
    }
    const double canvasW = double(canvasSize.width());
    const double canvasH = double(canvasSize.height());
    const double canvasAspect = canvasW / canvasH;

    // Collapse consecutive identical sizes and drop invalid samples.
    QList<WindowResizeSample> clean;
    for (const WindowResizeSample &s : samples) {
        if (s.size.width() < 2 || s.size.height() < 2) {
            continue;
        }
        const double t = std::max(0.0, s.timeSeconds);
        if (!clean.isEmpty() && clean.last().size == s.size) {
            continue;
        }
        if (!clean.isEmpty() && t - clean.last().timeSeconds < 0.05) {
            clean.last().size = s.size;
            continue;
        }
        clean.append({t, s.size});
    }
    if (clean.isEmpty()) {
        return {};
    }
    clean.first().timeSeconds = 0.0;

    bool needsLetterbox = false;
    for (const WindowResizeSample &s : clean) {
        const double aspect = double(s.size.width()) / double(s.size.height());
        if (std::abs(aspect - canvasAspect) / canvasAspect > 0.015) {
            needsLetterbox = true;
            break;
        }
    }
    if (!needsLetterbox) {
        return {};
    }

    const auto num = [](double v) { return QString::number(v, 'f', 3); };
    QStringList chains;
    QStringList concatInputs;
    for (int i = 0; i < clean.size(); ++i) {
        const WindowResizeSample &seg = clean.at(i);
        QString trim = QStringLiteral("trim=start=%1").arg(num(seg.timeSeconds));
        if (i + 1 < clean.size()) {
            trim += QStringLiteral(":end=%1").arg(num(clean.at(i + 1).timeSeconds));
        }
        const double sw = double(seg.size.width());
        const double sh = double(seg.size.height());
        const double scale = std::min(canvasW / sw, canvasH / sh);
        int fitW = std::max(2, int(std::lround(sw * scale)) & ~1);
        int fitH = std::max(2, int(std::lround(sh * scale)) & ~1);
        fitW = std::min(fitW, canvasSize.width());
        fitH = std::min(fitH, canvasSize.height());
        const int padX = (canvasSize.width() - fitW) / 2;
        const int padY = (canvasSize.height() - fitH) / 2;

        const QString label = QStringLiteral("[v%1]").arg(i);
        concatInputs.append(label);
        if (fitW == canvasSize.width() && fitH == canvasSize.height()) {
            chains.append(QStringLiteral("[0:v]%1,setpts=PTS-STARTPTS,setsar=1%2").arg(trim, label));
        } else {
            chains.append(
                QStringLiteral("[0:v]%1,setpts=PTS-STARTPTS,scale=%2:%3,pad=%4:%5:%6:%7:black,setsar=1%8")
                    .arg(trim)
                    .arg(fitW)
                    .arg(fitH)
                    .arg(canvasSize.width())
                    .arg(canvasSize.height())
                    .arg(padX)
                    .arg(padY)
                    .arg(label));
        }
    }
    chains.append(QStringLiteral("%1concat=n=%2:v=1:a=0[outv]").arg(concatInputs.join(QString())).arg(clean.size()));
    return chains.join(QLatin1Char(';'));
}

QStringList letterboxArguments(const QString &input, const QString &output, const QString &filterGraph,
                               const QString &metadata)
{
    QStringList args{QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-y"), QStringLiteral("-i"), input,
                     QStringLiteral("-filter_complex"), filterGraph, QStringLiteral("-map"), QStringLiteral("[outv]")};
    const QString ext = QFileInfo(output).suffix().toLower();
    if (ext == QLatin1String("webm")) {
        args << QStringLiteral("-c:v") << QStringLiteral("libvpx-vp9") << QStringLiteral("-b:v") << QStringLiteral("0")
             << QStringLiteral("-crf") << QStringLiteral("30");
    } else if (ext != QLatin1String("gif")) {
        args << QStringLiteral("-c:v") << QStringLiteral("libx264") << QStringLiteral("-preset")
             << QStringLiteral("veryfast") << QStringLiteral("-crf") << QStringLiteral("20")
             << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
    }
    args += recordingMetadataArguments(output, metadata);
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
                                  const QString &metadata, const QList<WindowResizeSample> &resizeSamples)
{
    path_ = path;
    pauses_ = pauses;
    resizeSamples_ = resizeSamples;
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
            if (!resizeSamples_.isEmpty()) {
                probeSizeAndLetterbox(false, QStringLiteral("nothing to extend"));
            } else {
                finish(false, QStringLiteral("nothing to extend"));
            }
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
        const QString note = extended ? QString() : QStringLiteral("metadata written, nothing to extend");
        if (!resizeSamples_.isEmpty()) {
            probeSizeAndLetterbox(extended, note);
        } else {
            finish(extended, note);
        }
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

void RecordingFinalizer::probeSizeAndLetterbox(bool alreadyExtended, const QString &priorNote)
{
    const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    process_ = new QProcess(this);
    connect(process_, &QProcess::finished, this,
            [this, alreadyExtended, priorNote](int exitCode, QProcess::ExitStatus status) {
                const QString out = QString::fromUtf8(process_->readAllStandardOutput()).trimmed();
                process_->deleteLater();
                process_ = nullptr;
                if (status != QProcess::NormalExit || exitCode != 0) {
                    finish(alreadyExtended, priorNote);
                    return;
                }
                const QStringList dims = out.split(QLatin1Char(','));
                if (dims.size() < 2) {
                    finish(alreadyExtended, priorNote);
                    return;
                }
                const QSize canvasSize(dims.at(0).toInt(), dims.at(1).toInt());
                const QString graph = letterboxFilterGraph(canvasSize, resizeSamples_);
                if (graph.isEmpty()) {
                    finish(alreadyExtended, priorNote);
                    return;
                }
                runLetterbox(graph, alreadyExtended, priorNote);
            });
    connect(process_, &QProcess::errorOccurred, this, [this, alreadyExtended, priorNote](QProcess::ProcessError err) {
        if (err == QProcess::FailedToStart) {
            finish(alreadyExtended, priorNote);
        }
    });
    process_->start(ffprobe,
                    {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"),
                     QStringLiteral("v:0"), QStringLiteral("-show_entries"), QStringLiteral("stream=width,height"),
                     QStringLiteral("-of"), QStringLiteral("csv=p=0"), path_});
}

void RecordingFinalizer::runLetterbox(const QString &filterGraph, bool alreadyExtended, const QString &priorNote)
{
    const QFileInfo info(path_);
    temporary_ = info.dir().filePath(QStringLiteral(".%1.letterbox.%2").arg(info.completeBaseName(), info.suffix()));
    process_ = new QProcess(this);
    connect(process_, &QProcess::finished, this,
            [this, alreadyExtended, priorNote](int exitCode, QProcess::ExitStatus status) {
                process_->deleteLater();
                process_ = nullptr;
                const QFileInfo temporary(temporary_);
                if (status != QProcess::NormalExit || exitCode != 0 || !temporary.exists() || temporary.size() == 0) {
                    QFile::remove(temporary_);
                    temporary_.clear();
                    finish(alreadyExtended, priorNote);
                    return;
                }
                if (std::rename(QFile::encodeName(temporary_).constData(), QFile::encodeName(path_).constData()) != 0) {
                    QFile::remove(temporary_);
                    temporary_.clear();
                    finish(alreadyExtended, priorNote);
                    return;
                }
                temporary_.clear();
                finish(true, QStringLiteral("letterboxed resized window"));
            });
    connect(process_, &QProcess::errorOccurred, this, [this, alreadyExtended, priorNote](QProcess::ProcessError err) {
        if (err == QProcess::FailedToStart) {
            finish(alreadyExtended, priorNote);
        }
    });
    QTimer::singleShot(processTimeoutMs, this, [this, process = process_] {
        if (process_ == process && process_) {
            process_->kill();
        }
    });
    process_->start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")),
                    letterboxArguments(path_, temporary_, filterGraph, metadata_));
}

void RecordingFinalizer::finish(bool extended, const QString &note)
{
    if (finished_) {
        return;
    }
    finished_ = true;
    Q_EMIT done(extended, note);
}

} // namespace itsova
