#include "screenrecorder.h"

#include "logging.h"
#include "qwayland-zkde-screencast-unstable-v1.h"
#include "recordingaudiocapture.h"
#include "recordingfinalizer.h"
#include "windowlist.h"

#include <PipeWireRecord>

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>
#include <QWaylandClientExtensionTemplate>

namespace itsova
{

namespace
{
// stream_region needs version 3; version 6 adds the object serial.
constexpr int wantedVersion = 6;
constexpr int minimumVersion = 3;
constexpr int streamTimeoutMs = 10000;

enum PointerMode : uint32_t { PointerHidden = 1, PointerEmbedded = 2 };

PipeWireBaseEncodedStream::Encoder encoderFor(RecordingFormat format)
{
    switch (format) {
    case RecordingFormat::Gif:
        return PipeWireBaseEncodedStream::Gif;
    case RecordingFormat::WebM:
        return PipeWireBaseEncodedStream::VP9;
    case RecordingFormat::Mp4:
        break;
    }
    return PipeWireBaseEncodedStream::H264Main;
}

} // namespace

class ScreencastManager : public QWaylandClientExtensionTemplate<ScreencastManager>,
                          public QtWayland::zkde_screencast_unstable_v1
{
public:
    ScreencastManager()
        : QWaylandClientExtensionTemplate<ScreencastManager>(wantedVersion)
    {
        initialize();
    }
    ~ScreencastManager() override
    {
        if (isInitialized()) {
            destroy();
        }
    }
};

class ScreencastStream : public QObject, public QtWayland::zkde_screencast_stream_unstable_v1
{
    Q_OBJECT

public:
    explicit ScreencastStream(::zkde_screencast_stream_unstable_v1 *stream)
        : QtWayland::zkde_screencast_stream_unstable_v1(stream)
    {
    }
    ~ScreencastStream() override
    {
        if (isInitialized()) {
            close();
        }
    }

Q_SIGNALS:
    void ready(quint64 objectSerial, quint32 nodeId);
    void failed(const QString &error);
    void closed();

protected:
    void zkde_screencast_stream_unstable_v1_created(uint32_t node) override
    {
        // Older KWin sends only the node; version 6 follows up with the serial (used when both
        // the protocol file and KPipeWire know it).
        nodeId_ = node;
        if (!useSerial()) {
            Q_EMIT ready(0, node);
        }
    }
#ifdef ITSOVA_SCREENCAST_SERIAL_EVENT
    void zkde_screencast_stream_unstable_v1_serial(uint32_t hi, uint32_t low) override
    {
        if (useSerial()) {
            Q_EMIT ready((quint64(hi) << 32) | low, nodeId_);
        }
    }
#endif
    void zkde_screencast_stream_unstable_v1_failed(const QString &error) override { Q_EMIT failed(error); }
    void zkde_screencast_stream_unstable_v1_closed() override { Q_EMIT closed(); }

private:
    [[nodiscard]] bool useSerial() const
    {
#if defined(ITSOVA_SCREENCAST_SERIAL_EVENT) && defined(ITSOVA_KPIPEWIRE_SERIAL)
        return version() >= 6;
#else
        return false;
#endif
    }

