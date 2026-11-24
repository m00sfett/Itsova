#include "imageoutput.h"
#include "selectiongeometry.h"
#include "selectionmodel.h"
#include "targets.h"
#include "triggerengine.h"
#include "triggerplan.h"

#include <KConfig>
#include <KConfigGroup>

#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QTemporaryDir>
#include <QtTest>

#include <cstring>

using namespace glimply;

namespace
{

QList<ScreenInfo> dualLayout()
{
    return {
        {QStringLiteral("HDMI-A-1"), QRect(0, 318, 1920, 1080), QStringLiteral("Acme, Inc"), QStringLiteral("FHD"),
         QStringLiteral("H-1")},
        {QStringLiteral("DP-1"), QRect(1920, 0, 3072, 1728), QStringLiteral("Acme, Inc"), QStringLiteral("UHD"),
         QStringLiteral("D-1")},
    };
}

CaptureResult dualFrame(const QString &connector)
{
    if (connector == QStringLiteral("DP-1")) {
        QImage img(3840, 2160, QImage::Format_RGBX8888);
        img.fill(QColor(210, 20, 20));
        return {img, 1.25, {}};
    }
    if (connector == QStringLiteral("HDMI-A-1")) {
        QImage img(1920, 1080, QImage::Format_RGBX8888);
        img.fill(QColor(20, 40, 220));
        return {img, 1.0, {}};
    }
    if (connector == QStringLiteral("DP-2")) {
        QImage img(1280, 720, QImage::Format_RGBX8888);
        img.fill(QColor(20, 200, 40));
        return {img, 1.0, {}};
    }
    return {};
}

} // namespace

class CrossMonitorTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void edgeSnapWithinEightPixelsAndReleaseBeyond();
    void singleMonitorFastPathIsByteIdentical();
    void crossMonitorMixedDpiCompositionAndJpegGap();
    void partialCaptureWhenOneMonitorDisconnected();
    void threeMonitorPartialCaptureComposesRemainingTwo();
    void zeroConnectedMonitorsSavesNothingWithPassiveNotice();
    void configRoundTripSingleAndCrossMonitor();
};

void CrossMonitorTest::edgeSnapWithinEightPixelsAndReleaseBeyond()
{
    const QList<QRect> screens{QRect(-1920, 318, 1920, 1080), QRect(0, 0, 3072, 1728)};
    const QRect desktopBounds(-1920, 0, 4992, 1728);

    // Distances <= 8 logical px snap onto monitor seam x = 0; 9 px releases across it.
    QCOMPARE(snapPointToMonitorEdges(QPoint(0, 500), screens), QPoint(0, 500));
    QCOMPARE(snapPointToMonitorEdges(QPoint(-4, 500), screens), QPoint(0, 500));
    QCOMPARE(snapPointToMonitorEdges(QPoint(-8, 500), screens), QPoint(0, 500));
    QCOMPARE(snapPointToMonitorEdges(QPoint(8, 500), screens), QPoint(0, 500));
    QCOMPARE(snapPointToMonitorEdges(QPoint(9, 500), screens), QPoint(9, 500));
    QCOMPARE(snapPointToMonitorEdges(QPoint(-9, 500), screens), QPoint(-9, 500));

    // Resize delta snapping along right seam (x = 0) and release at +9 px.
    const QRect start(-300, 400, 200, 100);
    QCOMPARE(snapResizeDeltaToMonitorEdges(start, Qt::RightEdge, QPoint(96, 0), screens), QPoint(100, 0));
    QCOMPARE(snapResizeDeltaToMonitorEdges(start, Qt::RightEdge, QPoint(108, 0), screens), QPoint(100, 0));
    QCOMPARE(snapResizeDeltaToMonitorEdges(start, Qt::RightEdge, QPoint(109, 0), screens), QPoint(109, 0));

    // Move rectangle snapping against seam x = 0 and release beyond 8 px.
    const QRect nearSeam(-206, 400, 200, 100); // right = -6 (distance 6 from 0)
    QCOMPARE(snapMovedRectToMonitorEdges(nearSeam, screens, desktopBounds), QRect(-200, 400, 200, 100));
    const QRect pastSeam(-191, 400, 200, 100); // right = +9
    QCOMPARE(snapMovedRectToMonitorEdges(pastSeam, screens, desktopBounds), QRect(-191, 400, 200, 100));

    // Arbitrary / negative desktop origins work for draw, resize, and arrow stepping.
    QCOMPARE(drawnSelection(QPoint(-400, 400), QPoint(200, 700), {}, desktopBounds), QRect(-400, 400, 600, 300));
    QCOMPARE(drawnSelection(QPoint(-100, 500), QPoint(50, 550), {true, true}, desktopBounds),
             QRect(-250, 350, 300, 300));
    QCOMPARE(resizedSelection(QRect(-200, 400, 400, 200), Qt::RightEdge, QPoint(50, 0), {}, desktopBounds),
             QRect(-200, 400, 450, 200));
    QCOMPARE(steppedSelection(QRect(-200, 400, 400, 200), QPoint(-10, 0), {}, 4, desktopBounds),
             QRect(-210, 400, 400, 200));

    SelectionModel model;
    const int id = model.add(QRect(-200, 400, 400, 200));
    QCOMPARE(model.topmostAt(QPoint(50, 500)), id);
    QCOMPARE(model.topmostAt(QStringLiteral("DP-1"), QPoint(50, 500)), 0);
}

