#include "screenrecorder.h"

#include "qwayland-zkde-screencast-unstable-v1.h"
#include "recordingfinalizer.h"

#include <PipeWireRecord>

#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QTimer>
#include <QWaylandClientExtensionTemplate>

namespace glimply
{

namespace
{
Q_LOGGING_CATEGORY(lcRecord, "glimply.record")

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
        // Older KWin sends only the node; version 6 follows up with the serial.
        nodeId_ = node;
        if (version() < 6) {
            Q_EMIT ready(0, node);
        }
    }
    void zkde_screencast_stream_unstable_v1_serial(uint32_t hi, uint32_t low) override
    {
        Q_EMIT ready((quint64(hi) << 32) | low, nodeId_);
    }
    void zkde_screencast_stream_unstable_v1_failed(const QString &error) override { Q_EMIT failed(error); }
    void zkde_screencast_stream_unstable_v1_closed() override { Q_EMIT closed(); }

private:
    quint32 nodeId_ = 0;
};

ScreenRecorder::ScreenRecorder(QObject *parent)
    : QObject(parent)
{
    monotonic_.start();
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
        Q_EMIT failed(tr("KWin does not offer screen recording to glimply. Reinstall glimply so its desktop file "
                         "grants the screencast interface, then log in again."));
        return false;
    }

    target_ = target;
    options_ = options;
    stopRequested_ = false;
    finalDurationMs_ = 0;
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
    if (objectSerial != 0) {
        encoder_->setObjectSerial(objectSerial);
    } else {
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
    qCDebug(lcRecord) << "encoder state" << encoderState;
    switch (encoderState) {
    case PipeWireBaseEncodedStream::Recording:
        if (state_ == State::Starting) {
            clock_.start(monotonic_.elapsed());
            setState(State::Recording);
        } else if (state_ == State::Paused) {
            clock_.resume(monotonic_.elapsed());
            setState(State::Recording);
        }
        break;
    case PipeWireBaseEncodedStream::Paused:
        if (state_ == State::Recording) {
            clock_.pause(monotonic_.elapsed());
            setState(State::Paused);
        }
        break;
    case PipeWireBaseEncodedStream::Rendering:
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
        qCInfo(lcRecord) << "finalized" << options_.path << (extended ? "last frame held" : note);
        const QString path = options_.path;
        const RecordingTarget target = target_;
        const qint64 duration = finalDurationMs_;
        // The finalizer emits from its own slot; delete it later.
        finalizer_.release()->deleteLater();
        cleanUp();
        setState(State::Idle);
        Q_EMIT finished(path, target, duration);
    });
    finalizer_->finalize(options_.path, finalDurationMs_);
}

void ScreenRecorder::stop()
{
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

void ScreenRecorder::setPaused(bool paused)
{
    if (!encoder_) {
        return;
    }
    if (paused && state_ == State::Recording) {
        encoder_->pause();
    } else if (!paused && state_ == State::Paused) {
        encoder_->resume();
    }
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
    // The window closed or the monitor went away: keep what was recorded.
    qCInfo(lcRecord) << "stream closed by KWin";
    stop();
}

void ScreenRecorder::fail(const QString &error)
{
    if (state_ == State::Idle) {
        return;
    }
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
    finalizing_ = false;
}

} // namespace glimply

#include "screenrecorder.moc"
