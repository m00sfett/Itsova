#include "recordingfinalizer.h"

#include "recordingmetadata.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryFile>
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

struct SampleRange
{
    qint64 start;
    qint64 end;
};

QList<SampleRange> keptSamples(double targetSeconds, const QList<RecordingPause> &pauses)
{
    const qint64 target = qRound64(targetSeconds * recordingAudioRate);
    QList<SampleRange> ranges;
    QList<RecordingPause> ordered = pauses;
    std::sort(ordered.begin(), ordered.end(), [](const RecordingPause &a, const RecordingPause &b) {
        return a.start < b.start;
    });
    qint64 raw = 0;
    qint64 remaining = target;
    for (const RecordingPause &pause : ordered) {
        const qint64 end = qRound64(pause.end * recordingAudioRate);
        if (pause.end <= pause.start || end <= raw) {
            continue;
        }
        const qint64 start = qRound64(pause.start * recordingAudioRate);
        const qint64 kept = std::clamp(start - raw, qint64(0), remaining);
        if (kept > 0) {
            ranges.append({raw, raw + kept});
            remaining -= kept;
        }
        if (remaining == 0) {
            return ranges;
        }
        raw = end;
    }
    if (remaining > 0) {
        ranges.append({raw, raw + remaining});
    }
    return ranges;
}

QString audioFilter(const RecordingAudioTrack &track, const QList<SampleRange> &kept, int input)
{
    const QFileInfo info(track.path);
    if (!info.isFile() || !info.isReadable()) {
        return {};
    }
    const qint64 trackStart = qRound64(static_cast<double>(track.startUs) * recordingAudioRate / 1000000);
    const qint64 trackEnd = trackStart + info.size() / recordingAudioFrameBytes;
    QStringList chains;
    QStringList segments;
    qint64 edited = 0;
    qint64 lead = 0;
    for (const SampleRange &range : kept) {
        const qint64 start = std::max(range.start, trackStart);
        const qint64 end = std::min(range.end, trackEnd);
        if (start < end) {
            if (segments.isEmpty()) {
                lead = edited + start - range.start;
            }
            const QString label = QStringLiteral("[a%1_%2]").arg(input).arg(segments.size());
            chains.append(QStringLiteral("[%1:a]atrim=start_sample=%2:end_sample=%3,asetpts=N/SR/TB%4")
                              .arg(input)
                              .arg(start - trackStart)
                              .arg(end - trackStart)
                              .arg(label));
            segments.append(label);
        }
        edited += range.end - range.start;
    }
    if (segments.isEmpty()) {
        return {};
    }
    QString output = segments.join(QString());
    if (segments.size() > 1) {
        output += QStringLiteral("concat=n=%1:v=0:a=1,").arg(segments.size());
    }
    output += lead > 0 ? QStringLiteral("adelay=%1S:all=1").arg(lead) : QStringLiteral("anull");
    output += QStringLiteral("[outa%1]").arg(input);
    chains.append(output);
    return chains.join(QLatin1Char(';'));
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
    QString width;
    QString height;
    for (qsizetype i = clean.size(); i-- > 0;) {
        const WindowResizeSample &seg = clean.at(i);
        const double sw = double(seg.size.width());
        const double sh = double(seg.size.height());
        const double scale = std::min(canvasW / sw, canvasH / sh);
        const int fitW = std::clamp(int(std::lround(sw * scale)) & ~1, 2, canvasSize.width());
        const int fitH = std::clamp(int(std::lround(sh * scale)) & ~1, 2, canvasSize.height());
        const auto dimension = [&](int fit, const QString &later) {
            const QString value = QString::number(fit);
            if (later.isEmpty() || later == value) {
                return value;
            }
            return QStringLiteral("if(lt(t,%1),%2,%3)").arg(num(clean.at(i + 1).timeSeconds), value, later);
        };
        width = dimension(fitW, width);
        height = dimension(fitH, height);
    }
    // One chain scales each sparse frame at its existing PTS without queuing other resize segments.
    return QStringLiteral("[0:v]scale=w='%1':h='%2':eval=frame,"
                          "pad=w=%3:h=%4:x='(ow-iw)/2':y='(oh-ih)/2':color=black:eval=frame,setsar=1[outv]")
        .arg(width, height)
        .arg(canvasSize.width())
        .arg(canvasSize.height());
}

