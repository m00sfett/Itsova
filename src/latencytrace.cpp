#include "latencytrace.h"

#include <QJsonValue>

#include <algorithm>
#include <ctime>
#include <mutex>

namespace itsova
{

namespace
{

constexpr int maxSamples = 50;
// Marks long after the trigger come from another capture.
constexpr qint64 markWindowUs = 30'000'000;

struct State
{
    std::mutex mutex;
    QList<LatencySample> samples; // the last one may be running
    bool running = false;
};

State &state()
{
    static State s;
    return s;
}

qint64 *stageField(LatencySample &sample, LatencyTrace::Stage stage)
{
    switch (stage) {
    case LatencyTrace::Stage::Request:
        return &sample.requestUs;
    case LatencyTrace::Stage::Pixels:
        return &sample.pixelsUs;
    case LatencyTrace::Stage::Overlay:
        return &sample.overlayUs;
    case LatencyTrace::Stage::File:
        return &sample.fileUs;
    }
    return nullptr;
}

QJsonValue optionalValue(qint64 value)
{
    return value < 0 ? QJsonValue() : QJsonValue(value);
}

qint64 optionalRead(const QJsonObject &object, const QString &key)
{
    const QJsonValue value = object.value(key);
    return value.isDouble() ? qint64(value.toDouble()) : -1;
}

} // namespace

QJsonObject LatencySample::toJson() const
{
    return {
        {QStringLiteral("requestUs"), optionalValue(requestUs)},
        {QStringLiteral("pixelsUs"), optionalValue(pixelsUs)},
        {QStringLiteral("overlayUs"), optionalValue(overlayUs)},
        {QStringLiteral("fileUs"), optionalValue(fileUs)},
        {QStringLiteral("pinWaitUs"), pinWaitUs},
        {QStringLiteral("monitors"), monitors},
    };
}

LatencySample LatencySample::fromJson(const QJsonObject &object)
{
    LatencySample sample;
    sample.requestUs = optionalRead(object, QStringLiteral("requestUs"));
    sample.pixelsUs = optionalRead(object, QStringLiteral("pixelsUs"));
    sample.overlayUs = optionalRead(object, QStringLiteral("overlayUs"));
    sample.fileUs = optionalRead(object, QStringLiteral("fileUs"));
    sample.pinWaitUs = std::max<qint64>(0, optionalRead(object, QStringLiteral("pinWaitUs")));
    sample.monitors = object.value(QStringLiteral("monitors")).toInt();
    return sample;
}

LatencyStats latencyStats(QList<qint64> values)
{
    values.removeIf([](qint64 v) { return v < 0; });
    LatencyStats stats;
    stats.count = int(values.size());
    if (values.isEmpty()) {
        return stats;
    }
    std::sort(values.begin(), values.end());
    const qsizetype n = values.size();
    stats.medianUs = n % 2 ? values.at(n / 2) : (values.at(n / 2 - 1) + values.at(n / 2)) / 2;
    // Nearest-rank percentile.
    const qsizetype rank = std::clamp<qsizetype>((95 * n + 99) / 100, 1, n);
    stats.p95Us = values.at(rank - 1);
    stats.maxUs = values.last();
    return stats;
}

qint64 LatencyTrace::nowUs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return qint64(ts.tv_sec) * 1'000'000 + ts.tv_nsec / 1000;
}

void LatencyTrace::begin(int monitors)
{
    State &s = state();
    const qint64 now = nowUs();
    const std::lock_guard lock(s.mutex);
    LatencySample sample;
    sample.startedUs = now;
    sample.monitors = monitors;
    s.samples.append(sample);
    while (s.samples.size() > maxSamples) {
        s.samples.removeFirst();
    }
    s.running = true;
}

void LatencyTrace::setPinWait(qint64 us)
{
    State &s = state();
    const std::lock_guard lock(s.mutex);
    if (s.running && !s.samples.isEmpty()) {
        s.samples.last().pinWaitUs = us;
    }
}

void LatencyTrace::mark(Stage stage)
{
    State &s = state();
    const qint64 now = nowUs();
    const std::lock_guard lock(s.mutex);
    if (!s.running || s.samples.isEmpty()) {
        return;
    }
    LatencySample &sample = s.samples.last();
    const qint64 offset = now - sample.startedUs;
    if (offset > markWindowUs) {
        s.running = false;
        return;
    }
    if (qint64 *field = stageField(sample, stage); field && *field < 0) {
        *field = offset;
    }
    if (sample.pixelsUs >= 0 && sample.overlayUs >= 0 && sample.fileUs >= 0) {
        s.running = false;
    }
}

bool LatencyTrace::reached(Stage stage)
{
    State &s = state();
    const std::lock_guard lock(s.mutex);
    if (s.samples.isEmpty()) {
        return false;
    }
    const qint64 *field = stageField(s.samples.last(), stage);
    return field && *field >= 0;
}

QJsonArray LatencyTrace::report()
{
    State &s = state();
    const std::lock_guard lock(s.mutex);
    QJsonArray array;
    for (const LatencySample &sample : std::as_const(s.samples)) {
        array.append(sample.toJson());
    }
    return array;
}

void LatencyTrace::resetForTest()
{
    State &s = state();
    const std::lock_guard lock(s.mutex);
    s.samples.clear();
    s.running = false;
}

} // namespace itsova
