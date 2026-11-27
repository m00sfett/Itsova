#include "replayplan.h"
#include "targets.h"

#include <KConfig>
#include <QTemporaryDir>
#include <QtTest>

using namespace itsova;

class ReplayTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void allMonitorsMappedByIdentityEvenWhenRearranged()
    {
        // Recorded: HDMI-A-1 on left (0,0 1920x1080), DP-1 on right (1920,0 3072x1728 @ 1.25).
        RecordingMetadata meta;
        meta.kind = QStringLiteral("all-monitors");
        meta.targetId = allMonitorsTargetId();
        meta.streamScale = 1.25;
        meta.monitors = {
            {QStringLiteral("HDMI-A-1"), QStringLiteral("Acme"), QStringLiteral("FHD"), QStringLiteral("S1"),
             QRect(0, 0, 1920, 1080), QRect(0, 0, 2400, 1350), 1.0, QSize(1920, 1080)},
            {QStringLiteral("DP-1"), QStringLiteral("Acme"), QStringLiteral("UHD"), QStringLiteral("S2"),
             QRect(1920, 0, 3072, 1728), QRect(2400, 0, 3840, 2160), 1.25, QSize(3840, 2160)},
        };

        // Current arrangement: swapped positions and DP-1 now has a different logical size!
        const QList<ScreenInfo> screens{
            {QStringLiteral("DP-2"), QRect(0, 0, 2560, 1440), QStringLiteral("Acme"), QStringLiteral("UHD"),
             QStringLiteral("S2")},
            {QStringLiteral("HDMI-A-2"), QRect(2560, 0, 1920, 1080), QStringLiteral("Acme"), QStringLiteral("FHD"),
             QStringLiteral("S1")},
        };

        const ReplayPlan plan = planReplay(QSize(6240, 2160), meta, screens, {}, QString(), QPoint(100, 100));
        QVERIFY(plan.error.isEmpty());
        QVERIFY(plan.usedMetadata);
        QVERIFY(plan.missingMonitors.isEmpty());
        QCOMPARE(plan.windows.size(), 2);

        // First recorded monitor (S1) matched HDMI-A-2 regardless of connector/position change.
        QCOMPARE(plan.windows.at(0).connector, QStringLiteral("HDMI-A-2"));
        QCOMPARE(plan.windows.at(0).sourceRect, QRectF(0, 0, 2400, 1350));
        QCOMPARE(plan.windows.at(0).targetRect, QRectF(0, 0, 1920, 1080));

        // Second recorded monitor (S2) matched DP-2 and scales to its current 2560x1440 size.
        QCOMPARE(plan.windows.at(1).connector, QStringLiteral("DP-2"));
        QCOMPARE(plan.windows.at(1).sourceRect, QRectF(2400, 0, 3840, 2160));
        QCOMPARE(plan.windows.at(1).targetRect, QRectF(0, 0, 2560, 1440));
    }

    void missingMonitorReportsMissingWhilePlayingRemaining()
    {
        RecordingMetadata meta;
        meta.kind = QStringLiteral("all-monitors");
        meta.monitors = {
            {QStringLiteral("HDMI-A-1"), QStringLiteral("Acme"), QStringLiteral("FHD"), QStringLiteral("S1"),
             QRect(0, 0, 1920, 1080), QRect(0, 0, 1920, 1080), 1.0, QSize(1920, 1080)},
            {QStringLiteral("DP-1"), QStringLiteral("Acme"), QStringLiteral("UHD"), QStringLiteral("S2"),
             QRect(1920, 0, 3072, 1728), QRect(1920, 0, 3840, 2160), 1.25, QSize(3840, 2160)},
        };
        const QList<ScreenInfo> onlyOne{
            {QStringLiteral("HDMI-A-1"), QRect(0, 0, 1920, 1080), QStringLiteral("Acme"), QStringLiteral("FHD"),
             QStringLiteral("S1")},
        };
        const ReplayPlan plan = planReplay(QSize(5760, 2160), meta, onlyOne, {}, QString(), QPoint(10, 10));
        QVERIFY(plan.error.isEmpty());
        QCOMPARE(plan.windows.size(), 1);
        QCOMPARE(plan.windows.first().connector, QStringLiteral("HDMI-A-1"));
        QCOMPARE(plan.missingMonitors.size(), 1);
        QVERIFY(plan.missingMonitors.first().contains(QStringLiteral("UHD")));
    }

    void windowRecordingPlaysAtOriginalPositionAndSize()
    {
        RecordingMetadata meta;
        meta.kind = QStringLiteral("window");
        meta.windowRect = QRect(300, 200, 640, 480);
        meta.monitors = {
            {QStringLiteral("DP-1"), QStringLiteral("Acme"), QStringLiteral("UHD"), QStringLiteral("S2"),
             QRect(0, 0, 1920, 1080), QRect(), 1.0, QSize(1920, 1080)},
        };
        const QList<ScreenInfo> screens{
            {QStringLiteral("DP-1"), QRect(0, 0, 1920, 1080), QStringLiteral("Acme"), QStringLiteral("UHD"),
             QStringLiteral("S2")},
        };
        const ReplayPlan plan = planReplay(QSize(640, 480), meta, screens, {}, QString(), QPoint(0, 0));
        QVERIFY(plan.error.isEmpty());
        QCOMPARE(plan.windows.size(), 1);
        QCOMPARE(plan.windows.first().targetRect, QRectF(300, 200, 640, 480));
    }

    void foreignVideoPlaysAspectFitOnPointerMonitorAndExplicitScreen()
    {
        const QList<ScreenInfo> screens{
            {QStringLiteral("HDMI-A-1"), QRect(0, 0, 1920, 1080), {}, {}, {}},
            {QStringLiteral("DP-1"), QRect(1920, 0, 2560, 1440), {}, {}, {}},
        };
        // Pointer on second monitor, 4:3 video -> pillarboxed on DP-1.
        const ReplayPlan foreign = planReplay(QSize(800, 600), std::nullopt, screens, {}, QString(), QPoint(2100, 500));
        QVERIFY(!foreign.usedMetadata);
        QCOMPARE(foreign.windows.size(), 1);
        QCOMPARE(foreign.windows.first().connector, QStringLiteral("DP-1"));
        QCOMPARE(foreign.windows.first().targetRect, QRectF(320, 0, 1920, 1440));

        // Explicit --screen selects named monitor.
        QHash<QString, QString> names;
        names.insert(TargetStore::monitorKey(screens.first()), QStringLiteral("Left"));
        const ReplayPlan explicitPlan =
            planReplay(QSize(1920, 1080), std::nullopt, screens, names, QStringLiteral("Left"), QPoint(2100, 500));
        QCOMPARE(explicitPlan.windows.size(), 1);
        QCOMPARE(explicitPlan.windows.first().connector, QStringLiteral("HDMI-A-1"));
        QCOMPARE(explicitPlan.windows.first().targetRect, QRectF(0, 0, 1920, 1080));
    }

    void replaySettingsRoundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("itsovarc"));
        {
            TargetStore store;
            QVERIFY(store.replayLoop);
            QVERIFY(store.replayShowCursor);
            QVERIFY(!store.replayFirstHintShown);
            store.replayLoop = false;
            store.replayShowCursor = false;
            store.replayFirstHintShown = true;
            KConfig cfg(path, KConfig::SimpleConfig);
            store.save(cfg);
        }
        {
            KConfig cfg(path, KConfig::SimpleConfig);
            TargetStore loaded;
            loaded.load(cfg);
            QCOMPARE(loaded.replayLoop, false);
            QCOMPARE(loaded.replayShowCursor, false);
            QCOMPARE(loaded.replayFirstHintShown, true);
        }
    }
};

QTEST_GUILESS_MAIN(ReplayTest)
#include "replaytest.moc"
