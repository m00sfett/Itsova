#include "latencytrace.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

using namespace glimply;

class LatencyTraceTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        LatencyTrace::resetForTest();
    }

    void statsIgnoreMissingValues()
    {
        const LatencyStats stats = latencyStats({-1, 400, 100, 300, 200, -1});
        QCOMPARE(stats.count, 4);
        QCOMPARE(stats.medianUs, 250);
        QCOMPARE(stats.p95Us, 400);
        QCOMPARE(stats.maxUs, 400);
        QCOMPARE(latencyStats({}).medianUs, -1);
    }

    void p95UsesNearestRank()
    {
        QList<qint64> values;
        for (int i = 1; i <= 20; ++i) {
            values.append(i);
        }
        QCOMPARE(latencyStats(values).p95Us, 19);
        QCOMPARE(latencyStats(values).medianUs, 10);
    }

    void marksArePerStageAndFirstWins()
    {
        LatencyTrace::begin(2);
        LatencyTrace::setPinWait(25'000);
        LatencyTrace::mark(LatencyTrace::Stage::Request);
        LatencyTrace::mark(LatencyTrace::Stage::Pixels);
        const QJsonObject first = LatencyTrace::report().last().toObject();
        LatencyTrace::mark(LatencyTrace::Stage::Request);
        const QJsonObject second = LatencyTrace::report().last().toObject();
        QCOMPARE(second.value(QStringLiteral("requestUs")), first.value(QStringLiteral("requestUs")));
        QVERIFY(LatencyTrace::reached(LatencyTrace::Stage::Pixels));
        QVERIFY(!LatencyTrace::reached(LatencyTrace::Stage::File));
        QVERIFY(second.value(QStringLiteral("fileUs")).isNull());
        QCOMPARE(second.value(QStringLiteral("pinWaitUs")).toInteger(), 25'000);
        QCOMPARE(second.value(QStringLiteral("monitors")).toInt(), 2);
    }

    void completedTraceIgnoresLaterMarks()
    {
        LatencyTrace::begin(1);
        LatencyTrace::mark(LatencyTrace::Stage::Pixels);
        LatencyTrace::mark(LatencyTrace::Stage::Overlay);
        LatencyTrace::mark(LatencyTrace::Stage::File);
        // An area shortcut after the Print capture must not fill the request of the finished trace.
        LatencyTrace::mark(LatencyTrace::Stage::Request);
        QVERIFY(LatencyTrace::report().last().toObject().value(QStringLiteral("requestUs")).isNull());
    }

    void reportKeepsTheLastFifty()
    {
        for (int i = 0; i < 60; ++i) {
            LatencyTrace::begin(i);
        }
        const QJsonArray report = LatencyTrace::report();
        QCOMPARE(report.size(), 50);
        QCOMPARE(report.first().toObject().value(QStringLiteral("monitors")).toInt(), 10);
    }

    void sampleRoundTripsThroughJson()
    {
        LatencySample sample;
        sample.requestUs = 120;
        sample.pixelsUs = 9'000;
        sample.monitors = 3;
        const LatencySample back = LatencySample::fromJson(sample.toJson());
        QCOMPARE(back.requestUs, 120);
        QCOMPARE(back.pixelsUs, 9'000);
        QCOMPARE(back.overlayUs, -1);
        QCOMPARE(back.monitors, 3);
    }
};

QTEST_GUILESS_MAIN(LatencyTraceTest)
#include "latencytracetest.moc"
