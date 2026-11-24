#include "countdowntimer.h"
#include "targets.h"

#include <KConfig>

#include <QCoreApplication>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace glimply;

class DelayTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void delayPersistenceAndOneShotReset();
    void effectiveDelayPrecedence();
    void countdownHidesBannerBeforeCapture();
    void cancelAbortsCountdownAndFlush();
};

void DelayTest::delayPersistenceAndOneShotReset()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const ScreenInfo screen{QStringLiteral("DP-1"), QRect(0, 0, 1920, 1080), QStringLiteral("Dell"),
                            QStringLiteral("U27"), QStringLiteral("XYZ")};

    TargetStore store;
    store.captureDelaySeconds = 5;
    store.keepDelayActive = false;

    QString areaId;
    {
        Area &area = store.addArea(screen, QRect(10, 20, 100, 80));
        area.delaySeconds = 7;
        areaId = area.id;
    }
    QString groupId;
    {
        Group &group = store.addGroup(QStringLiteral("DelayedGroup"));
        group.targetIds = {areaId};
        group.delaySeconds = 4;
        groupId = group.id;
    }
    store.setTargetDelaySeconds(screenTargetId(screen.connector), 9);

    QVERIFY(store.consumeOneShotDelay());
    QCOMPARE(store.captureDelaySeconds, 0);
    QVERIFY(!store.consumeOneShotDelay());

    store.keepDelayActive = true;
    store.captureDelaySeconds = 6;
    QVERIFY(!store.consumeOneShotDelay());
    QCOMPARE(store.captureDelaySeconds, 6);

    const QString iniPath = QDir(tempDir.path()).filePath(QStringLiteral("glimplyrc"));
    {
        KConfig saveCfg(iniPath, KConfig::SimpleConfig);
        store.save(saveCfg);
    }

    TargetStore loaded;
    {
        KConfig loadCfg(iniPath, KConfig::SimpleConfig);
        loaded.load(loadCfg);
    }
    QCOMPARE(loaded.keepDelayActive, true);
    QCOMPARE(loaded.captureDelaySeconds, 6);
    QCOMPARE(loaded.targetDelaySeconds(areaId), 7);
    QCOMPARE(loaded.targetDelaySeconds(groupId), 4);
    QCOMPARE(loaded.targetDelaySeconds(screenTargetId(screen.connector)), 9);
    QCOMPARE(loaded.targetDelaySeconds(allMonitorsTargetId()), 0);

    loaded.keepDelayActive = false;
    loaded.captureDelaySeconds = 8;
    {
        KConfig saveCfg(iniPath, KConfig::SimpleConfig);
        loaded.save(saveCfg);
    }
    TargetStore reloaded;
    {
        KConfig loadCfg(iniPath, KConfig::SimpleConfig);
        reloaded.load(loadCfg);
    }
    QVERIFY(!reloaded.keepDelayActive);
    QCOMPARE(reloaded.captureDelaySeconds, 0);
}

void DelayTest::effectiveDelayPrecedence()
{
    const ScreenInfo screen{QStringLiteral("HDMI-A-1"), QRect(0, 0, 1920, 1080), {}, {}, {}};
    TargetStore store;
    const QString areaId = store.addArea(screen, QRect(0, 0, 50, 50)).id;
    store.setTargetDelaySeconds(areaId, 7);

    QCOMPARE(store.effectiveDelaySeconds(), 0);
    QCOMPARE(store.effectiveDelaySeconds(areaId), 7);
    QCOMPARE(store.effectiveDelaySeconds(allMonitorsTargetId()), 0);

    store.captureDelaySeconds = 3;
    QCOMPARE(store.effectiveDelaySeconds(), 3);
    QCOMPARE(store.effectiveDelaySeconds(areaId), 3);
    QCOMPARE(store.effectiveDelaySeconds(allMonitorsTargetId()), 3);
}

void DelayTest::countdownHidesBannerBeforeCapture()
{
    CountdownController controller;
    controller.setTickIntervalMs(25);
    controller.setHideFlushDelayMs(35);

    CountdownBanner banner(&controller);

    QSignalSpy startedSpy(&controller, &CountdownController::started);
    QSignalSpy hideSpy(&controller, &CountdownController::hidingForCapture);

    int readyCount = 0;
    bool bannerVisibleDuringReady = true;
    controller.start(2, QStringLiteral("DP-1"), [&] {
        bannerVisibleDuringReady = banner.isVisible();
        ++readyCount;
    });

    QCOMPARE(startedSpy.count(), 1);
    QVERIFY(controller.isActive());
    QCOMPARE(controller.remainingSeconds(), 2);
    QVERIFY(banner.isVisible());

    QTRY_COMPARE_WITH_TIMEOUT(readyCount, 1, 3000);
    QCOMPARE(hideSpy.count(), 1);
    QVERIFY(!bannerVisibleDuringReady);
    QVERIFY(!banner.isVisible());
    QVERIFY(!controller.isActive());
}

void DelayTest::cancelAbortsCountdownAndFlush()
{
    CountdownController controller;
    controller.setTickIntervalMs(30);
    controller.setHideFlushDelayMs(80);

    CountdownBanner banner(&controller);
    int readyCount = 0;
    int cancelledCount = 0;

    // Case A: cancel mid-countdown after first tick.
    controller.start(
        3, QStringLiteral("Selection"),
        [&] { ++readyCount; },
        [&] { ++cancelledCount; });
    QVERIFY(banner.isVisible());
    QTRY_COMPARE_WITH_TIMEOUT(controller.remainingSeconds(), 2, 2000);
    controller.cancel();
    QCOMPARE(cancelledCount, 1);
    QVERIFY(!controller.isActive());
    QVERIFY(!banner.isVisible());
    QTest::qWait(120);
    QCOMPARE(readyCount, 0);

    // Case B: cancel during post-hide flush window before capture executes.
    connect(&controller, &CountdownController::hidingForCapture, &controller, &CountdownController::cancel,
            Qt::SingleShotConnection);
    controller.start(
        1, QStringLiteral("Window"),
        [&] { ++readyCount; },
        [&] { ++cancelledCount; });
    QTRY_COMPARE_WITH_TIMEOUT(cancelledCount, 2, 2000);
    QTest::qWait(120);
    QCOMPARE(readyCount, 0);
    QVERIFY(!controller.isActive());
    QVERIFY(!banner.isVisible());
}

QTEST_MAIN(DelayTest)
#include "delaytest.moc"
