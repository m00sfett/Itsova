#pragma once

#include <QString>

namespace itsova
{

enum class RecordingAudioSource { None, System, Microphone, Application };

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
