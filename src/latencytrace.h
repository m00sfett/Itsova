#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>


namespace itsova
{

// Timing of one Print capture, from the trigger to the written file (#67). On Wayland kglobalaccel
// sends no key timestamp (always 0), so the time before itsova receives the trigger is unknown.
// All offsets are microseconds after itsova received the trigger; -1 means "not reached".
struct LatencySample
{
    qint64 startedUs = 0; // CLOCK_MONOTONIC, when itsova received the trigger
    qint64 requestUs = -1; // first ScreenShot2 request sent
    qint64 pixelsUs = -1; // frames of all monitors in memory
    qint64 overlayUs = -1; // first overlay frame painted
    qint64 fileUs = -1; // full screenshot written
    qint64 pinWaitUs = 0; // waiting for the compositor to show transparent pins
    int monitors = 0;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static LatencySample fromJson(const QJsonObject &object);
};

struct LatencyStats
{
    int count = 0;
    qint64 medianUs = -1;
    qint64 p95Us = -1;
    qint64 maxUs = -1;
};

// Median, 95th percentile and maximum of the non-negative values.
[[nodiscard]] LatencyStats latencyStats(QList<qint64> values);

// Process-wide recorder. begin() runs on the main thread; mark() is thread-safe,
// because ScreenShot2 requests are sent from worker threads. The first mark of a stage wins.
class LatencyTrace
{
public:
    enum class Stage {
        Request,
        Pixels,
        Overlay,
        File,
    };

    [[nodiscard]] static qint64 nowUs();
    static void begin(int monitors);
    static void setPinWait(qint64 us);
    static void mark(Stage stage);
    [[nodiscard]] static bool reached(Stage stage);
    // The last samples, oldest first, including the running one.
    [[nodiscard]] static QJsonArray report();
    static void resetForTest();
};

} // namespace itsova