void CrossMonitorTest::singleMonitorFastPathIsByteIdentical()
{
    const QList<ScreenInfo> layout = dualLayout();
    const QRect globalInsideDp(2000, 120, 400, 240);

    TargetStore store;
    const Area &added = store.addAreaForGlobalRect(layout, globalInsideDp);
    QVERIFY(!added.isCrossMonitor());
    QCOMPARE(added.connector, QStringLiteral("DP-1"));
    QCOMPARE(added.rect, QRect(80, 120, 400, 240));

    const TriggerPlan plan = planTrigger(store, added.id, layout);
    QVERIFY(plan.errors.isEmpty());
    QVERIFY(plan.partialNotifications.isEmpty());
    QCOMPARE(plan.connectors, QStringList{QStringLiteral("DP-1")});
    QCOMPARE(plan.outputs.size(), 1);
    QCOMPARE(plan.outputs.first().kind, OutputSpec::Kind::Area);

    QImage dpNative(3840, 2160, QImage::Format_RGBX8888);
    for (int y = 0; y < dpNative.height(); ++y) {
        for (int x = 0; x < dpNative.width(); ++x) {
            dpNative.setPixelColor(x, y, QColor((x * 3) % 256, (y * 5) % 256, ((x + y) * 7) % 256));
        }
    }
    const CaptureResult frame{dpNative, 1.25, {}};
    const QList<RenderedOutput> outputs = renderOutputs(plan, {{QStringLiteral("DP-1"), frame}});
    QCOMPARE(outputs.size(), 1);
    QVERIFY(outputs.first().error.isEmpty());

    QString directErr;
    const QImage direct = cropLogicalArea(dpNative, added.rect, 1.25, &directErr);
    QVERIFY(directErr.isEmpty());

    const QImage &rendered = outputs.first().image;
    QCOMPARE(rendered.size(), direct.size());
    QCOMPARE(rendered.format(), direct.format());
    QCOMPARE(rendered.hasAlphaChannel(), direct.hasAlphaChannel());
    QCOMPARE(rendered, direct);
    for (int y = 0; y < direct.height(); ++y) {
        QCOMPARE(std::memcmp(rendered.constScanLine(y), direct.constScanLine(y), size_t(direct.bytesPerLine())), 0);
    }
}

