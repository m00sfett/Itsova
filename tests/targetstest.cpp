#include "targets.h"
#include "windowlist.h"
#include "triggerengine.h"
#include "triggerplan.h"

#include <KConfig>
#include <KConfigGroup>

#include <QAtomicInt>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMutex>
#include <QTemporaryDir>
#include <QtTest>

using namespace itsova;

namespace
{

// The target system: DP-1 3840x2160 at scale 1.25, HDMI-A-1 1920x1080 at scale 1, vertically offset.
QList<ScreenInfo> testLayout()
{
    return {
        {QStringLiteral("DP-1"), QRect(1920, 0, 3072, 1728), QStringLiteral("Acme"), QStringLiteral("UHD"),
         QStringLiteral("123")},
        {QStringLiteral("HDMI-A-1"), QRect(0, 318, 1920, 1080), QStringLiteral("Acme"), QStringLiteral("FHD"),
         QString()},
    };
}

CaptureResult fakeFrame(const QString &connector)
{
    if (connector == QStringLiteral("DP-1")) {
        QImage image(3840, 2160, QImage::Format_RGBX8888);
        image.fill(QColor(200, 10, 10));
        return {image, 1.25, {}};
    }
    QImage image(1920, 1080, QImage::Format_RGBX8888);
    image.fill(QColor(10, 10, 200));
    return {image, 1.0, {}};
}

} // namespace

class TargetsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void autoNamesAreSequentialAndUnique();
    void reservedNamesAreRejected();
    void configRoundTrip();
    void legacyRegionConfigIsMigrated();
    void overlayStartMode();
    void lastRecordingTargetBecomesDefaultOnce();
    void recordingAudioRequiresOptIn();
    void defaultRecordingTargetResolution();
    void trayVisibilityRoundTrip();
    void monitorNames();
    void overlayPanelPositionsRoundTrip();
    void removingAreaDropsItFromGroups();
    void groupPlanCapturesEachMonitorOnce();
    void groupCursorOverridesTargets();
    void missingMonitorAndOversizedAreaAreSkipped();
    void monitorIdentityChangeWarns();
    void allMonitorsComposition();
    void groupTriggerSharesTimestampAndFrames();
    void printKeyFirstLaunchPrompt();
};

void TargetsTest::autoNamesAreSequentialAndUnique()
{
    TargetStore store;
    const ScreenInfo dp = testLayout().at(0);
    QCOMPARE(store.addArea(dp, QRect(0, 0, 10, 10)).name, QStringLiteral("DP-1-1"));
    QCOMPARE(store.addArea(dp, QRect(0, 0, 10, 10)).name, QStringLiteral("DP-1-2"));
    store.areas.first().name = QStringLiteral("Chat");
    QCOMPARE(store.addArea(dp, QRect(0, 0, 10, 10)).name, QStringLiteral("DP-1-1"));
    QVERIFY(store.areas.at(0).id != store.areas.at(1).id);
}

void TargetsTest::reservedNamesAreRejected()
{
    TargetStore store;
    store.addArea(testLayout().at(0), QRect(0, 0, 10, 10));
    QVERIFY(!store.isNameAvailable(QStringLiteral("ALL")));
    QVERIFY(!store.isNameAvailable(QStringLiteral("dp-1-1")));
    QVERIFY(!store.isNameAvailable(QStringLiteral("DP-1")));
    QVERIFY(!store.isNameAvailable(QStringLiteral("HDMI-A-1"), {}, {QStringLiteral("HDMI-A-1")}));
    QVERIFY(!store.isNameAvailable(QStringLiteral("   ")));
    QVERIFY(store.isNameAvailable(QStringLiteral("DP-1-1"), store.areas.first().id));
    QVERIFY(store.isNameAvailable(QStringLiteral("Chat")));
}

