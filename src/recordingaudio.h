#pragma once

#include <QList>
#include <QString>

namespace itsova
{

enum class RecordingAudioSource { None, System, Microphone, Application };

// One playback source at most. The microphone can add its own track to either.
class RecordingAudioSelection
{
public:
    void toggle(RecordingAudioSource source);
    [[nodiscard]] bool isEmpty() const { return playback_ == RecordingAudioSource::None && !microphone_; }
    [[nodiscard]] RecordingAudioSource playback() const { return playback_; }
    [[nodiscard]] bool microphone() const { return microphone_; }
    [[nodiscard]] bool includes(RecordingAudioSource source) const;
    [[nodiscard]] QList<RecordingAudioSource> sources() const;

private:
    RecordingAudioSource playback_ = RecordingAudioSource::None;
    bool microphone_ = false;
};

inline constexpr int recordingAudioRate = 48000;
inline constexpr int recordingAudioChannels = 2;
inline constexpr int recordingAudioFrameBytes = recordingAudioChannels * 2;

// A private signed 16-bit little-endian PCM file. Its first sample corresponds to
// startUs on the video clock, before any pauses are removed.
struct RecordingAudioTrack
{
    QString path;
    QString title;
    qint64 startUs = 0;
};

} // namespace itsova