void CrossMonitorTest::crossMonitorMixedDpiCompositionAndJpegGap()
{
    const QList<ScreenInfo> layout = dualLayout();
    const QRect span(1520, 200, 800, 600); // spans HDMI-A-1 (y >= 318) and DP-1 (x >= 1920)

    TargetStore store;
    const Area &cross = store.addAreaForGlobalRect(layout, span);
    QVERIFY(cross.isCrossMonitor());
    QCOMPARE(cross.rect, span);
    QCOMPARE(cross.involvedConnectors(), (QStringList{QStringLiteral("HDMI-A-1"), QStringLiteral("DP-1")}));

    const TriggerPlan plan = planTrigger(store, cross.id, layout);
    QVERIFY(plan.errors.isEmpty());
    QVERIFY(plan.warnings.isEmpty());
    QVERIFY(plan.partialNotifications.isEmpty());
    QCOMPARE(plan.connectors, (QStringList{QStringLiteral("HDMI-A-1"), QStringLiteral("DP-1")}));
    QCOMPARE(plan.outputs.size(), 1);
    QCOMPARE(plan.outputs.first().kind, OutputSpec::Kind::CrossMonitorArea);

    QHash<QString, CaptureResult> frames;
    frames.insert(QStringLiteral("HDMI-A-1"), dualFrame(QStringLiteral("HDMI-A-1")));
    frames.insert(QStringLiteral("DP-1"), dualFrame(QStringLiteral("DP-1")));

    const QList<RenderedOutput> outputs = renderOutputs(plan, frames);
    QCOMPARE(outputs.size(), 1);
    QVERIFY2(outputs.first().error.isEmpty(), qPrintable(outputs.first().error));

    const QImage &composed = outputs.first().image;
    QCOMPARE(composed.size(), QSize(1000, 750)); // 800x600 at max scale 1.25
    QCOMPARE(composed.format(), QImage::Format_ARGB32_Premultiplied);
    QVERIFY(composed.hasAlphaChannel());

    // Dead top-left corner above HDMI-A-1 (y < (318 - 200) * 1.25 = 148) is transparent.
    QCOMPARE(qAlpha(composed.pixel(10, 10)), 0);
    QCOMPARE(qAlpha(composed.pixel(499, 147)), 0);
    // HDMI-A-1 half (x = 0..499, y >= 148) is blue.
    QCOMPARE(composed.pixelColor(250, 500), QColor(20, 40, 220));
    QCOMPARE(composed.pixelColor(499, 500), QColor(20, 40, 220));
    // DP-1 half (x = 500..999, y = 0..749) is red, seamless across x = 499 / 500.
    QCOMPARE(composed.pixelColor(500, 300), QColor(210, 20, 20));
    QCOMPARE(composed.pixelColor(750, 300), QColor(210, 20, 20));

    // Non-alpha JPEG export flattens transparent gaps onto #000000 black.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ExportOptions jpegOpts;
    jpegOpts.format = ImageFormat::Jpeg;
    jpegOpts.quality = 95;
    QString jpegPath;
    QString err;
    QVERIFY2(writeUniqueImageFile(dir.path(), QStringLiteral("cross.png"), composed, jpegOpts, &jpegPath, &err),
             qPrintable(err));
    const QImage jpegDecoded = QImageReader(jpegPath).read();
    QVERIFY(!jpegDecoded.isNull());
    QCOMPARE(jpegDecoded.pixelColor(10, 10), QColor(0, 0, 0));
    QVERIFY(jpegDecoded.pixelColor(250, 500).blue() > 180);
    QVERIFY(jpegDecoded.pixelColor(750, 300).red() > 180);
}

