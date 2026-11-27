#include "kwinpinscript.h"
#include "pinstore.h"
#include "pinwindow.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QTest>

using namespace itsova;

class PinTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void zoomSteps();
    void opacitySteps();
    void storeRoundTripAndRemoval();
    void loadSkipsMissingImagesAndBadIds();
    void positionFollowsOutputOrFallsBack();
    void outputAtPicksContainingOrNearest();
    void scriptEscapesCaption();
    void windowSizeFollowsZoomAndScale();
    void rtlReadoutMirrorsWithoutMirroringImage();
};

void PinTest::zoomSteps()
{
    QCOMPARE(nextPinZoom(1.0, 1), 1.25);
    QCOMPARE(nextPinZoom(1.0, -1), 0.75);
    QCOMPARE(nextPinZoom(4.0, 1), 4.0);
    QCOMPARE(nextPinZoom(0.25, -1), 0.25);
    QCOMPARE(nextPinZoom(1.1, 1), 1.25);
    QCOMPARE(nextPinZoom(1.1, -1), 1.0);
    QCOMPARE(nextPinZoom(9.0, 0), 4.0);
}

void PinTest::opacitySteps()
{
    QCOMPARE(clampPinOpacity(0.05), 0.2);
    QCOMPARE(clampPinOpacity(2.0), 1.0);
    QCOMPARE(nextPinOpacity(1.0, -1), 0.9);
    QCOMPARE(nextPinOpacity(0.2, -1), 0.2);
    QCOMPARE(nextPinOpacity(0.9, 1), 1.0);
}

void PinTest::storeRoundTripAndRemoval()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pinDir = dir.filePath(QStringLiteral("pins"));
    PinStore store(pinDir);
    QImage image(40, 20, QImage::Format_ARGB32);
    image.fill(Qt::red);
    const std::optional<PinRecord> added = store.add(image, QStringLiteral("DP-1"), QPoint(10, 20));
    QVERIFY(added);
    QVERIFY(QFileInfo::exists(store.imagePath(added->id)));
    QCOMPARE(QFileInfo(store.imagePath(added->id)).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther),
             QFileDevice::Permissions());
    PinRecord changed = *added;
    changed.zoom = 2.0;
    changed.opacity = 0.55;
    QVERIFY(store.update(changed));
    store.setHidden(true);
    QVERIFY(store.save());

    PinStore loaded(pinDir);
    QVERIFY(loaded.load());
    QCOMPARE(loaded.pins().size(), 1);
    QVERIFY(loaded.hidden());
    const PinRecord &record = loaded.pins().first();
    QCOMPARE(record.id, added->id);
    QCOMPARE(record.connector, QStringLiteral("DP-1"));
    QCOMPARE(record.offset, QPoint(10, 20));
    QCOMPARE(record.zoom, 2.0);
    QCOMPARE(record.opacity, 0.6);
    QCOMPARE(QImage(loaded.imagePath(record.id)).size(), QSize(40, 20));

    QVERIFY(loaded.remove(record.id));
    QVERIFY(!QFileInfo::exists(loaded.imagePath(record.id)));
    QVERIFY(!loaded.remove(record.id));
    QVERIFY(!loaded.update(record));
}

