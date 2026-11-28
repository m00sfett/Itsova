#include "recordingaudiocapture.h"

#ifdef ITSOVA_HAVE_RECORDING_AUDIO
#include "logging.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>

#include <pulse/pulseaudio.h>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace itsova
{

#ifdef ITSOVA_HAVE_RECORDING_AUDIO
namespace
{
constexpr pa_sample_spec format{PA_SAMPLE_S16LE, recordingAudioRate, recordingAudioChannels};
constexpr int fragmentUs = 20000;
constexpr qint64 driftCheckIntervalUs = 1000000;
constexpr qint64 driftThresholdFrames = 2LL * fragmentUs * recordingAudioRate / 1000000;
constexpr int driftCorrectionFrames = recordingAudioRate / 1000;

qint64 sampleIndex(qint64 timestampUs)
{
    return qRound64(double(timestampUs) * recordingAudioRate / 1000000);
}

int openSidecar(const QString &directory)
{
    const QByteArray name = QFile::encodeName(directory);
    const int anonymous = ::open(name.constData(), O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
    if (anonymous >= 0) {
        return anonymous;
    }
    // Some filesystems lack O_TMPFILE. Unlink the empty, private file before recording into it.
    QTemporaryFile temporary(QDir(directory).filePath(QStringLiteral(".itsova-pcm-XXXXXX")));
    if (!temporary.open()) {
        return -1;
    }
    const int handle = ::fcntl(temporary.handle(), F_DUPFD_CLOEXEC, 0);
    if (handle >= 0 && ::unlink(QFile::encodeName(temporary.fileName()).constData()) != 0) {
        ::close(handle);
        return -1;
    }
    return handle;
}

bool writeSamples(int handle, const char *data, size_t bytes, qint64 offset)
{
    while (bytes > 0) {
        const ssize_t count = ::pwrite(handle, data, bytes, offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        data += count;
        bytes -= count;
        offset += count;
    }
    return true;
}
} // namespace

struct RecordingAudioCapture::Capture
{
    RecordingAudioSource source;
    QByteArray windowPid;
    QByteArray microphoneName;
    uint32_t microphoneIndex = PA_INVALID_INDEX;
    QString title;
    QElapsedTimer clock;
    qint64 videoStartUs;
    int file = -1;
    QString path;
    pa_threaded_mainloop *loop = nullptr;
    pa_context *context = nullptr;
    pa_stream *stream = nullptr;
    QByteArray device;
    uint32_t playbackStream = PA_INVALID_INDEX;
    uint32_t playbackSink = PA_INVALID_INDEX;
    uint32_t monitorSource = PA_INVALID_INDEX;
    bool applicationRouteVerified = false;
    bool started = false;
    bool failed = false;
    bool wantsSamples = true;
    bool receiving = false;
    bool corkAgain = false;
    bool controlPending = false;
    qint64 captureAfterUs = 0;
    std::optional<qint64> nextFrame;
    qint64 nextDriftCheckUs = 0;
    int driftChecks = 0;
    std::optional<qint64> firstFrame;
    qint64 endFrame = 0;

    ~Capture()
    {
        stop();
        if (file >= 0) {
            ::close(file);
        }
    }

    qint64 nowUs() const { return clock.nsecsElapsed() / 1000 - videoStartUs; }

    bool open(const QString &directory)
    {
        file = openSidecar(directory);
        path = QStringLiteral("/proc/%1/fd/%2").arg(QCoreApplication::applicationPid()).arg(file);
        if (file < 0 || !QFileInfo(path).isReadable()) {
            qCWarning(lcRecord) << "could not open private audio sidecar";
            return false;
        }
        loop = pa_threaded_mainloop_new();
        if (!loop) {
            return false;
        }
        context = pa_context_new(pa_threaded_mainloop_get_api(loop), "Itsova");
        if (!context) {
            return false;
        }
        pa_context_set_state_callback(context, [](pa_context *context, void *userdata) {
            auto &self = *static_cast<Capture *>(userdata);
            switch (pa_context_get_state(context)) {
            case PA_CONTEXT_READY:
                self.findDevice();
                break;
            case PA_CONTEXT_FAILED:
            case PA_CONTEXT_TERMINATED:
                self.fail("audio service unavailable");
                break;
            default:
                break;
            }
        }, this);
        if (pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
            return false;
        }
        started = pa_threaded_mainloop_start(loop) == 0;
        return started;
    }

    void stop()
    {
        if (!loop) {
            return;
        }
        // Join the Pulse thread before making the sidecar available to the finalizer.
        if (started) {
            pa_threaded_mainloop_stop(loop);
            started = false;
        }
        if (stream) {
            pa_stream_set_state_callback(stream, nullptr, nullptr);
            if (pa_stream_get_state(stream) == PA_STREAM_READY) {
                pa_stream_disconnect(stream);
            }
            pa_stream_unref(stream);
            stream = nullptr;
        }
        if (context) {
            pa_context_set_state_callback(context, nullptr, nullptr);
            pa_context_set_subscribe_callback(context, nullptr, nullptr);
            pa_context_disconnect(context);
            pa_context_unref(context);
            context = nullptr;
        }
        pa_threaded_mainloop_free(loop);
        loop = nullptr;
        if (firstFrame && !failed && ::ftruncate(file, (endFrame - *firstFrame) * recordingAudioFrameBytes) < 0) {
            fail("could not size audio sidecar");
        }
    }

    void findDevice()
    {
        if (source == RecordingAudioSource::Microphone && !microphoneName.isEmpty()) {
            findMicrophone();
            return;
        }
        submit(pa_context_get_server_info(context, [](pa_context *context, const pa_server_info *info, void *userdata) {
            auto &self = *static_cast<Capture *>(userdata);
            if (self.source == RecordingAudioSource::Application) {
                // The direct route check below is specific to PipeWire's PulseAudio server.
                if (!info || !info->server_name || !QByteArrayView(info->server_name).startsWith("PulseAudio (on PipeWire ")) {
                    self.fail("application audio requires the PipeWire PulseAudio service");
                    return;
                }
                self.watchApplicationStream();
            } else if (info && self.source == RecordingAudioSource::System && info->default_sink_name
                && *info->default_sink_name) {
                self.submit(pa_context_get_sink_info_by_name(context, info->default_sink_name,
                    [](pa_context *, const pa_sink_info *sink, int end, void *userdata) {
                        auto &self = *static_cast<Capture *>(userdata);
                        if (sink && sink->monitor_source_name) {
                            self.connectDevice(sink->monitor_source_name);
                        } else if (end != 0 && !self.stream) {
                            self.fail("default audio output has no monitor");
                        }
                    }, &self));
            } else if (info && self.source == RecordingAudioSource::Microphone && info->default_source_name
                       && *info->default_source_name) {
                self.microphoneName = info->default_source_name;
                self.findMicrophone();
            } else {
                self.fail("selected default audio device is unavailable");
            }
        }, this));
    }

    void findMicrophone()
    {
        submit(pa_context_get_source_info_by_name(context, microphoneName.constData(),
            [](pa_context *, const pa_source_info *input, int end, void *userdata) {
                auto &self = *static_cast<Capture *>(userdata);
                if (end != 0) {
                    if (!self.stream) {
                        self.fail("selected microphone is unavailable");
                    }
                } else if (input && input->name && self.microphoneName == input->name
                           && input->monitor_of_sink == PA_INVALID_INDEX && input->index != PA_INVALID_INDEX) {
                    self.microphoneIndex = input->index;
                    self.connectDevice(input->name);
                }
            }, this));
    }

    void watchApplicationStream()
    {
        pa_context_set_subscribe_callback(context, [](pa_context *, pa_subscription_event_type_t event, uint32_t index,
                                                       void *userdata) {
            auto &self = *static_cast<Capture *>(userdata);
            if (index == self.playbackStream
                && (event & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK_INPUT
                && (event & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE) {
                self.fail("the window application's playback stream disappeared");
            }
        }, this);
        submit(pa_context_subscribe(context, PA_SUBSCRIPTION_MASK_SINK_INPUT, [](pa_context *, int success,
                                                                                void *userdata) {
            auto &self = *static_cast<Capture *>(userdata);
            if (success) {
                self.findApplicationStream();
            } else {
                self.fail("could not watch the window application's playback stream");
            }
        }, this));
    }

    void findApplicationStream()
    {
        submit(pa_context_get_sink_input_info_list(context, [](pa_context *, const pa_sink_input_info *input,
                                                               int end, void *userdata) {
            auto &self = *static_cast<Capture *>(userdata);
            if (self.failed) {
                return;
            }
            if (end != 0) {
                if (end < 0 || self.playbackStream == PA_INVALID_INDEX || self.playbackSink == PA_INVALID_INDEX) {
                    self.fail("the window application has no identifiable playback stream on an audio output");
                } else {
                    self.findApplicationMonitor();
                }
                return;
            }
            const char *pid = input && input->proplist
                ? pa_proplist_gets(input->proplist, PA_PROP_APPLICATION_PROCESS_ID) : nullptr;
            if (!pid || self.windowPid != pid) {
                return;
            }
            // Zero is not a valid direct target in PipeWire's PulseAudio service.
            if (self.playbackStream != PA_INVALID_INDEX || input->index == PA_INVALID_INDEX || input->index == 0) {
                self.fail("the window application's playback stream is ambiguous or unavailable");
                return;
            }
            self.playbackStream = input->index;
            self.playbackSink = input->sink;
        }, this));
    }

    void findApplicationMonitor()
    {
        submit(pa_context_get_sink_info_by_index(context, playbackSink, [](pa_context *, const pa_sink_info *sink,
                                                                          int end, void *userdata) {
            auto &self = *static_cast<Capture *>(userdata);
            if (self.failed) {
                return;
            }
            if (sink && sink->index == self.playbackSink && sink->monitor_source != PA_INVALID_INDEX
                && sink->monitor_source_name && *sink->monitor_source_name) {
                self.monitorSource = sink->monitor_source;
                self.connectDevice(sink->monitor_source_name);
            } else if (end != 0 && !self.stream) {
                self.fail("the window application's audio output has no monitor");
            }
        }, this));
    }

    pa_stream *createStream()
    {
        if (source != RecordingAudioSource::Application) {
            return pa_stream_new(context, "Itsova screen recording", &format, nullptr);
        }
        const std::unique_ptr<pa_proplist, decltype(&pa_proplist_free)> properties(pa_proplist_new(), pa_proplist_free);
        if (!properties || pa_proplist_sets(properties.get(), "node.dont-fallback", "true") < 0
            || pa_proplist_sets(properties.get(), "node.dont-move", "true") < 0
            || pa_proplist_sets(properties.get(), "node.dont-reconnect", "true") < 0
            || pa_proplist_sets(properties.get(), "stream.restore-target", "false") < 0) {
            return nullptr;
        }
        return pa_stream_new_with_proplist(context, "Itsova screen recording", &format, nullptr, properties.get());
    }

    void connectDevice(const char *name)
    {
        if (!name || !*name) {
            fail("selected default audio device has no name");
            return;
        }
        device = name;
        stream = createStream();
        if (!stream) {
            fail("could not create audio stream");
            return;
        }
        if (source == RecordingAudioSource::Application && pa_stream_set_monitor_stream(stream, playbackStream) < 0) {
            fail("could not select the window application's playback stream");
            return;
        }
        pa_stream_set_state_callback(stream, [](pa_stream *stream, void *userdata) {
            auto &self = *static_cast<Capture *>(userdata);
            switch (pa_stream_get_state(stream)) {
            case PA_STREAM_READY:
                self.checkDevice();
                if (self.source == RecordingAudioSource::Application && !self.failed) {
                    self.verifyApplicationRoute();
                } else {
                    self.updateStream();
                }
                break;
            case PA_STREAM_FAILED:
            case PA_STREAM_TERMINATED:
                self.fail("audio stream disconnected");
                break;
            default:
                break;
            }
        }, this);
        pa_stream_set_moved_callback(stream, [](pa_stream *, void *userdata) {
            auto &self = *static_cast<Capture *>(userdata);
            if (self.source == RecordingAudioSource::Application) {
                self.fail("the window application's recording stream was moved");
            } else {
                self.checkDevice();
            }
        }, this);
        pa_stream_set_read_callback(stream, [](pa_stream *, size_t, void *userdata) {
            static_cast<Capture *>(userdata)->readSamples();
        }, this);
        pa_stream_set_overflow_callback(stream, [](pa_stream *, void *userdata) {
            static_cast<Capture *>(userdata)->nextFrame.reset();
        }, this);
        pa_buffer_attr buffer{};
        buffer.maxlength = uint32_t(-1);
        buffer.fragsize = uint32_t(pa_usec_to_bytes(fragmentUs, &format));
        const auto flags = pa_stream_flags_t(PA_STREAM_START_CORKED | PA_STREAM_DONT_MOVE | PA_STREAM_ADJUST_LATENCY
                                            | PA_STREAM_AUTO_TIMING_UPDATE | PA_STREAM_INTERPOLATE_TIMING);
        if (pa_stream_connect_record(stream, device.constData(), &buffer, flags) < 0) {
            fail("could not open selected audio device");
        }
    }

    void checkDevice()
    {
        const uint32_t index = pa_stream_get_device_index(stream);
        if (device != pa_stream_get_device_name(stream) || (source == RecordingAudioSource::Application && index != monitorSource)
            || (source == RecordingAudioSource::Microphone && index != microphoneIndex)) {
            fail("audio service did not keep the selected device");
        }
    }

    void verifyApplicationRoute()
    {
        submit(pa_context_get_source_output_info(context, pa_stream_get_index(stream),
            [](pa_context *, const pa_source_output_info *output, int end, void *userdata) {
                auto &self = *static_cast<Capture *>(userdata);
                if (self.failed) {
                    return;
                }
                if (output) {
                    // PipeWire reports a source index for an ordinary device or sink monitor.
                    // A direct playback peer has no source index here, only a target object.
                    const char *target = output->proplist ? pa_proplist_gets(output->proplist, "target.object") : nullptr;
                    if (output->source == PA_INVALID_INDEX && QByteArray::number(self.playbackStream) == target) {
                        self.applicationRouteVerified = true;
                        self.updateStream();
                    } else {
                        self.fail("audio service did not provide a direct application route");
                    }
                } else if (end != 0 && !self.applicationRouteVerified) {
                    self.fail("could not verify the direct application route");
                }
            }, this));
    }

    void setPaused(bool paused, qint64 nowMs)
    {
        if (!started) {
            return;
        }
        pa_threaded_mainloop_lock(loop);
        wantsSamples = !paused;
        receiving = false;
        nextFrame.reset();
        if (paused) {
            corkAgain = true;
        } else {
            captureAfterUs = nowMs * 1000 - videoStartUs;
        }
        updateStream();
        pa_threaded_mainloop_unlock(loop);
    }

    void updateStream()
    {
        if (failed || (source == RecordingAudioSource::Application && !applicationRouteVerified)
            || controlPending || !stream || pa_stream_get_state(stream) != PA_STREAM_READY) {
            return;
        }
        if (pa_stream_is_corked(stream) == 1) {
            corkAgain = false;
            if (wantsSamples) {
                // Drop delayed samples from the previous active interval before uncorking.
                controlPending = true;
                submit(pa_stream_flush(stream, [](pa_stream *, int success, void *userdata) {
                    auto &self = *static_cast<Capture *>(userdata);
                    if (self.completeControl(success) && self.wantsSamples) {
                        self.captureAfterUs = std::max(self.captureAfterUs, self.nowUs());
                        self.changeCork(false);
                    }
                }, this));
            }
        } else if (!wantsSamples || corkAgain) {
            changeCork(true);
        } else {
            receiving = true;
        }
    }

    void changeCork(bool cork)
    {
        controlPending = true;
        submit(pa_stream_cork(stream, cork, [](pa_stream *, int success, void *userdata) {
            auto &self = *static_cast<Capture *>(userdata);
            if (self.completeControl(success)) {
                self.updateStream();
            }
        }, this));
    }

    bool completeControl(int success)
    {
        controlPending = false;
        if (!success) {
            fail("could not pause or resume audio stream");
        }
        return !failed;
    }

    void readSamples()
    {
        while (!failed) {
            const void *data = nullptr;
            size_t bytes = 0;
            if (pa_stream_peek(stream, &data, &bytes) < 0) {
                fail("could not read audio samples");
                return;
            }
            if (bytes == 0) {
                return;
            }
            const bool written = !receiving || appendSamples(static_cast<const char *>(data), bytes);
            const bool dropped = pa_stream_drop(stream) == 0;
            if (!written || !dropped) {
                fail("could not store audio samples");
            }
        }
    }

    bool appendSamples(const char *data, size_t bytes)
    {
        if (bytes % recordingAudioFrameBytes != 0) {
            return false;
        }
        const qint64 now = nowUs();
        if (!nextFrame || now >= nextDriftCheckUs) {
            updateSamplePosition(bytes, now);
        }
        const qint64 packetStart = *nextFrame;
        *nextFrame += qint64(bytes / recordingAudioFrameBytes);
        const qint64 writeStart = std::max({packetStart, sampleIndex(captureAfterUs), endFrame});
        if (*nextFrame <= writeStart || (!data && !firstFrame)) {
            return true;
        }
        if (!firstFrame) {
            firstFrame = writeStart;
        }
        if (data && !writeSamples(file, data + (writeStart - packetStart) * recordingAudioFrameBytes,
                                  (*nextFrame - writeStart) * recordingAudioFrameBytes,
                                  (writeStart - *firstFrame) * recordingAudioFrameBytes)) {
            return false;
        }
        // The sidecar spans the original timeline. Gaps are sparse zeros, including pauses.
        endFrame = *nextFrame;
        return true;
    }

    void updateSamplePosition(size_t bytes, qint64 now)
    {
        pa_usec_t latency = 0;
        int negative = 0;
        nextDriftCheckUs = now + driftCheckIntervalUs;
        const bool hasLatency = pa_stream_get_latency(stream, &latency, &negative) == 0;
        const qint64 delay = negative ? -qint64(latency) : qint64(latency);
        if (!nextFrame) {
            // Each active interval starts on the video clock. Pulse can deliver the first
            // fragment before sending timing information.
            const size_t available = pa_stream_readable_size(stream);
            const qint64 bufferedUs = qint64(pa_bytes_to_usec(available == size_t(-1) ? bytes : available, &format));
            nextFrame = sampleIndex(now - (hasLatency ? delay : bufferedUs));
            driftChecks = 0;
            return;
        }

        // Device clocks can drift against the video clock. Ignore jitter below two fragments and
        // require three consecutive measurements in the same direction before nudging by 1 ms.
        // The normal sidecar write then leaves a small gap or drops overlapping samples.
        const qint64 drift = hasLatency ? sampleIndex(now - delay) - *nextFrame : 0;
        if (drift > driftThresholdFrames) {
            driftChecks = std::clamp(driftChecks + 1, 1, 3);
        } else if (drift < -driftThresholdFrames) {
            driftChecks = std::clamp(driftChecks - 1, -3, -1);
        } else {
            driftChecks = 0;
        }
        if (driftChecks == 3) {
            *nextFrame += driftCorrectionFrames;
        } else if (driftChecks == -3) {
            *nextFrame -= driftCorrectionFrames;
        }
    }

    void submit(pa_operation *operation)
    {
        if (operation) {
            pa_operation_unref(operation);
        } else {
            fail("audio service rejected a request");
        }
    }

    void fail(const char *reason)
    {
        if (failed) {
            return;
        }
        failed = true;
        const int error = context ? pa_context_errno(context) : PA_OK;
        if (error != PA_OK) {
            qCWarning(lcRecord) << reason << pa_strerror(error);
        } else {
            qCWarning(lcRecord) << reason;
        }
        if (stream && pa_stream_get_state(stream) == PA_STREAM_READY) {
            pa_stream_disconnect(stream);
        }
    }
};
#else
struct RecordingAudioCapture::Capture {};
#endif

RecordingAudioCapture::RecordingAudioCapture(std::unique_ptr<Capture> capture)
    : capture_(std::move(capture))
{
}

RecordingAudioCapture::~RecordingAudioCapture() = default;

bool RecordingAudioCapture::isSupported()
{
#ifdef ITSOVA_HAVE_RECORDING_AUDIO
    return true;
#else
    return false;
#endif
}

std::optional<QList<RecordingMicrophoneDevice>> RecordingAudioCapture::microphoneDevices()
{
#ifdef ITSOVA_HAVE_RECORDING_AUDIO
    struct DeviceList
    {
        QList<RecordingMicrophoneDevice> devices;
        bool complete = false;
    };

    const QDeadlineTimer deadline(1500);
    const std::unique_ptr<pa_mainloop, decltype(&pa_mainloop_free)> loop(pa_mainloop_new(), pa_mainloop_free);
    if (!loop) {
        return std::nullopt;
    }
    const std::unique_ptr<pa_context, decltype(&pa_context_unref)> context(
        pa_context_new(pa_mainloop_get_api(loop.get()), "Itsova"), pa_context_unref);
    if (!context || pa_context_connect(context.get(), nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
        return std::nullopt;
    }
    // Use the same deadline for connecting and listing, including an unresponsive server.
    const auto dispatch = [&] {
        const qint64 remainingMs = deadline.remainingTime();
        return remainingMs > 0 && pa_mainloop_prepare(loop.get(), int(remainingMs * 1000)) >= 0
            && pa_mainloop_poll(loop.get()) >= 0 && pa_mainloop_dispatch(loop.get()) >= 0;
    };
    while (pa_context_get_state(context.get()) != PA_CONTEXT_READY) {
        if (!PA_CONTEXT_IS_GOOD(pa_context_get_state(context.get())) || !dispatch()) {
            return std::nullopt;
        }
    }

    DeviceList list;
    const std::unique_ptr<pa_operation, decltype(&pa_operation_unref)> operation(
        pa_context_get_source_info_list(context.get(), [](pa_context *, const pa_source_info *input, int end,
                                                         void *userdata) {
            auto &list = *static_cast<DeviceList *>(userdata);
            if (end != 0) {
                list.complete = end > 0;
            } else if (input && input->name && *input->name && input->monitor_of_sink == PA_INVALID_INDEX) {
                list.devices.append({QString::fromUtf8(input->name), QString::fromUtf8(input->description)});
            }
        }, &list), pa_operation_unref);
    if (!operation) {
        return std::nullopt;
    }
    while (!list.complete && pa_operation_get_state(operation.get()) == PA_OPERATION_RUNNING) {
        if (pa_context_get_state(context.get()) != PA_CONTEXT_READY || !dispatch()) {
            pa_operation_cancel(operation.get());
            return std::nullopt;
        }
    }
    if (list.complete) {
        return std::move(list.devices);
    }
#endif
    return std::nullopt;
}

std::unique_ptr<RecordingAudioCapture> RecordingAudioCapture::start(RecordingAudioSource source, const QString &title,
                                                                   const QString &videoPath, QElapsedTimer clock,
                                                                   qint64 videoStartMs, qint64 windowPid,
                                                                   const QString &microphoneDevice)
{
#ifdef ITSOVA_HAVE_RECORDING_AUDIO
    if (source == RecordingAudioSource::None || (source == RecordingAudioSource::Application && windowPid <= 0)) {
        return {};
    }
    auto capture = std::make_unique<Capture>();
    capture->source = source;
    if (source == RecordingAudioSource::Application) {
        capture->windowPid = QByteArray::number(windowPid);
    } else if (source == RecordingAudioSource::Microphone) {
        capture->microphoneName = microphoneDevice.toUtf8();
    }
    capture->title = title;
    capture->clock = clock;
    capture->videoStartUs = videoStartMs * 1000;
    if (capture->open(QFileInfo(videoPath).absolutePath())) {
        return std::unique_ptr<RecordingAudioCapture>(new RecordingAudioCapture(std::move(capture)));
    }
    qCWarning(lcRecord) << "could not start audio recording";
#else
    Q_UNUSED(source)
    Q_UNUSED(title)
    Q_UNUSED(videoPath)
    Q_UNUSED(clock)
    Q_UNUSED(videoStartMs)
    Q_UNUSED(windowPid)
    Q_UNUSED(microphoneDevice)
#endif
    return {};
}

void RecordingAudioCapture::setPaused(bool paused, qint64 nowMs)
{
#ifdef ITSOVA_HAVE_RECORDING_AUDIO
    capture_->setPaused(paused, nowMs);
#else
    Q_UNUSED(paused)
    Q_UNUSED(nowMs)
#endif
}

void RecordingAudioCapture::stop()
{
#ifdef ITSOVA_HAVE_RECORDING_AUDIO
    capture_->stop();
#endif
}

std::optional<RecordingAudioTrack> RecordingAudioCapture::track() const
{
#ifdef ITSOVA_HAVE_RECORDING_AUDIO
    if (!capture_->started && !capture_->failed && capture_->firstFrame) {
        const qint64 startUs = qRound64(double(*capture_->firstFrame) * 1000000 / recordingAudioRate);
        return RecordingAudioTrack{capture_->path, capture_->title, startUs};
    }
#endif
    return std::nullopt;
}

} // namespace itsova
