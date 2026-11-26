#pragma once

#include "recordingplan.h"
#include "targets.h"

#include <QList>
#include <QRect>
#include <QSize>
#include <QString>

#include <optional>

namespace glimply
{

// Where a recording came from (#103), stored in the file so a later replay can put it back on the
// same monitors: MP4 and WebM carry it as the container tag "glimply" (JSON); GIF has no place for
// it.
struct RecordingMonitorInfo
{
    QString connector;
    QString manufacturer;
    QString model;
    QString serial;
    QRect logical;    // the monitor in global logical coordinates at recording time
    QRect video;      // the part of the video it covers, in video pixels (empty for windows)
    qreal scale = 1;  // device pixels per logical pixel
    QSize resolution; // native pixels
};

struct RecordingMetadata
{
    static constexpr int currentVersion = 1;
    int version = currentVersion;
    QString kind;     // "monitor", "all-monitors", "area", "window"
    QString targetId;
    qreal streamScale = 1; // video pixels per logical pixel
    QRect region;          // region recordings: the recorded global logical rectangle
    QRect windowRect;      // window recordings: the window on its monitor (logical)
    QList<RecordingMonitorInfo> monitors;
};

// scales: device pixels per logical pixel, one per screen.
[[nodiscard]] RecordingMetadata buildRecordingMetadata(const RecordingTarget &target, const QList<ScreenInfo> &screens,
                                                       const QList<qreal> &scales);
[[nodiscard]] QString recordingMetadataJson(const RecordingMetadata &metadata);
// Nothing for malformed JSON or a newer, unknown version.
[[nodiscard]] std::optional<RecordingMetadata> parseRecordingMetadata(const QString &json);
// Reads the tag back with ffprobe (blocking). Nothing when ffprobe is missing or the file has none.
[[nodiscard]] std::optional<RecordingMetadata> readRecordingMetadata(const QString &path);
// The ffmpeg output options that write the tag for this file name (none for GIF).
[[nodiscard]] QStringList recordingMetadataArguments(const QString &outputPath, const QString &json);

} // namespace glimply
