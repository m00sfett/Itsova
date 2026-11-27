#pragma once

#include <QDateTime>
#include <QList>
#include <QPair>
#include <QRect>
#include <QString>

namespace itsova
{

// Screen recording (#30): what to record and how, without any compositor or encoder code, so the
// rules stay testable. KWin streams a logical region of the workspace (monitors, areas, the
// selection) or one window; the recorder turns that into a file.
enum class RecordingFormat { Mp4, Gif, WebM };

[[nodiscard]] QString recordingFormatKey(RecordingFormat format); // "mp4", "gif", "webm"
[[nodiscard]] RecordingFormat recordingFormatFromKey(const QString &key); // unknown keys: Mp4
[[nodiscard]] QString recordingExtension(RecordingFormat format);
[[nodiscard]] QList<RecordingFormat> allRecordingFormats();

// GIF files grow fast with the frame rate, so GIF recordings keep at most this many frames per second.
constexpr int maxGifFrameRate = 15;
// Frame rates offered in the settings.
[[nodiscard]] QList<int> recordingFrameRates();
// The frame rate a recording actually uses: the setting, clamped to what the format and the
// offered range allow.
[[nodiscard]] int effectiveFrameRate(RecordingFormat format, int requested);

struct RecordingTarget
{
    enum class Kind { Region, Window };
    Kind kind = Kind::Region;
    QString targetId; // "screen:DP-1", "all", "area:<uuid>", "selection", "window:<uuid>"
    QString displayName; // shown in the tray and the notification
    QString token; // file name token, e.g. "DP-1", "window-konsole"
    QRect region; // Region: global logical rectangle; Window: the window's geometry at the start
    qreal scale = 1.0; // Region: device pixels per logical pixel of the stream
    QString windowHandle; // Window: KWin window UUID

    [[nodiscard]] bool isValid() const;
};

// Scale of a region stream: the highest scale of the monitors it touches, so nothing is recorded
// below its native resolution. 1.0 when it touches none.
[[nodiscard]] qreal regionScale(const QRect &region, const QList<QRect> &screenRects, const QList<qreal> &screenScales);

// A region clipped to the union of the monitors' bounding box. Areas of disconnected monitors
// come back empty.
[[nodiscard]] QRect clipRegionToScreens(const QRect &region, const QList<QRect> &screenRects);

// Even pixel sizes: H.264 and VP9 need them, so the region shrinks by at most one logical pixel
// per side when region × scale would be odd.
[[nodiscard]] QRect evenSizedRegion(const QRect &region, qreal scale);

// Files itsova writes as recordings (.mp4, .webm, .gif): opened with the default player instead of
// the editor, never pinned.
[[nodiscard]] bool isRecordingFile(const QString &path);

// How to stop a recording (#98): the tray and widget tooltips and the start hint (#125) say it.
// shortcut is the global record shortcut, if one is set.
[[nodiscard]] QString recordingStopHint(const QString &shortcut);

// Where a recording hint goes (#125) so that it is not recorded. A window stream holds only the
// window: the hint goes on the window's monitor. A region: on a monitor the region does not touch,
// else in the largest free strip of a touched monitor beside the region. If the region leaves no
// room (a whole monitor, all monitors), the hint goes in the middle of the most covered monitor
// and the recording starts only once the hint has gone (waitBeforeRecording).
struct RecordingHintPlacement
{
    int screen = -1; // index into the monitor list, -1 without monitors
    QRect rect; // global logical rectangle of the hint
    bool waitBeforeRecording = false;
};
[[nodiscard]] RecordingHintPlacement planRecordingHint(bool windowTarget, const QRect &recorded,
                                                       const QList<QRect> &screens, const QSize &hint,
                                                       int margin = 24);

// "0:07", "12:34", "1:02:03".
[[nodiscard]] QString formatElapsed(qint64 milliseconds);

// The file name for a recording: the capture template with the recording's extension.
[[nodiscard]] QString recordingFileName(const QString &filenameTemplate, const QString &token,
                                        const QDateTime &timestamp, RecordingFormat format);

// directory/fileName, or directory/name-2.ext, -3, … when the name is taken.
[[nodiscard]] QString uniqueRecordingPath(const QString &directory, const QString &fileName);

// Pause-aware stopwatch for the elapsed recording time.
class RecordingClock
{
public:
    void start(qint64 nowMs);
    void pause(qint64 nowMs);
    void resume(qint64 nowMs);
    [[nodiscard]] qint64 elapsed(qint64 nowMs) const;
    [[nodiscard]] bool paused() const { return paused_; }
    // Finished pauses as (start, end) in milliseconds after start().
    [[nodiscard]] QList<QPair<qint64, qint64>> pauses() const { return pauses_; }

private:
    qint64 startedAt_ = 0;
    qint64 pausedAt_ = 0;
    qint64 pausedTotal_ = 0;
    bool running_ = false;
    bool paused_ = false;
    QList<QPair<qint64, qint64>> pauses_;
};

} // namespace itsova
