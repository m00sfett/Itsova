#include "intervalcontroller.h"
#include "targets.h"
#include "triggerengine.h"
#include "triggerplan.h"

#include <KConfig>

#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <cstring>

using namespace glimply;

namespace
{

QImage solidImage(int w, int h, const QColor &color, QImage::Format fmt = QImage::Format_RGBA8888)
{
    QImage img(w, h, fmt);
    img.fill(color);
    return img;
}

} // namespace

class IntervalTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void configRoundTrip();
    void pixelEqualAndStrideSafety();
    void changeDetectionSingleAndGroup();
    void stopAfterMaxCapturesIgnoresSkipped();
    void stopAfterMaxMinutes();
    void lowDiskAndDisconnectGuardrails();
    void concurrentIndependentIntervals();
    void unchangedGroupWithSkippedMemberKeepsRunning();
};

void IntervalTest::configRoundTrip()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const ScreenInfo screen{QStringLiteral("DP-1"), QRect(0, 0, 1920, 1080), QStringLiteral("Dell"),
                            QStringLiteral("U27"), QStringLiteral("123")};

    TargetStore store;
    Area &area = store.addArea(screen, QRect(10, 20, 200, 120));
    area.interval = IntervalConfig{12, 50, 30, true};
    const QString areaId = area.id;

    Group &group = store.addGroup(QStringLiteral("Timelapse"));
    group.targetIds = {areaId, allMonitorsTargetId()};
    group.interval = IntervalConfig{60, 0, 10, false};
    const QString groupId = group.id;

    store.setTargetIntervalConfig(screenTargetId(QStringLiteral("DP-1")), IntervalConfig{3, 5, 0, true});
    store.setTargetIntervalConfig(allMonitorsTargetId(), IntervalConfig{15, 0, 0, true});

    const QString iniPath = QDir(tempDir.path()).filePath(QStringLiteral("glimplyrc"));
    {
        KConfig cfg(iniPath, KConfig::SimpleConfig);
        store.save(cfg);
    }

    TargetStore loaded;
    {
        KConfig cfg(iniPath, KConfig::SimpleConfig);
        loaded.load(cfg);
    }

    QCOMPARE(loaded.targetIntervalConfig(areaId), (IntervalConfig{12, 50, 30, true}));
    QCOMPARE(loaded.targetIntervalConfig(groupId), (IntervalConfig{60, 0, 10, false}));
    QCOMPARE(loaded.targetIntervalConfig(screenTargetId(QStringLiteral("DP-1"))), (IntervalConfig{3, 5, 0, true}));
    QCOMPARE(loaded.targetIntervalConfig(allMonitorsTargetId()), (IntervalConfig{15, 0, 0, true}));
    QCOMPARE(loaded.targetIntervalConfig(screenTargetId(QStringLiteral("HDMI-A-1"))), IntervalConfig{});

    loaded.setTargetIntervalConfig(allMonitorsTargetId(), IntervalConfig{});
    QCOMPARE(loaded.builtInIntervals.contains(allMonitorsTargetId()), false);
}

void IntervalTest::pixelEqualAndStrideSafety()
{
    const QImage a = solidImage(15, 10, QColor(10, 20, 30), QImage::Format_RGB32);
    const QImage b = solidImage(15, 10, QColor(10, 20, 30), QImage::Format_ARGB32_Premultiplied);
    QVERIFY(TriggerEngine::imagesPixelEqual(a, b));

    QImage diff = a.copy();
    diff.setPixelColor(14, 9, QColor(11, 20, 30));
    QVERIFY(!TriggerEngine::imagesPixelEqual(a, diff));

    QVERIFY(!TriggerEngine::imagesPixelEqual(a, solidImage(16, 10, QColor(10, 20, 30))));
    QVERIFY(!TriggerEngine::imagesPixelEqual(QImage(), a));

    // Verify unused stride padding bytes on indexed/byte-padded images do not trigger false diffs.
    QImage pad1(5, 4, QImage::Format_Grayscale8);
    pad1.fill(128);
    QImage pad2 = pad1.copy();
    QVERIFY(pad1.bytesPerLine() >= 8);
    pad1.scanLine(0)[5] = 0x11;
    pad2.scanLine(0)[5] = 0xEE;
    QVERIFY(TriggerEngine::imagesPixelEqual(pad1, pad2));
}