void PinTest::loadSkipsMissingImagesAndBadIds()
{
    QTemporaryDir dir;
    const QString pinDir = dir.filePath(QStringLiteral("pins"));
    PinStore store(pinDir);
    QImage image(4, 4, QImage::Format_ARGB32);
    image.fill(Qt::blue);
    const std::optional<PinRecord> keep = store.add(image, QStringLiteral("DP-1"), {});
    const std::optional<PinRecord> gone = store.add(image, QStringLiteral("DP-1"), {});
    QVERIFY(keep && gone);
    QVERIFY(store.save());
    QFile::remove(store.imagePath(gone->id));
    // A hand-edited id must never turn into a path outside the pin folder.
    QFile file(pinDir + QStringLiteral("/pins.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QByteArray json = file.readAll();
    file.close();
    json.replace("\"pins\": [", "\"pins\": [{\"id\": \"../../evil\"},");
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(json);
    file.close();

    PinStore loaded(pinDir);
    QVERIFY(loaded.load());
    QCOMPARE(loaded.pins().size(), 1);
    QCOMPARE(loaded.pins().first().id, keep->id);
}

void PinTest::positionFollowsOutputOrFallsBack()
{
    const QList<PinOutput> outputs{
        {QStringLiteral("HDMI-A-1"), QRect(0, 318, 1920, 1080)},
        {QStringLiteral("DP-1"), QRect(1920, 0, 3072, 1728)},
    };
    PinRecord record;
    record.connector = QStringLiteral("DP-1");
    record.offset = QPoint(180, 100);
    QCOMPARE(resolvePinPosition(record, outputs, QSize(200, 100)), QPoint(2100, 100));

    // Monitor gone: same offset on the primary (first) output.
    record.connector = QStringLiteral("DP-2");
    QCOMPARE(resolvePinPosition(record, outputs, QSize(200, 100)), QPoint(180, 418));

    // Offset outside the fallback output is clamped so the pin stays grabbable.
    record.offset = QPoint(3000, 1600);
    const QPoint clamped = resolvePinPosition(record, outputs, QSize(200, 100));
    QVERIFY(outputs.first().geometry.contains(clamped + QPoint(10, 10)));
}

void PinTest::outputAtPicksContainingOrNearest()
{
    const QList<PinOutput> outputs{
        {QStringLiteral("HDMI-A-1"), QRect(0, 318, 1920, 1080)},
        {QStringLiteral("DP-1"), QRect(1920, 0, 3072, 1728)},
    };
    auto hit = pinOutputAt(QPoint(2100, 100), outputs);
    QVERIFY(hit);
    QCOMPARE(hit->first, QStringLiteral("DP-1"));
    QCOMPARE(hit->second, QPoint(180, 100));
    hit = pinOutputAt(QPoint(100, 10), outputs); // above HDMI-A-1, in no output
    QVERIFY(hit);
    QCOMPARE(hit->first, QStringLiteral("HDMI-A-1"));
    QVERIFY(!pinOutputAt(QPoint(0, 0), {}));
}

void PinTest::scriptEscapesCaption()
{
    KWinPinScriptRequest request;
    request.pinId = QStringLiteral("abc");
    request.caption = QStringLiteral("itsova-pin-\"; evil(); \"");
    request.geometry = QRect(2100, 100, 200, 100);
    request.service = QStringLiteral("io.github.m00sfett.itsova");
    request.path = QStringLiteral("/Control");
    request.interface = QStringLiteral("io.github.m00sfett.itsova.Control");
    const QString js = kwinPinScriptSource(request);
    QVERIFY(js.contains(QStringLiteral("const caption = \"itsova-pin-\\\"; evil(); \\\"\";")));
    QVERIFY(js.contains(QStringLiteral("const target = {x: 2100, y: 100};")));
    QVERIFY(js.contains(QStringLiteral("w.keepAbove = true;")));
    QVERIFY(js.contains(QStringLiteral("\"PinMoved\"")));
    request.geometry = QRect();
    QVERIFY(kwinPinScriptSource(request).contains(QStringLiteral("const target = null;")));
    QCOMPARE(kwinPinPluginName(QStringLiteral("abc")), QStringLiteral("itsova-pin-abc"));
}

void PinTest::windowSizeFollowsZoomAndScale()
{
    QImage image(400, 200, QImage::Format_ARGB32);
    image.fill(Qt::green);
    PinWindow window(QStringLiteral("id"), image, 1.0, 1.0);
    QCOMPARE(window.kwinCaption(), QStringLiteral("itsova-pin-id"));
    QCOMPARE(window.logicalSizeFor(1.0), QSize(400, 200));
    QCOMPARE(window.logicalSizeFor(1.25), QSize(320, 160));
    QSignalSpy zoomSpy(&window, &PinWindow::zoomChanged);
    window.setZoom(2.0);
    QCOMPARE(zoomSpy.count(), 1);
    QCOMPARE(window.logicalSizeFor(1.0), QSize(800, 400));
    window.setZoom(2.0);
    QCOMPARE(zoomSpy.count(), 1);
    window.setOpacityLevel(0.04);
    QCOMPARE(window.opacity(), 0.2);
}

void PinTest::rtlReadoutMirrorsWithoutMirroringImage()
{
    QImage image(200, 100, QImage::Format_ARGB32);
    {
        QPainter p(&image);
        p.fillRect(0, 0, 100, 100, QColor(220, 20, 20));
        p.fillRect(100, 0, 100, 100, QColor(20, 20, 220));
    }
    PinWindow window(QStringLiteral("rtl"), image, 1.0, 1.0);
    window.setLayoutDirection(Qt::LeftToRight);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.setZoom(1.5);

    const QRect ltrBox = window.readoutRect();
    QVERIFY(!ltrBox.isEmpty());
    QVERIFY(ltrBox.center().x() > window.width() / 2);
    const QImage ltrGrab = window.grab().toImage();
    QVERIFY(ltrGrab.pixelColor(20, 20).red() > 180);
    QVERIFY(ltrGrab.pixelColor(window.width() - 20, 20).blue() > 180);

    window.setLayoutDirection(Qt::RightToLeft);
    QCoreApplication::processEvents();
    const QRect rtlBox = window.readoutRect();
    QVERIFY(!rtlBox.isEmpty());
    QVERIFY(rtlBox.center().x() < window.width() / 2);
    // Compare with the LTR box of the same window size: on Wayland the window may meet its output
    // (screenChanged) only now and take that output's scale (#76).
    window.setLayoutDirection(Qt::LeftToRight);
    const QRect ltrNow = window.readoutRect();
    window.setLayoutDirection(Qt::RightToLeft);
    QCOMPARE(rtlBox.bottom(), ltrNow.bottom());
    QCOMPARE(rtlBox.size(), ltrNow.size());
    // Captured image content must stay unmirrored in RTL.
    const QImage rtlGrab = window.grab().toImage();
    QVERIFY(rtlGrab.pixelColor(20, 20).red() > 180);
    QVERIFY(rtlGrab.pixelColor(window.width() - 20, 20).blue() > 180);

    window.close();
}

QTEST_MAIN(PinTest)
#include "pintest.moc"