void TargetsTest::configRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("itsovarc"));

    TargetStore store;
    const auto layout = testLayout();
    Area &a = store.addArea(layout.at(0), QRect(8, 64, 400, 300));
    a.includeCursor = true;
    const QString aId = a.id;
    const QString bId = store.addArea(layout.at(1), QRect(1, 2, 3, 4)).id;
    Group &group = store.addGroup(QStringLiteral("Both"));
    group.targetIds = {aId, bId, allMonitorsTargetId(), screenTargetId(QStringLiteral("DP-1"))};
    group.includeCursor = true;
    store.cursorScreens = {QStringLiteral("HDMI-A-1")};
    store.allMonitorsCursor = true;
    store.captureDirectory = dir.filePath(QStringLiteral("out"));
    store.filenameTemplate = QStringLiteral("{area}_{date}.png");
    store.autostart = false;
    {
        KConfig config(path, KConfig::SimpleConfig);
        store.save(config);
    }

    TargetStore loaded;
    KConfig config(path, KConfig::SimpleConfig);
    loaded.load(config);
    QCOMPARE(loaded.areas.size(), 2);
    QCOMPARE(loaded.areas.at(0).id, aId);
    QCOMPARE(loaded.areas.at(0).name, QStringLiteral("DP-1-1"));
    QCOMPARE(loaded.areas.at(0).rect, QRect(8, 64, 400, 300));
    QCOMPARE(loaded.areas.at(0).monitorIdentity, QStringLiteral("Acme UHD 123"));
    QVERIFY(loaded.areas.at(0).includeCursor);
    QCOMPARE(loaded.areas.at(1).id, bId);
    QCOMPARE(loaded.groups.size(), 1);
    QCOMPARE(loaded.groups.at(0).targetIds, group.targetIds);
    QVERIFY(loaded.groups.at(0).includeCursor);
    QCOMPARE(loaded.cursorScreens, store.cursorScreens);
    QVERIFY(loaded.allMonitorsCursor);
    QCOMPARE(loaded.captureDirectory, store.captureDirectory);
    QCOMPARE(loaded.filenameTemplate, store.filenameTemplate);
    QVERIFY(!loaded.autostart);
    QVERIFY(loaded.autostartInitialized);
}

void TargetsTest::legacyRegionConfigIsMigrated()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("itsovarc"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("[General]\nRegionOrder=region:b,region:a\n\n"
                   "[Region region:a]\nName=DP-1-1\nConnector=DP-1\nRect=8,64,400,300\n\n"
                   "[Region region:b]\nName=HDMI-A-1-1\nConnector=HDMI-A-1\nRect=1,2,3,4\n\n"
                   "[Group group:g]\nName=G\nTargets=region:a,all\n");
    }
    TargetStore store;
    {
        KConfig config(path, KConfig::SimpleConfig);
        store.load(config);
    }
    QCOMPARE(store.areas.size(), 2);
    QCOMPARE(store.areas.at(0).id, QStringLiteral("area:b"));
    QCOMPARE(store.areas.at(1).id, QStringLiteral("area:a"));
    QCOMPARE(store.areas.at(1).rect, QRect(8, 64, 400, 300));
    QCOMPARE(store.groups.first().targetIds, (QStringList{QStringLiteral("area:a"), allMonitorsTargetId()}));
    QCOMPARE(store.migratedIds.value(QStringLiteral("region:a")), QStringLiteral("area:a"));

    {
        KConfig config(path, KConfig::SimpleConfig);
        store.save(config);
    }
    KConfig config(path, KConfig::SimpleConfig);
    QVERIFY(!config.hasGroup(QStringLiteral("Region region:a")));
    QVERIFY(config.hasGroup(QStringLiteral("Area area:a")));
    TargetStore reloaded;
    reloaded.load(config);
    QVERIFY(reloaded.migratedIds.isEmpty());
    QCOMPARE(reloaded.areas.size(), 2);
}

// #124: the shortcut kept recording LastTarget; after the update that target is the default once,
// and a cleared default stays cleared.
void TargetsTest::lastRecordingTargetBecomesDefaultOnce()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("itsovarc"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("[Recording]\nLastTarget=screen:DP-1\n");
    }
    TargetStore store;
    {
        KConfig config(path, KConfig::SimpleConfig);
        store.load(config);
    }
    QCOMPARE(store.defaultRecordingTarget, QStringLiteral("screen:DP-1"));
    store.defaultRecordingTarget.clear();
    {
        KConfig config(path, KConfig::SimpleConfig);
        store.save(config);
    }
    KConfig config(path, KConfig::SimpleConfig);
    QVERIFY(!config.group(QStringLiteral("Recording")).hasKey("LastTarget"));
    TargetStore reloaded;
    reloaded.load(config);
    QVERIFY(reloaded.defaultRecordingTarget.isEmpty());
}