void CrossMonitorTest::partialCaptureWhenOneMonitorDisconnected()
{
    const QList<ScreenInfo> fullLayout = dualLayout();
    const QRect span(1520, 200, 800, 600);

    TargetStore store;
    const QString areaId = store.addAreaForGlobalRect(fullLayout, span).id;

    // Disconnect HDMI-A-1, leaving only DP-1 connected.
    const QList<ScreenInfo> dpOnly{fullLayout.at(1)};
    const TriggerPlan plan = planTrigger(store, areaId, dpOnly);
    QVERIFY(plan.errors.isEmpty());
    QCOMPARE(plan.partialNotifications.size(), 1);
    QVERIFY(plan.partialNotifications.first().contains(QStringLiteral("HDMI-A-1")));
    QVERIFY(plan.partialNotifications.first().contains(QStringLiteral("DP-1")));
    QCOMPARE(plan.connectors, QStringList{QStringLiteral("DP-1")});
    QCOMPARE(plan.outputs.size(), 1);
    QCOMPARE(plan.outputs.first().kind, OutputSpec::Kind::Area);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const TriggerResult result = TriggerEngine::execute(
        plan, [](const QString &conn, bool) { return dualFrame(conn); }, dir.path(),
        TargetStore::defaultFilenameTemplate(), QDateTime(QDate(2026, 11, 24), QTime(14, 30, 0)));
    QVERIFY(result.errors.isEmpty());
    QCOMPARE(result.partialNotifications.size(), 1);
    QCOMPARE(result.writtenPaths.size(), 1);

    const QImage saved(result.writtenPaths.first());
    QCOMPARE(saved.size(), QSize(500, 750)); // visible part on DP-1 (400x600 @ 1.25)
    QCOMPARE(saved.pixelColor(250, 300), QColor(210, 20, 20));
}

void CrossMonitorTest::threeMonitorPartialCaptureComposesRemainingTwo()
{
    const QList<ScreenInfo> tripleLayout{
        {QStringLiteral("HDMI-A-1"), QRect(0, 318, 1920, 1080), {}, {}, {}},
        {QStringLiteral("DP-1"), QRect(1920, 0, 3072, 1728), {}, {}, {}},
        {QStringLiteral("DP-2"), QRect(4992, 0, 1280, 720), {}, {}, {}},
    };
    const QRect span(1820, 100, 3272, 400); // covers HDMI-A-1, DP-1, DP-2

    TargetStore store;
    const QString areaId = store.addAreaForGlobalRect(tripleLayout, span).id;

    // Disconnect DP-2; remaining visible intersection still spans HDMI-A-1 and DP-1.
    const QList<ScreenInfo> twoConnected{tripleLayout.at(0), tripleLayout.at(1)};
    const TriggerPlan plan = planTrigger(store, areaId, twoConnected);
    QVERIFY(plan.errors.isEmpty());
    QCOMPARE(plan.partialNotifications.size(), 1);
    QVERIFY(plan.partialNotifications.first().contains(QStringLiteral("DP-2")));
    QCOMPARE(plan.connectors, (QStringList{QStringLiteral("HDMI-A-1"), QStringLiteral("DP-1")}));
    QCOMPARE(plan.outputs.size(), 1);
    QCOMPARE(plan.outputs.first().kind, OutputSpec::Kind::CrossMonitorArea);
    QCOMPARE(plan.outputs.first().rect, QRect(1820, 100, 3172, 400));

    QHash<QString, CaptureResult> frames;
    frames.insert(QStringLiteral("HDMI-A-1"), dualFrame(QStringLiteral("HDMI-A-1")));
    frames.insert(QStringLiteral("DP-1"), dualFrame(QStringLiteral("DP-1")));
    const QList<RenderedOutput> outputs = renderOutputs(plan, frames);
    QCOMPARE(outputs.size(), 1);
    QVERIFY2(outputs.first().error.isEmpty(), qPrintable(outputs.first().error));
    QCOMPARE(outputs.first().image.size(), QSize(3965, 500)); // 3172x400 @ 1.25
}

void CrossMonitorTest::zeroConnectedMonitorsSavesNothingWithPassiveNotice()
{
    TargetStore store;
    const QString areaId = store.addAreaForGlobalRect(dualLayout(), QRect(1520, 200, 800, 600)).id;

    const TriggerPlan plan = planTrigger(store, areaId, {});
    QVERIFY(plan.outputs.isEmpty());
    QVERIFY(plan.errors.isEmpty());
    QCOMPARE(plan.partialNotifications.size(), 1);
    QVERIFY(plan.partialNotifications.first().contains(QStringLiteral("HDMI-A-1")));
    QVERIFY(plan.partialNotifications.first().contains(QStringLiteral("DP-1")));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const TriggerResult result = TriggerEngine::execute(
        plan, [](const QString &conn, bool) { return dualFrame(conn); }, dir.path(),
        TargetStore::defaultFilenameTemplate(), QDateTime(QDate(2026, 11, 24), QTime(14, 30, 1)));
    QVERIFY(result.writtenPaths.isEmpty());
    QVERIFY(result.errors.isEmpty());
    QCOMPARE(result.partialNotifications.size(), 1);
}