void IntervalTest::changeDetectionSingleAndGroup()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const ScreenInfo dp1{QStringLiteral("DP-1"), QRect(0, 0, 100, 100), {}, {}, {}};
    const ScreenInfo hdmi{QStringLiteral("HDMI-A-1"), QRect(100, 0, 100, 100), {}, {}, {}};
    const QList<ScreenInfo> screens{dp1, hdmi};

    TargetStore store;
    const QString areaDp = store.addArea(dp1, QRect(0, 0, 40, 40)).id;
    const QString areaHdmi = store.addArea(hdmi, QRect(0, 0, 40, 40)).id;
    Group &grp = store.addGroup(QStringLiteral("Dual"));
    grp.targetIds = {areaDp, areaHdmi};

    QImage dpImg = solidImage(100, 100, Qt::blue);
    QImage hdmiImg = solidImage(100, 100, Qt::green);
    auto capturer = [&](const QString &conn, bool) -> CaptureResult {
        return {conn == QLatin1String("DP-1") ? dpImg : hdmiImg, 1.0, {}};
    };

    TriggerPlan plan = planTrigger(store, grp.id, screens);
    TriggerEngine::FrameMap cache;

    // Tick 1: both members saved.
    {
        TriggerEngine::FrameMap updated;
        const TriggerResult r = TriggerEngine::executeWithChangeFilter(
            plan, capturer, tempDir.path(), TargetStore::defaultFilenameTemplate(),
            QDateTime(QDate(2026, 11, 24), QTime(15, 0, 0)), &cache, &updated);
        QCOMPARE(r.writtenPaths.size(), 2);
        QCOMPARE(r.skippedUnchanged, 0);
        for (auto it = updated.cbegin(); it != updated.cend(); ++it) {
            cache.insert(it.key(), it.value());
        }
    }

    // Tick 2: neither changed -> zero files written.
    {
        TriggerEngine::FrameMap updated;
        const TriggerResult r = TriggerEngine::executeWithChangeFilter(
            plan, capturer, tempDir.path(), TargetStore::defaultFilenameTemplate(),
            QDateTime(QDate(2026, 11, 24), QTime(15, 0, 5)), &cache, &updated);
        QCOMPARE(r.writtenPaths.size(), 0);
        QCOMPARE(r.skippedUnchanged, 2);
        QCOMPARE(updated.size(), 0);
    }

    // Tick 3: only HDMI frame changes -> only 1 member written, unchanged skipped.
    hdmiImg.setPixelColor(10, 10, Qt::red);
    {
        TriggerEngine::FrameMap updated;
        const TriggerResult r = TriggerEngine::executeWithChangeFilter(
            plan, capturer, tempDir.path(), TargetStore::defaultFilenameTemplate(),
            QDateTime(QDate(2026, 11, 24), QTime(15, 0, 10)), &cache, &updated);
        QCOMPARE(r.writtenPaths.size(), 1);
        QCOMPARE(r.skippedUnchanged, 1);
        QVERIFY(r.writtenPaths.first().contains(QStringLiteral("HDMI-A-1-1")));
    }
}