void TargetsTest::recordingAudioRequiresOptIn()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    KConfig config(dir.filePath(QStringLiteral("itsovarc")), KConfig::SimpleConfig);
    TargetStore store;
    store.load(config);
    QVERIFY(!store.experimentalRecordingAudio);
    QVERIFY(store.recordingMicrophoneDevice.isEmpty());
    store.experimentalRecordingAudio = true;
    store.recordingMicrophoneDevice = QStringLiteral("alsa_input.usb-Fixture_Microphone.analog-stereo");
    store.save(config);
    TargetStore restored;
    restored.load(config);
    QVERIFY(restored.experimentalRecordingAudio);
    QCOMPARE(restored.recordingMicrophoneDevice, store.recordingMicrophoneDevice);
    restored.experimentalRecordingAudio = false;
    restored.save(config);
    store.load(config);
    QVERIFY(!store.experimentalRecordingAudio);
    QCOMPARE(store.recordingMicrophoneDevice, restored.recordingMicrophoneDevice);
    store.recordingMicrophoneDevice.clear();
    store.save(config);
    restored.load(config);
    QVERIFY(restored.recordingMicrophoneDevice.isEmpty());
}

void TargetsTest::defaultRecordingTargetResolution()
{
    WindowInfo minimized;
    minimized.handle = QStringLiteral("{min}");
    minimized.minimized = true;
    WindowInfo elsewhere;
    elsewhere.handle = QStringLiteral("{other-desktop}");
    elsewhere.onCurrentDesktop = false;
    WindowInfo konsole;
    konsole.handle = QStringLiteral("{konsole}");
    const QList<WindowInfo> windows{minimized, elsewhere, konsole};

    bool gone = true;
    QCOMPARE(resolveDefaultRecordingTarget(QString(), windows, &gone), QString());
    QVERIFY(!gone);
    QCOMPARE(resolveDefaultRecordingTarget(QStringLiteral("screen:DP-1"), windows, &gone), QStringLiteral("screen:DP-1"));
    // Active window: the most recently used one on this desktop that is not minimized.
    QCOMPARE(resolveDefaultRecordingTarget(activeWindowRecordingTargetId(), windows, &gone),
             windowRecordingTargetId(QStringLiteral("{konsole}")));
    QCOMPARE(resolveDefaultRecordingTarget(activeWindowRecordingTargetId(), {minimized}, &gone), QString());
    QVERIFY(!gone);
    // A specific window: recorded while open, dropped once gone.
    const QString konsoleId = windowRecordingTargetId(QStringLiteral("{konsole}"));
    QCOMPARE(resolveDefaultRecordingTarget(konsoleId, windows, &gone), konsoleId);
    QVERIFY(!gone);
    QCOMPARE(resolveDefaultRecordingTarget(konsoleId, {minimized}, &gone), QString());
    QVERIFY(gone);
}

// #123
void TargetsTest::trayVisibilityRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("itsovarc"));
    TargetStore store;
    QCOMPARE(store.trayVisibility, TargetStore::TrayVisibility::Always);
    for (const auto mode : {TargetStore::TrayVisibility::WhileRecording, TargetStore::TrayVisibility::WhileRunning,
                            TargetStore::TrayVisibility::Always}) {
        store.trayVisibility = mode;
        {
            KConfig config(path, KConfig::SimpleConfig);
            store.save(config);
        }
        KConfig config(path, KConfig::SimpleConfig);
        TargetStore reloaded;
        reloaded.load(config);
        QCOMPARE(reloaded.trayVisibility, mode);
    }
}

void TargetsTest::overlayStartMode()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("itsovarc"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("[General]\nOverlayStartsFrozen=false\n");
    }
    TargetStore store;
    {
        KConfig config(path, KConfig::SimpleConfig);
        store.load(config);
    }
    // "Start frozen" switched off before 0.3.0 becomes a fixed Live start.
    QCOMPARE(store.overlayStart, TargetStore::OverlayStart::Live);
    QVERIFY(!store.overlayStartsFrozen());

    // A fresh configuration remembers the last mode.
    TargetStore fresh;
    QCOMPARE(fresh.overlayStart, TargetStore::OverlayStart::LastUsed);
    QVERIFY(fresh.overlayStartsFrozen());
    fresh.lastOverlayFrozen = false;
    QVERIFY(!fresh.overlayStartsFrozen());
    QVERIFY(!fresh.overlayFirstHintShown);
    fresh.overlayFirstHintShown = true;
    {
        KConfig config(path, KConfig::SimpleConfig);
        config.group(QStringLiteral("General")).writeEntry("OverlayHelpExpanded", true); // 0.8.0 key
        fresh.save(config);
    }
    KConfig config(path, KConfig::SimpleConfig);
    QVERIFY(!config.group(QStringLiteral("General")).hasKey("OverlayStartsFrozen"));
    QVERIFY(!config.group(QStringLiteral("General")).hasKey("OverlayHelpExpanded")); // #72
    TargetStore reloaded;
    reloaded.load(config);
    QCOMPARE(reloaded.overlayStart, TargetStore::OverlayStart::LastUsed);
    QVERIFY(!reloaded.lastOverlayFrozen);
    QVERIFY(reloaded.overlayFirstHintShown);
}