void CrossMonitorTest::configRoundTripSingleAndCrossMonitor()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("glimplyrc"));

    const QList<ScreenInfo> layout{
        {QStringLiteral("DP-1"), QRect(-1920, 0, 1920, 1080), QStringLiteral("Acme, Ltd"), QStringLiteral("UHD"),
         QStringLiteral("111")},
        {QStringLiteral("HDMI-A-1"), QRect(0, 0, 1920, 1080), QStringLiteral("Beta, Co"), QStringLiteral("FHD"),
         QStringLiteral("222")},
    };

    TargetStore store;
    const QString singleId = store.addArea(layout.at(0), QRect(10, 20, 300, 200)).id;
    Area &cross = store.addCrossMonitorArea(layout, QRect(-300, 100, 600, 400));
    cross.name = QStringLiteral("SpanArea");
    cross.includeCursor = true;
    cross.delaySeconds = 5;
    cross.exportOverride = {true, {ImageFormat::WebP, 88, -1, false}};
    const QString crossId = cross.id;

    {
        KConfig config(path, KConfig::SimpleConfig);
        store.save(config);
    }

    {
        KConfig raw(path, KConfig::SimpleConfig);
        const KConfigGroup singleGroup = raw.group(QStringLiteral("Area ") + singleId);
        QVERIFY(!singleGroup.hasKey("CrossMonitor"));
        QVERIFY(!singleGroup.hasKey("MonitorCount"));
        QCOMPARE(singleGroup.readEntry("Connector", QString()), QStringLiteral("DP-1"));

        const KConfigGroup crossGroup = raw.group(QStringLiteral("Area ") + crossId);
        QVERIFY(crossGroup.readEntry("CrossMonitor", false));
        QCOMPARE(crossGroup.readEntry("MonitorCount", 0), 2);
    }

    TargetStore reloaded;
    {
        KConfig config(path, KConfig::SimpleConfig);
        reloaded.load(config);
    }
    QCOMPARE(reloaded.areas.size(), 2);

    const Area *loadedSingle = reloaded.area(singleId);
    QVERIFY(loadedSingle != nullptr);
    QVERIFY(!loadedSingle->isCrossMonitor());
    QCOMPARE(loadedSingle->connector, QStringLiteral("DP-1"));
    QCOMPARE(loadedSingle->monitorIdentity, QStringLiteral("Acme, Ltd UHD 111"));
    QCOMPARE(loadedSingle->rect, QRect(10, 20, 300, 200));

    const Area *loadedCross = reloaded.area(crossId);
    QVERIFY(loadedCross != nullptr);
    QVERIFY(loadedCross->isCrossMonitor());
    QCOMPARE(loadedCross->name, QStringLiteral("SpanArea"));
    QCOMPARE(loadedCross->rect, QRect(-300, 100, 600, 400));
    QVERIFY(loadedCross->includeCursor);
    QCOMPARE(loadedCross->delaySeconds, 5);
    QVERIFY(loadedCross->exportOverride.enabled);
    QCOMPARE(loadedCross->exportOverride.options.format, ImageFormat::WebP);
    QCOMPARE(loadedCross->exportOverride.options.quality, 88);
    QCOMPARE(loadedCross->monitors.size(), 2);
    QCOMPARE(loadedCross->monitors.at(0), (AreaMonitorBinding{QStringLiteral("DP-1"), QStringLiteral("Acme, Ltd UHD 111")}));
    QCOMPARE(loadedCross->monitors.at(1),
             (AreaMonitorBinding{QStringLiteral("HDMI-A-1"), QStringLiteral("Beta, Co FHD 222")}));
}

QTEST_GUILESS_MAIN(CrossMonitorTest)
#include "crossmonitortest.moc"