QStringList letterboxArguments(const QString &input, const QString &output, const QString &filterGraph,
                               const QString &metadata)
{
    QStringList args{QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-y"), QStringLiteral("-i"), input,
                     QStringLiteral("-filter_complex"), filterGraph, QStringLiteral("-map"), QStringLiteral("[outv]"),
                     QStringLiteral("-fps_mode:v"), QStringLiteral("passthrough"),
                     QStringLiteral("-enc_time_base:v"), QStringLiteral("filter")};
    const QString ext = QFileInfo(output).suffix().toLower();
    if (ext != QLatin1String("gif")) {
        args << QStringLiteral("-map") << QStringLiteral("0:a?") << QStringLiteral("-c:a") << QStringLiteral("copy");
    }
    if (ext == QLatin1String("webm")) {
        args << QStringLiteral("-c:v") << QStringLiteral("libvpx-vp9") << QStringLiteral("-b:v") << QStringLiteral("0")
             << QStringLiteral("-crf") << QStringLiteral("30");
    } else if (ext != QLatin1String("gif")) {
        args << QStringLiteral("-c:v") << QStringLiteral("libx264") << QStringLiteral("-preset")
             << QStringLiteral("veryfast") << QStringLiteral("-crf") << QStringLiteral("20")
             << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p") << QStringLiteral("-bf") << QStringLiteral("0");
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
                                  const QString &metadata, const QList<WindowResizeSample> &resizeSamples,
                                  const QList<RecordingAudioTrack> &audioTracks)
{
    path_ = path;
    pauses_ = pauses;
    resizeSamples_ = resizeSamples;
    audioTracks_ = audioTracks;
    metadata_ = metadata;
    targetSeconds_ = double(targetDurationMs) / 1000.0;
    finished_ = false;
    if (QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty()
        || QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
        finish(false, QStringLiteral("ffmpeg/ffprobe not found"));
        return;
    }
    probePackets(false);
}

void RecordingFinalizer::probePackets(bool letterboxed, const QString &note)
{
    startProcess(QStringLiteral("ffprobe"), ffprobePacketArguments(path_),
                 [this, letterboxed, note](bool success, const QByteArray &output) {
                     if (!success) {
                         finish(letterboxed, QStringLiteral("ffprobe failed"));
                         return;
                     }
                     const QString csv = QString::fromUtf8(output);
                     // Re-encoding keeps frame PTS but may discard packet durations, including
                     // the held final frame. The pause edits are already in the letterboxed file.
                     std::optional<TailFix> fix = letterboxed ? planTailFix(csv, targetSeconds_)
                                                              : planTailFix(csv, targetSeconds_, pauses_);
                     if (!fix && !letterboxed && !recordingMetadataArguments(path_, metadata_).isEmpty()) {
                         fix = TailFix{}; // only the metadata (#103)
                     }
                     if (fix) {
                         fix->metadata = metadata_;
                         runFix(*fix, letterboxed, note);
                     } else if (!letterboxed && !resizeSamples_.isEmpty()) {
                         probeSizeAndLetterbox(false, QStringLiteral("nothing to extend"));
                     } else {
                         muxAudio(letterboxed, letterboxed ? note : QStringLiteral("nothing to extend"));
                     }
                 });
}

void RecordingFinalizer::runFix(const TailFix &fix, bool letterboxed, const QString &note)
{
    if (!openTemporary(QStringLiteral("part"))) {
        finish(letterboxed, QStringLiteral("could not create a temporary recording"));
        return;
    }
    const bool extended = fix.extraSeconds > 0 || !fix.pauses.isEmpty();
    startProcess(QStringLiteral("ffmpeg"), tailFixArguments(path_, temporary_, fix),
                 [this, extended, letterboxed, note](bool success, const QByteArray &) {
                     if (!replaceWithTemporary(success)) {
                         finish(letterboxed, QStringLiteral("could not finalize video timing"));
                         return;
                     }
                     if (letterboxed) {
                         muxAudio(true, note);
                         return;
                     }
                     const QString result = extended ? QString() : QStringLiteral("metadata written, nothing to extend");
                     if (resizeSamples_.isEmpty()) {
                         muxAudio(extended, result);
                     } else {
                         probeSizeAndLetterbox(extended, result);
                     }
                 });
}

void RecordingFinalizer::probeSizeAndLetterbox(bool alreadyExtended, const QString &priorNote)
{
    startProcess(QStringLiteral("ffprobe"),
                 {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"),
                  QStringLiteral("v:0"), QStringLiteral("-show_entries"), QStringLiteral("stream=width,height"),
                  QStringLiteral("-of"), QStringLiteral("csv=p=0"), path_},
                 [this, alreadyExtended, priorNote](bool success, const QByteArray &output) {
                     const QStringList dims = QString::fromUtf8(output).trimmed().split(QLatin1Char(','));
                     if (!success || dims.size() < 2) {
                         muxAudio(alreadyExtended, priorNote);
                         return;
                     }
                     const QSize canvasSize(dims.at(0).toInt(), dims.at(1).toInt());
                     const QString graph = letterboxFilterGraph(canvasSize, resizeSamples_);
                     if (graph.isEmpty()) {
                         muxAudio(alreadyExtended, priorNote);
                     } else {
                         runLetterbox(graph, alreadyExtended, priorNote);
                     }
                 });
}

void RecordingFinalizer::runLetterbox(const QString &filterGraph, bool alreadyExtended, const QString &priorNote)
{
    if (!openTemporary(QStringLiteral("letterbox"))) {
        muxAudio(alreadyExtended, priorNote);
        return;
    }
    startProcess(QStringLiteral("ffmpeg"), letterboxArguments(path_, temporary_, filterGraph, metadata_),
                 [this, alreadyExtended, priorNote](bool success, const QByteArray &) {
                     if (!replaceWithTemporary(success)) {
                         muxAudio(alreadyExtended, priorNote);
                         return;
                     }
                     probePackets(true, QStringLiteral("letterboxed resized window"));
                 });
}

void RecordingFinalizer::muxAudio(bool extended, const QString &note)
{
    const QString ext = QFileInfo(path_).suffix().toLower();
    if (audioTracks_.isEmpty() || targetSeconds_ <= 0
        || (ext != QLatin1String("mp4") && ext != QLatin1String("webm"))) {
        finish(extended, note);
        return;
    }
    const QList<SampleRange> kept = keptSamples(targetSeconds_, pauses_);
    QStringList inputs{QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-y"), QStringLiteral("-i"),
                       path_};
    QStringList filters;
    QStringList maps{QStringLiteral("-map"), QStringLiteral("0:v:0")};
    QStringList codecs{QStringLiteral("-c"), QStringLiteral("copy")};
    const bool mp4 = ext == QLatin1String("mp4");
    for (const RecordingAudioTrack &track : audioTracks_) {
        const int index = filters.size();
        const int input = index + 1;
        const QString filter = audioFilter(track, kept, input);
        if (filter.isEmpty()) {
            continue;
        }
        filters.append(filter);
        inputs << QStringLiteral("-f") << QStringLiteral("s16le") << QStringLiteral("-ar")
               << QString::number(recordingAudioRate) << QStringLiteral("-ac") << QString::number(recordingAudioChannels)
               << QStringLiteral("-i") << track.path;
        maps << QStringLiteral("-map") << QStringLiteral("[outa%1]").arg(input);
        codecs << QStringLiteral("-c:a:%1").arg(index) << (mp4 ? QStringLiteral("aac") : QStringLiteral("libopus"))
               << QStringLiteral("-metadata:s:a:%1").arg(index)
               << (mp4 ? QStringLiteral("handler_name=") : QStringLiteral("title=")) + track.title;
    }
    if (filters.isEmpty()) {
        finish(extended, QStringLiteral("no recorded audio samples"));
        return;
    }
    if (!openTemporary(QStringLiteral("audio"))) {
        finish(extended, QStringLiteral("could not create a temporary recording for audio"));
        return;
    }
    maps << QStringLiteral("-map") << QStringLiteral("0:a?");
    QStringList args = inputs;
    args << QStringLiteral("-filter_complex") << filters.join(QLatin1Char(';'));
    args += maps;
    args += codecs;
    args += recordingMetadataArguments(temporary_, metadata_);
    args << temporary_;
    startProcess(QStringLiteral("ffmpeg"), args, [this, extended, note](bool success, const QByteArray &) {
        if (!replaceWithTemporary(success)) {
            finish(extended, QStringLiteral("could not add recorded audio"));
            return;
        }
        finish(extended, note);
    });
}

void RecordingFinalizer::startProcess(const QString &program, const QStringList &args,
                                      const std::function<void(bool, const QByteArray &)> &completed)
{
    process_ = new QProcess(this);
    QProcess *process = process_;
    const auto done = [this, process, completed](bool success) {
        const QByteArray output = process->readAllStandardOutput();
        process->deleteLater();
        process_ = nullptr;
        completed(success, output);
    };
    connect(process, &QProcess::finished, this, [done](int code, QProcess::ExitStatus status) {
        done(status == QProcess::NormalExit && code == 0);
    });
    connect(process, &QProcess::errorOccurred, this, [done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            done(false);
        }
    });
    QTimer::singleShot(processTimeoutMs, process, [process] { process->kill(); });
    process->start(QStandardPaths::findExecutable(program), args);
}

bool RecordingFinalizer::openTemporary(const QString &purpose)
{
    const QFileInfo info(path_);
    QTemporaryFile temporary(info.dir().filePath(
        QStringLiteral(".%1.%2.XXXXXX.%3").arg(info.completeBaseName(), purpose, info.suffix())));
    if (!temporary.open()) {
        return false;
    }
    temporary.setAutoRemove(false);
    temporary_ = temporary.fileName();
    return true;
}

bool RecordingFinalizer::replaceWithTemporary(bool success)
{
    const bool replaced = success && QFileInfo(temporary_).size() > 0
        && QFile::setPermissions(temporary_, QFile::permissions(path_))
        // rename(2) replaces the original in one step.
        && std::rename(QFile::encodeName(temporary_).constData(), QFile::encodeName(path_).constData()) == 0;
    if (!replaced) {
        QFile::remove(temporary_);
    }
    temporary_.clear();
    return replaced;
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