void TargetsTest::monitorNames()
{
    TargetStore store;
    const auto layout = testLayout();
    const ScreenInfo dp = layout.at(0);
    const ScreenInfo hdmi = layout.at(1);
    QCOMPARE(store.monitorName(dp), QStringLiteral("DP-1"));
    store.addArea(dp, QRect(0, 0, 10, 10)); // DP-1-1, named before the monitor got a name

    // The name belongs to the physical monitor, not to the connector.
    store.setMonitorName(dp, QStringLiteral("Left"));
    QCOMPARE(store.monitorName(dp), QStringLiteral("Left"));
    ScreenInfo moved = dp;
    moved.connector = QStringLiteral("DP-2");
    QCOMPARE(store.monitorName(moved), QStringLiteral("Left"));
    // A monitor without identity falls back to its connector as the key.
    ScreenInfo anonymous{QStringLiteral("DP-3"), QRect(0, 0, 100, 100), {}, {}, {}};
    QCOMPARE(TargetStore::monitorKey(anonymous), QStringLiteral("connector:DP-3"));

    // New automatic area names use the monitor name; existing ones stay.
    QCOMPARE(store.addArea(dp, QRect(0, 0, 10, 10)).name, QStringLiteral("Left-1"));
    QCOMPARE(store.areas.first().name, QStringLiteral("DP-1-1"));

    // Monitor names, area and group names must not collide.
    QVERIFY(!store.isNameAvailable(QStringLiteral("left")));
    QVERIFY(store.isNameAvailable(QStringLiteral("Left"), TargetStore::monitorKey(dp)));
    QVERIFY(!store.isNameAvailable(QStringLiteral("Left-1"), TargetStore::monitorKey(hdmi)));

    // The file name token and display name of a monitor capture follow the name.
    const TriggerPlan plan = planTrigger(store, screenTargetId(QStringLiteral("DP-1")), layout);
    QCOMPARE(plan.outputs.size(), 1);
    QCOMPARE(plan.outputs.first().token, QStringLiteral("Left"));
    QCOMPARE(plan.outputs.first().connector, QStringLiteral("DP-1"));
    QCOMPARE(plan.displayName, QStringLiteral("Left"));

    // Survives a save and load; an empty name or the connector itself removes it.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("itsovarc"));
    {
        KConfig config(path, KConfig::SimpleConfig);
        store.save(config);
    }
    TargetStore reloaded;
    {
        KConfig config(path, KConfig::SimpleConfig);
        reloaded.load(config);
    }
    QCOMPARE(reloaded.monitorName(dp), QStringLiteral("Left"));
    reloaded.setMonitorName(dp, QStringLiteral("DP-1"));
    QCOMPARE(reloaded.monitorName(dp), QStringLiteral("DP-1"));
    QVERIFY(reloaded.monitorNames.isEmpty());
    store.setMonitorName(dp, QStringLiteral("  "));
    QVERIFY(store.monitorNames.isEmpty());
}

void TargetsTest::overlayPanelPositionsRoundTrip()
{
    const ScreenInfo dp = testLayout().at(0);
    TargetStore store;
    store.overlayPanelPositions.insert(TargetStore::monitorKey(dp), QPoint(40, 900));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("itsovarc"));
    {
        KConfig config(path, KConfig::SimpleConfig);
        store.save(config);
    }
    TargetStore reloaded;
    {
        KConfig config(path, KConfig::SimpleConfig);
        reloaded.load(config);
    }
    QCOMPARE(reloaded.overlayPanelPositions.value(TargetStore::monitorKey(dp)), QPoint(40, 900));

    // Resetting removes the entry from the file, too.
    reloaded.overlayPanelPositions.clear();
    {
        KConfig config(path, KConfig::SimpleConfig);
        reloaded.save(config);
    }
    TargetStore again;
    {
        KConfig config(path, KConfig::SimpleConfig);
        again.load(config);
    }
    QVERIFY(again.overlayPanelPositions.isEmpty());
}