void IntervalTest::stopAfterMaxCapturesIgnoresSkipped()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const ScreenInfo dp1{QStringLiteral("DP-1"), QRect(0, 0, 64, 64), {}, {}, {}};
    TargetStore store;
    const QString areaId = store.addArea(dp1, QRect(0, 0, 32, 32)).id;

    int tickCounter = 0;
    QImage frame0 = solidImage(64, 64, Qt::white);
    QImage frameChanged = solidImage(64, 64, Qt::black);

    IntervalController ctrl(
        [&](const QString &id) { return planTrigger(store, id, {dp1}); },
        [&] { return tempDir.path(); },
        [] { return TargetStore::defaultFilenameTemplate(); },
        [&](TriggerPlan plan, const QString &dir, const QString &tmpl, std::optional<TriggerEngine::FrameMap> prev,
            std::function<void(TriggerResult, TriggerEngine::FrameMap)> done) {
            ++tickCounter;
            // Tick 1: white (save #1); Ticks 2 & 3: white (skipped!); Tick 4: black (save #2 -> stop)
            const QImage &src = (tickCounter <= 3) ? frame0 : frameChanged;
            TriggerEngine::FrameMap updated;
            const TriggerResult r = TriggerEngine::executeWithChangeFilter(
                plan, [&](const QString &, bool) { return CaptureResult{src, 1.0, {}}; }, dir, tmpl,
                QDateTime::currentDateTime().addSecs(tickCounter), prev ? &*prev : nullptr, &updated);
            done(r, updated);
        });
    ctrl.setDiskSpaceProvider([](const QString &) { return IntervalController::minFreeDiskBytes * 10; });
    ctrl.setTickIntervalProvider([](int) { return 10; });

    QSignalSpy savedSpy(&ctrl, &IntervalController::captureSaved);
    QSignalSpy finishedSpy(&ctrl, &IntervalController::intervalFinished);

    QCOMPARE(ctrl.start(areaId, QStringLiteral("DP-1-1"), IntervalConfig{1, 2, 0, true}), QString());
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 2000);
    QCOMPARE(savedSpy.count(), 2);
    QCOMPARE(tickCounter, 4);
    QCOMPARE(finishedSpy.at(0).at(2).toInt(), 2);
    QVERIFY(!ctrl.isActive(areaId));
}

void IntervalTest::stopAfterMaxMinutes()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const ScreenInfo dp1{QStringLiteral("DP-1"), QRect(0, 0, 64, 64), {}, {}, {}};
    TargetStore store;
    const QString areaId = store.addArea(dp1, QRect(0, 0, 32, 32)).id;

    qint64 nowMs = 1000;
    int savedTicks = 0;

    IntervalController ctrl(
        [&](const QString &id) { return planTrigger(store, id, {dp1}); },
        [&] { return tempDir.path(); },
        [] { return TargetStore::defaultFilenameTemplate(); },
        [&](TriggerPlan plan, const QString &dir, const QString &tmpl, std::optional<TriggerEngine::FrameMap>,
            std::function<void(TriggerResult, TriggerEngine::FrameMap)> done) {
            ++savedTicks;
            TriggerEngine::FrameMap updated;
            const TriggerResult r = TriggerEngine::executeWithChangeFilter(
                plan, [&](const QString &, bool) { return CaptureResult{solidImage(64, 64, Qt::cyan), 1.0, {}}; },
                dir, tmpl, QDateTime::currentDateTime().addSecs(savedTicks), nullptr, &updated);
            // Advance clock past maxMinutes (1 min = 60,000 ms) before tick 2 checks.
            nowMs += 65'000;
            done(r, updated);
        });
    ctrl.setDiskSpaceProvider([](const QString &) { return IntervalController::minFreeDiskBytes * 10; });
    ctrl.setClockProvider([&] { return nowMs; });
    ctrl.setTickIntervalProvider([](int) { return 10; });

    QSignalSpy finishedSpy(&ctrl, &IntervalController::intervalFinished);
    QCOMPARE(ctrl.start(areaId, QStringLiteral("DP-1-1"), IntervalConfig{1, 0, 1, false}), QString());
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 2000);
    QCOMPARE(savedTicks, 1);
    QVERIFY(!ctrl.hasActive());
}

