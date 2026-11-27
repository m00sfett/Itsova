#pragma once

#include "recordingaudio.h"

#include <QElapsedTimer>

#include <memory>
#include <optional>

namespace itsova
{

// Capture one explicitly selected source through the PulseAudio protocol. Its private sidecar
// remains readable by the finalizer until this object is destroyed.
class RecordingAudioCapture
{
public:
    [[nodiscard]] static bool isSupported();
    [[nodiscard]] static std::unique_ptr<RecordingAudioCapture> start(RecordingAudioSource source,
                                                                      const QString &title, const QString &videoPath,
                                                                      QElapsedTimer clock, qint64 videoStartMs);
    ~RecordingAudioCapture();

    void setPaused(bool paused, qint64 nowMs);
    void stop();
    [[nodiscard]] std::optional<RecordingAudioTrack> track() const;

private:
    struct Capture;
    explicit RecordingAudioCapture(std::unique_ptr<Capture> capture);
    std::unique_ptr<Capture> capture_;
};

} // namespace itsova