void TargetsTest::removingAreaDropsItFromGroups()
{
    TargetStore store;
    const QString id = store.addArea(testLayout().at(0), QRect(0, 0, 10, 10)).id;
    store.addGroup(QStringLiteral("G")).targetIds = {id, allMonitorsTargetId()};
    store.remove(id);
    QVERIFY(store.areas.isEmpty());
    QCOMPARE(store.groups.first().targetIds, QStringList{allMonitorsTargetId()});
}

void TargetsTest::groupPlanCapturesEachMonitorOnce()
{
    TargetStore store;
    const auto layout = testLayout();
    const QString a = store.addArea(layout.at(0), QRect(0, 0, 400, 300)).id;
    const QString b = store.addArea(layout.at(0), QRect(100, 100, 50, 50)).id;
    const QString c = store.addArea(layout.at(1), QRect(0, 0, 400, 300)).id;
    Group &group = store.addGroup(QStringLiteral("G"));
    group.targetIds = {a, b, c, screenTargetId(QStringLiteral("DP-1")), a};

    const TriggerPlan plan = planTrigger(store, group.id, layout);
    QVERIFY(plan.errors.isEmpty());
    QCOMPARE(plan.connectors, (QStringList{QStringLiteral("DP-1"), QStringLiteral("HDMI-A-1")}));
    QCOMPARE(plan.outputs.size(), 4);
}

void TargetsTest::groupCursorOverridesTargets()
{
    TargetStore store;
    Area &area = store.addArea(testLayout().at(0), QRect(0, 0, 10, 10));
    area.includeCursor = true;
    const QString areaId = area.id;
    Group &group = store.addGroup(QStringLiteral("G"));
    group.targetIds = {areaId};
    QVERIFY(!planTrigger(store, group.id, testLayout()).includeCursor);
    QVERIFY(planTrigger(store, areaId, testLayout()).includeCursor);
}

void TargetsTest::missingMonitorAndOversizedAreaAreSkipped()
{
    TargetStore store;
    const auto layout = testLayout();
    const QString tooBig = store.addArea(layout.at(1), QRect(1800, 0, 400, 300)).id;
    const QString elsewhere = store.addArea({QStringLiteral("DP-2"), QRect(0, 0, 100, 100), {}, {}, {}}, QRect(0, 0, 10, 10)).id;
    const QString fine = store.addArea(layout.at(1), QRect(0, 0, 10, 10)).id;
    Group &group = store.addGroup(QStringLiteral("G"));
    group.targetIds = {tooBig, elsewhere, fine, screenTargetId(QStringLiteral("DP-3"))};

    const TriggerPlan plan = planTrigger(store, group.id, layout);
    QCOMPARE(plan.errors.size(), 3);
    QCOMPARE(plan.outputs.size(), 1);
    QCOMPARE(plan.connectors, QStringList{QStringLiteral("HDMI-A-1")});
}

void TargetsTest::monitorIdentityChangeWarns()
{
    TargetStore store;
    auto layout = testLayout();
    const QString id = store.addArea(layout.at(0), QRect(0, 0, 10, 10)).id;
    layout[0].serialNumber = QStringLiteral("999");
    const TriggerPlan plan = planTrigger(store, id, layout);
    QVERIFY(plan.errors.isEmpty());
    QCOMPARE(plan.warnings.size(), 1);
    QCOMPARE(plan.outputs.size(), 1);
}

void TargetsTest::allMonitorsComposition()
{
    const auto layout = testLayout();
    QHash<QString, CaptureResult> frames;
    for (const ScreenInfo &screen : layout) {
        frames.insert(screen.connector, fakeFrame(screen.connector));
    }
    QString error;
    const QImage image = composeAllMonitors(layout, frames, &error);
    QVERIFY2(!image.isNull(), qPrintable(error));
    QCOMPARE(image.size(), QSize(6240, 2160)); // A10
    QVERIFY(image.hasAlphaChannel());
    // Above and below the vertically offset HDMI monitor stays transparent.
    QCOMPARE(qAlpha(image.pixel(10, 10)), 0);
    QCOMPARE(qAlpha(image.pixel(10, 2150)), 0);
    // HDMI-A-1 is upscaled to y 398..1748, x 0..2400.
    QCOMPARE(image.pixelColor(1200, 1000), QColor(10, 10, 200));
    QCOMPARE(qAlpha(image.pixel(1200, 396)), 0);
    QCOMPARE(qAlpha(image.pixel(1200, 1749)), 0);
    // DP-1 covers x 2400..6240 at native resolution.
    QCOMPARE(image.pixelColor(2400, 0), QColor(200, 10, 10));
    QCOMPARE(image.pixelColor(6239, 2159), QColor(200, 10, 10));
}