void IntervalTest::lowDiskAndDisconnectGuardrails()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const ScreenInfo dp1{QStringLiteral("DP-1"), QRect(0, 0, 64, 64), {}, {}, {}};
    TargetStore store;
    const QString areaId = store.addArea(dp1, QRect(0, 0, 32, 32)).id;

    // 1. Disconnected target at start returns an error immediately.
    {
        IntervalController ctrl(
            [&](const QString &id) { return planTrigger(store, id, {}); },
            [&] { return tempDir.path(); },
            [] { return TargetStore::defaultFilenameTemplate(); },
            [&](TriggerPlan, const QString &, const QString &, std::optional<TriggerEngine::FrameMap>,
                std::function<void(TriggerResult, TriggerEngine::FrameMap)>) {});
        const QString err = ctrl.start(areaId, QStringLiteral("DP-1-1"), IntervalConfig{1, 0, 0, false});
        QVERIFY(!err.isEmpty());
        QVERIFY(!ctrl.hasActive());
    }

    // 2. Low free disk space stops cleanly with one error notification.
    {
        qint64 avail = IntervalController::minFreeDiskBytes - 1;
        IntervalController ctrl(
            [&](const QString &id) { return planTrigger(store, id, {dp1}); },
            [&] { return tempDir.path(); },
            [] { return TargetStore::defaultFilenameTemplate(); },
            [&](TriggerPlan, const QString &, const QString &, std::optional<TriggerEngine::FrameMap>,
                std::function<void(TriggerResult, TriggerEngine::FrameMap)>) { QFAIL("Should not capture"); });
        ctrl.setDiskSpaceProvider([&](const QString &) { return avail; });
        QSignalSpy errSpy(&ctrl, &IntervalController::intervalStoppedWithError);
        QCOMPARE(ctrl.start(areaId, QStringLiteral("DP-1-1"), IntervalConfig{1, 0, 0, false}), QString());
        QTRY_COMPARE_WITH_TIMEOUT(errSpy.count(), 1, 2000);
        QVERIFY(errSpy.at(0).at(2).toString().contains(QStringLiteral("100 MB")));
        QVERIFY(!ctrl.hasActive());
    }

    // 3. Mid-run monitor disconnect stops cleanly with one error notification.
    {
        QList<ScreenInfo> currentLayout{dp1};
        IntervalController ctrl(
            [&](const QString &id) { return planTrigger(store, id, currentLayout); },
            [&] { return tempDir.path(); },
            [] { return TargetStore::defaultFilenameTemplate(); },
            [&](TriggerPlan plan, const QString &dir, const QString &tmpl, std::optional<TriggerEngine::FrameMap>,
                std::function<void(TriggerResult, TriggerEngine::FrameMap)> done) {
                TriggerEngine::FrameMap updated;
                const TriggerResult r = TriggerEngine::executeWithChangeFilter(
                    plan, [&](const QString &, bool) { return CaptureResult{solidImage(64, 64, Qt::red), 1.0, {}}; },
                    dir, tmpl, QDateTime::currentDateTime(), nullptr, &updated);
                currentLayout.clear(); // simulate monitor unplug after tick 1
                done(r, updated);
            });
        ctrl.setDiskSpaceProvider([](const QString &) { return IntervalController::minFreeDiskBytes * 10; });
        ctrl.setTickIntervalProvider([](int) { return 10; });

        QSignalSpy errSpy(&ctrl, &IntervalController::intervalStoppedWithError);
        QCOMPARE(ctrl.start(areaId, QStringLiteral("DP-1-1"), IntervalConfig{1, 0, 0, false}), QString());
        QTRY_COMPARE_WITH_TIMEOUT(errSpy.count(), 1, 2000);
        QVERIFY(!ctrl.hasActive());
    }
}