    quint32 nodeId_ = 0;
};

ScreenRecorder::ScreenRecorder(QObject *parent)
    : QObject(parent)
{
    monotonic_.start();
    resizePollTimer_ = new QTimer(this);
    resizePollTimer_->setInterval(200);
    connect(resizePollTimer_, &QTimer::timeout, this, &ScreenRecorder::pollWindowSize);
}

ScreenRecorder::~ScreenRecorder()
{
    cleanUp();
}

bool ScreenRecorder::start(const RecordingTarget &target, const Options &options)
{
    if (state_ != State::Idle) {
        Q_EMIT failed(tr("A recording is already running."));
        return false;
    }
    if (!target.isValid()) {
        Q_EMIT failed(tr("There is nothing to record."));
        return false;
    }
    if (!manager_) {
        manager_ = std::make_unique<ScreencastManager>();
    }
    if (!manager_->isActive() || manager_->QWaylandClientExtension::version() < minimumVersion) {
        manager_.reset();
        Q_EMIT failed(tr("KWin does not offer screen recording to Itsova. Reinstall Itsova so its desktop file "
                         "grants the screencast interface, then log in again."));
        return false;
    }

    target_ = target;
    options_ = options;
    if (options_.audioSource != RecordingAudioSource::None
        && (options_.format == RecordingFormat::Gif || !canRecordAudio())) {
        qCInfo(lcRecord) << "audio is unavailable for this recording";
        options_.audioSource = RecordingAudioSource::None;
    }
    stopRequested_ = false;
    endReason_.clear();
    finalDurationMs_ = 0;
    windowResizes_.clear();
    lastWindowSize_ = QSize();
    const uint32_t pointer = options.includeCursor ? PointerEmbedded : PointerHidden;
    ::zkde_screencast_stream_unstable_v1 *raw = nullptr;
    if (target.kind == RecordingTarget::Kind::Window) {
        raw = manager_->stream_window(target.windowHandle, pointer);
    } else {
        raw = manager_->stream_region(target.region.x(), target.region.y(), uint32_t(target.region.width()),
                                      uint32_t(target.region.height()), wl_fixed_from_double(target.scale), pointer);
    }
    stream_ = std::make_unique<ScreencastStream>(raw);
    connect(stream_.get(), &ScreencastStream::ready, this, &ScreenRecorder::onStreamReady);
    connect(stream_.get(), &ScreencastStream::failed, this, &ScreenRecorder::onStreamFailed);
    connect(stream_.get(), &ScreencastStream::closed, this, &ScreenRecorder::onStreamClosed);
    setState(State::Starting);
    qCInfo(lcRecord) << "requested stream" << target.targetId << target.region << target.scale << options.path;

    QTimer::singleShot(streamTimeoutMs, this, [this, stream = stream_.get()] {
        if (state_ == State::Starting && stream_.get() == stream) {
            fail(tr("KWin did not deliver the screen stream."));
        }
    });
    return true;
}

void ScreenRecorder::onStreamReady(quint64 objectSerial, quint32 nodeId)
{
    if (state_ != State::Starting || encoder_) {
        return;
    }
    encoder_ = std::make_unique<PipeWireRecord>();
    const auto wanted = encoderFor(options_.format);
    if (!encoder_->suggestedEncoders().contains(wanted)) {
        fail(tr("This system has no encoder for %1 recordings (FFmpeg is missing it).")
                 .arg(recordingFormatKey(options_.format).toUpper()));
        return;
    }
    encoder_->setEncoder(wanted);
    encoder_->setMaxFramerate(quint32(effectiveFrameRate(options_.format, options_.frameRate)), 1);
    encoder_->setOutput(options_.path);
#ifdef ITSOVA_KPIPEWIRE_SERIAL
    if (objectSerial != 0) {
        encoder_->setObjectSerial(objectSerial);
    } else
#endif
    {
        Q_UNUSED(objectSerial)
        QT_WARNING_PUSH
        QT_WARNING_DISABLE_DEPRECATED
        encoder_->setNodeId(nodeId);
        QT_WARNING_POP
    }
    connect(encoder_.get(), &PipeWireBaseEncodedStream::stateChanged, this, &ScreenRecorder::onEncoderStateChanged);
    connect(encoder_.get(), &PipeWireBaseEncodedStream::errorFound, this, [this](const QString &error) {
        qCWarning(lcRecord) << "encoder error" << error;
        fail(error.isEmpty() ? tr("The recording could not be encoded.") : error);
    });
    qCInfo(lcRecord) << "stream ready, serial" << objectSerial << "node" << nodeId;
    encoder_->start();
}

void ScreenRecorder::onEncoderStateChanged()
{
    if (!encoder_) {
        return;
    }
    const auto encoderState = encoder_->state();
    qCInfo(lcRecord) << "encoder state" << encoderState << "while" << state_;
    switch (encoderState) {
    case PipeWireBaseEncodedStream::Recording:
        if (state_ == State::Starting) {
            const qint64 now = monotonic_.elapsed();
            clock_.start(now);
            if (options_.audioSource != RecordingAudioSource::None) {
                const QString title = options_.audioSource == RecordingAudioSource::System ? tr("System audio")
                                                                                            : tr("Microphone");
                audioCapture_ = RecordingAudioCapture::start(options_.audioSource, title, options_.path, monotonic_, now);
                if (!audioCapture_) {
                    options_.audioSource = RecordingAudioSource::None;
                }
            }
            setState(State::Recording);
            if (target_.kind == RecordingTarget::Kind::Window) {
                pollWindowSize();
                resizePollTimer_->start();
            }
        } else if (state_ == State::Paused) {
            const qint64 now = monotonic_.elapsed();
            clock_.resume(now);
            if (audioCapture_) {
                audioCapture_->setPaused(false, now);
            }
            setState(State::Recording);
            if (target_.kind == RecordingTarget::Kind::Window) {
                pollWindowSize();
                resizePollTimer_->start();
            }
        }
        break;
#ifdef ITSOVA_KPIPEWIRE_PAUSE
    case PipeWireBaseEncodedStream::Paused:
        if (state_ == State::Recording) {
            resizePollTimer_->stop();
            const qint64 now = monotonic_.elapsed();
            clock_.pause(now);
            if (audioCapture_) {
                audioCapture_->setPaused(true, now);
            }
            setState(State::Paused);
        }
        break;
#endif
    case PipeWireBaseEncodedStream::Rendering:
        resizePollTimer_->stop();
        if (!stopRequested_ && endReason_.isEmpty()) {
            endReason_ = tr("The encoder stopped by itself.");
            qCWarning(lcRecord) << "encoder stopped without a stop request";
        }
        if (state_ != State::Finishing) {
            finalDurationMs_ = clock_.elapsed(monotonic_.elapsed());
            setState(State::Finishing);
        }
        break;
    case PipeWireBaseEncodedStream::Idle:
        if ((state_ == State::Finishing || stopRequested_) && !finalizing_) {
            finalizing_ = true;
            if (state_ != State::Finishing) {
                finalDurationMs_ = clock_.elapsed(monotonic_.elapsed());
                setState(State::Finishing);
            }
            // Not from inside the encoder's own signal: finalizeFile() deletes the encoder.
            QMetaObject::invokeMethod(this, &ScreenRecorder::finalizeFile, Qt::QueuedConnection);
        }
        break;
    }
}

void ScreenRecorder::finalizeFile()
{
    // The encoder is done; the stream and encoder go, the file gets its full length.
    if (encoder_) {
        encoder_->disconnect(this);
        encoder_.release()->deleteLater();
    }
    if (stream_) {
        stream_->disconnect(this);
        stream_.release()->deleteLater();
    }
    const QFileInfo info(options_.path);
    if (!info.exists() || info.size() == 0) {
        const QString path = options_.path;
        cleanUp();
        setState(State::Idle);
        QFile::remove(path);
        Q_EMIT failed(tr("The recording ended without any frames."));
        return;
    }
    finalizer_ = std::make_unique<RecordingFinalizer>();
    connect(finalizer_.get(), &RecordingFinalizer::done, this, [this](bool extended, const QString &note) {
        if (extended) {
            qCInfo(lcRecord) << "finalized" << options_.path << "last frame held" << note;
        } else {
            qCInfo(lcRecord) << "finalized" << options_.path << note;
        }
        const QString path = options_.path;
        const RecordingTarget target = target_;
        const qint64 duration = finalDurationMs_;
        const QString endReason = endReason_;
        // The finalizer emits from its own slot; delete it later.
        finalizer_.release()->deleteLater();
        cleanUp();
        setState(State::Idle);
        if (!endReason.isEmpty()) {
            Q_EMIT interrupted(endReason);
        }
        Q_EMIT finished(path, target, duration);
    });
    QList<RecordingPause> pauses;
    for (const auto &[start, end] : clock_.pauses()) {
        pauses.append({double(start) / 1000.0, double(end) / 1000.0});
    }
    QList<RecordingAudioTrack> audioTracks;
    if (audioCapture_) {
        if (const auto track = audioCapture_->track()) {
            audioTracks.append(*track);
        } else {
            qCInfo(lcRecord) << "no usable audio samples for recording";
        }
    }
    finalizer_->finalize(options_.path, finalDurationMs_, pauses, options_.metadata, windowResizes_, audioTracks);
}

void ScreenRecorder::pollWindowSize()
{
    if (state_ != State::Recording || target_.kind != RecordingTarget::Kind::Window || target_.windowHandle.isEmpty()) {
        return;
    }
    const QList<WindowInfo> windows = listWindows(QCoreApplication::applicationPid());
    for (const WindowInfo &win : windows) {
        if (win.handle == target_.windowHandle) {
            const QSize sz = win.geometry.size();
            if (sz.isValid() && sz != lastWindowSize_) {
                lastWindowSize_ = sz;
                windowResizes_.append({double(clock_.elapsed(monotonic_.elapsed())) / 1000.0, sz});
            }
            return;
        }
    }
}

void ScreenRecorder::stop(const QString &origin)
{
    qCInfo(lcRecord) << "stop requested by" << origin << "while" << state_;
    switch (state_) {
    case State::Idle:
    case State::Finishing:
        return;
    case State::Starting:
        // Nothing recorded yet: drop the stream instead of waiting for the encoder.
        stopRequested_ = true;
        if (encoder_ && encoder_->state() != PipeWireBaseEncodedStream::Idle) {
            encoder_->stop();
        } else {
            const QString path = options_.path;
            cleanUp();
            setState(State::Idle);
            if (QFileInfo(path).exists() && QFileInfo(path).size() == 0) {
                QFile::remove(path);
            }
        }
        return;
    case State::Recording:
    case State::Paused:
        stopRequested_ = true;
        finalDurationMs_ = clock_.elapsed(monotonic_.elapsed());
        setState(State::Finishing);
        encoder_->stop();
        return;
    }
}

bool ScreenRecorder::canPause()
{
#ifdef ITSOVA_KPIPEWIRE_PAUSE
    return true;
#else
    return false;
#endif
}

bool ScreenRecorder::canRecordAudio()
{
    return RecordingAudioCapture::isSupported() && !QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()
        && !QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty();
}

void ScreenRecorder::setPaused(bool paused)
{
#ifdef ITSOVA_KPIPEWIRE_PAUSE
    if (!encoder_) {
        return;
    }
    if (paused && state_ == State::Recording) {
        encoder_->pause();
    } else if (!paused && state_ == State::Paused) {
        encoder_->resume();
    }
#else
    Q_UNUSED(paused)
#endif
}

void ScreenRecorder::togglePause()
{
    setPaused(state_ == State::Recording);
}

qint64 ScreenRecorder::elapsedMs() const
{
    if (state_ == State::Finishing) {
        return finalDurationMs_;
    }
    return clock_.elapsed(monotonic_.elapsed());
}

void ScreenRecorder::onStreamFailed(const QString &error)
{
    qCWarning(lcRecord) << "stream failed" << error;
    fail(error.isEmpty() ? tr("KWin refused the screen stream.") : error);
}

void ScreenRecorder::onStreamClosed()
{
    // The window closed or the monitor went away: keep what was recorded and say why it ended.
    qCInfo(lcRecord) << "stream closed by KWin";
    if (state_ != State::Finishing && state_ != State::Idle) {
        endReason_ = target_.kind == RecordingTarget::Kind::Window
                         ? tr("The recorded window was closed.")
                         : tr("The recorded monitor is no longer available.");
    }
    stop(QStringLiteral("KWin (stream closed)"));
}

void ScreenRecorder::fail(const QString &error)
{
    if (state_ == State::Idle) {
        return;
    }
    qCWarning(lcRecord) << "recording failed while" << state_ << ":" << error;
    const QString path = options_.path;
    cleanUp();
    setState(State::Idle);
    const QFileInfo info(path);
    if (info.exists() && info.size() == 0) {
        QFile::remove(path);
    }
    Q_EMIT failed(error);
}

void ScreenRecorder::setState(State state)
{
    if (state_ == state) {
        return;
    }
    if (state == State::Finishing && audioCapture_) {
        audioCapture_->stop();
    }
    qCInfo(lcRecord) << "state" << state_ << "->" << state;
    state_ = state;
    Q_EMIT stateChanged(state);
}

void ScreenRecorder::cleanUp()
{
    // Callers may run inside a signal of the encoder or the stream, so both are deleted later.
    if (encoder_) {
        encoder_->disconnect(this);
        encoder_.release()->deleteLater();
    }
    if (stream_) {
        stream_->disconnect(this);
        stream_.release()->deleteLater();
    }
    finalizer_.reset();
    audioCapture_.reset();
    options_.audioSource = RecordingAudioSource::None;
    finalizing_ = false;
}

} // namespace itsova

#include "screenrecorder.moc"