void TargetsTest::groupTriggerSharesTimestampAndFrames()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    TargetStore store;
    const auto layout = testLayout();
    const QString a = store.addArea(layout.at(0), QRect(0, 0, 400, 300)).id;
    const QString b = store.addArea(layout.at(1), QRect(0, 0, 400, 300)).id;
    Group &group = store.addGroup(QStringLiteral("G"));
    group.targetIds = {a, b, screenTargetId(QStringLiteral("HDMI-A-1")), allMonitorsTargetId()};

    QMutex mutex;
    QStringList captured;
    const TriggerEngine::Capturer capturer = [&](const QString &connector, bool) {
        const QMutexLocker lock(&mutex);
        captured.append(connector);
        return fakeFrame(connector);
    };
    const QDateTime timestamp(QDate(2026, 11, 22), QTime(14, 5, 33));
    const TriggerResult result = TriggerEngine::execute(planTrigger(store, group.id, layout), capturer, dir.path(),
                                                        TargetStore::defaultFilenameTemplate(), timestamp);
    QVERIFY2(result.errors.isEmpty(), qPrintable(result.errors.join(QLatin1Char('\n'))));
    captured.sort();
    QCOMPARE(captured, (QStringList{QStringLiteral("DP-1"), QStringLiteral("HDMI-A-1")})); // one frame per monitor
    QCOMPARE(result.capturedMonitors, 2);

    QStringList names;
    for (const QString &path : result.writtenPaths) {
        names.append(QFileInfo(path).fileName());
    }
    QCOMPARE(names, (QStringList{QStringLiteral("2026-11-22_14-05-33_DP-1-1.png"),
                                 QStringLiteral("2026-11-22_14-05-33_HDMI-A-1-1.png"),
                                 QStringLiteral("2026-11-22_14-05-33_HDMI-A-1.png"),
                                 QStringLiteral("2026-11-22_14-05-33_all.png")}));
    QCOMPARE(QImageReader(result.writtenPaths.at(0)).size(), QSize(500, 375)); // A8
    QCOMPARE(QImageReader(result.writtenPaths.at(1)).size(), QSize(400, 300));
    QCOMPARE(QImageReader(result.writtenPaths.at(3)).size(), QSize(6240, 2160));
}

void TargetsTest::printKeyFirstLaunchPrompt()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cfgPath = dir.filePath(QStringLiteral("freshrc"));

    // Fresh config: hasExistingConfig is false -> prompt should be shown.
    {
        KConfig fresh(cfgPath, KConfig::SimpleConfig);
        TargetStore store;
        store.load(fresh);
        QVERIFY(!store.hasExistingConfig);
        QVERIFY(!store.saveImmediatelyOnPrint);
        QVERIFY(shouldPromptForPrintKey(fresh, store.hasExistingConfig));

        // Declining persists Claimed=false so the prompt does not reappear.
        markPrintKeyDeclined(fresh);
        QVERIFY(!shouldPromptForPrintKey(fresh, false));
        QCOMPARE(fresh.group(QStringLiteral("PrintKey")).readEntry("Claimed", true), false);
    }

    // Existing user config without [PrintKey]: do not prompt again (#137).
    const QString existingPath = dir.filePath(QStringLiteral("existingrc"));
    {
        KConfig existing(existingPath, KConfig::SimpleConfig);
        existing.group(QStringLiteral("General")).writeEntry("UsePrintKey", true);
        existing.sync();
    }
    {
        KConfig existing(existingPath, KConfig::SimpleConfig);
        TargetStore store;
        store.load(existing);
        QVERIFY(store.hasExistingConfig);
        QVERIFY(!shouldPromptForPrintKey(existing, store.hasExistingConfig));
    }
}

QTEST_GUILESS_MAIN(TargetsTest)
#include "targetstest.moc"