void IntervalTest::concurrentIndependentIntervals()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const ScreenInfo dp1{QStringLiteral("DP-1"), QRect(0, 0, 64, 64), {}, {}, {}};
    const ScreenInfo hdmi{QStringLiteral("HDMI-A-1"), QRect(64, 0, 64, 64), {}, {}, {}};
    const QList<ScreenInfo> screens{dp1, hdmi};

    TargetStore store;
    const QString id1 = screenTargetId(QStringLiteral("DP-1"));
    const QString id2 = screenTargetId(QStringLiteral("HDMI-A-1"));

    int seq = 0;
    IntervalController ctrl(
        [&](const QString &id) { return planTrigger(store, id, screens); },
        [&] { return tempDir.path(); },
        [] { return TargetStore::defaultFilenameTemplate(); },
        [&](TriggerPlan plan, const QString &dir, const QString &tmpl, std::optional<TriggerEngine::FrameMap>,
            std::function<void(TriggerResult, TriggerEngine::FrameMap)> done) {
            ++seq;
            TriggerEngine::FrameMap updated;
            const TriggerResult r = TriggerEngine::executeWithChangeFilter(
                plan,
                [&](const QString &, bool) { return CaptureResult{solidImage(64, 64, Qt::darkBlue), 1.0, {}}; },
                dir, tmpl, QDateTime::currentDateTime().addSecs(seq), nullptr, &updated);
            done(r, updated);
        });
    ctrl.setDiskSpaceProvider([](const QString &) { return IntervalController::minFreeDiskBytes * 10; });
    ctrl.setTickIntervalProvider([](int) { return 50; });

    QCOMPARE(ctrl.start(id1, QStringLiteral("DP-1"), IntervalConfig{1, 0, 0, false}), QString());
    QCOMPARE(ctrl.start(id2, QStringLiteral("HDMI-A-1"), IntervalConfig{2, 0, 0, false}), QString());
    QCOMPARE(ctrl.activeIntervals().size(), 2);
    QVERIFY(ctrl.isActive(id1));
    QVERIFY(ctrl.isActive(id2));

    QVERIFY(ctrl.stop(id1));
    QVERIFY(!ctrl.isActive(id1));
    QVERIFY(ctrl.isActive(id2));
    QCOMPARE(ctrl.activeIntervals().size(), 1);

    ctrl.stopAll();
    QVERIFY(!ctrl.hasActive());
}

void IntervalTest::unchangedGroupWithSkippedMemberKeepsRunning()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const ScreenInfo dp1{QStringLiteral("DP-1"), QRect(0, 0, 64, 64), {}, {}, {}};
    const ScreenInfo hdmi{QStringLiteral("HDMI-A-1"), QRect(64, 0, 64, 64), {}, {}, {}};
    TargetStore store;
    const QString areaDp = store.addArea(dp1, QRect(0, 0, 32, 32)).id;
    const QString areaHdmi = store.addArea(hdmi, QRect(0, 0, 32, 32)).id;
    Group &grp = store.addGroup(QStringLiteral("Dual"));
    grp.targetIds = {areaDp, areaHdmi};
    const QString groupId = grp.id;

    int ticks = 0;
    const QImage frame = solidImage(64, 64, Qt::white);
    // HDMI-A-1 is unplugged: every plan carries a "skipped" error for that member.
    IntervalController ctrl(
        [&](const QString &id) { return planTrigger(store, id, {dp1}); },
        [&] { return tempDir.path(); },
        [] { return TargetStore::defaultFilenameTemplate(); },
        [&](TriggerPlan plan, const QString &dir, const QString &tmpl, std::optional<TriggerEngine::FrameMap> prev,
            std::function<void(TriggerResult, TriggerEngine::FrameMap)> done) {
            ++ticks;
            TriggerEngine::FrameMap updated;
            const TriggerResult r = TriggerEngine::executeWithChangeFilter(
                plan, [&](const QString &, bool) { return CaptureResult{frame, 1.0, {}}; }, dir, tmpl,
                QDateTime::currentDateTime().addSecs(ticks), prev ? &*prev : nullptr, &updated);
            done(r, updated);
        });
    ctrl.setDiskSpaceProvider([](const QString &) { return IntervalController::minFreeDiskBytes * 10; });
    ctrl.setTickIntervalProvider([](int) { return 10; });

    QSignalSpy errorSpy(&ctrl, &IntervalController::intervalStoppedWithError);
    QCOMPARE(ctrl.start(groupId, QStringLiteral("Dual"), IntervalConfig{1, 0, 0, true}), QString());
    QTRY_VERIFY_WITH_TIMEOUT(ticks >= 4, 2000);
    QCOMPARE(errorSpy.count(), 0);
    QVERIFY(ctrl.isActive(groupId));
    ctrl.stopAll();
}

QTEST_MAIN(IntervalTest)
#include "intervaltest.moc"
