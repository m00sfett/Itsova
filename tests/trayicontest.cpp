#include "trayicon.h"

#include <QGuiApplication>
#include <QImage>
#include <QTest>

using namespace itsova;

namespace
{

bool near(QColor a, QColor b)
{
    return std::abs(a.red() - b.red()) < 40 && std::abs(a.green() - b.green()) < 40 && std::abs(a.blue() - b.blue()) < 40;
}

const QColor red(0xB1, 0x12, 0x18);
const QColor yellow(0xFF, 0xD2, 0x1F);

} // namespace

class TrayIconTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // #123: hidden modes remove the item unless something runs; the widget standby applies only
    // to "Always".
    void presenceFollowsModeAndActivity()
    {
        // mode 0 = always
        QCOMPARE(trayPresence(0, false, false, false), TrayPresence::Shown);
        QCOMPARE(trayPresence(0, false, false, true), TrayPresence::Standby);
        QCOMPARE(trayPresence(0, true, false, true), TrayPresence::Standby);
        // mode 1 = while something runs
        QCOMPARE(trayPresence(1, false, false, false), TrayPresence::Hidden);
        QCOMPARE(trayPresence(1, true, false, false), TrayPresence::Shown);
        QCOMPARE(trayPresence(1, false, true, false), TrayPresence::Shown);
        QCOMPARE(trayPresence(1, false, true, true), TrayPresence::Shown);
        QCOMPARE(trayPresence(1, false, false, true), TrayPresence::Hidden);
        // mode 2 = while recording
        QCOMPARE(trayPresence(2, false, true, false), TrayPresence::Hidden);
        QCOMPARE(trayPresence(2, true, false, false), TrayPresence::Shown);
        QCOMPARE(trayPresence(2, true, true, true), TrayPresence::Shown);
    }

    // #99: while recording, the tile is inverted like the capture flash with a red dot in the
    // middle; paused, two red bars.
    void recordingIsAnInvertedTileWithARedDot()
    {
        const QImage image = trayIcon(TrayIconState::Recording).pixmap(22, 22).toImage();
        QVERIFY(near(image.pixelColor(11, 11), red));  // the dot
        QVERIFY(near(image.pixelColor(11, 2), yellow)); // the tile between the corners
        QVERIFY(near(image.pixelColor(6, 4), red));    // a viewfinder corner
    }

    void pausedShowsTwoRedBars()
    {
        const QImage image = trayIcon(TrayIconState::RecordingPaused).pixmap(22, 22).toImage();
        QVERIFY(near(image.pixelColor(8, 11), red));
        QVERIFY(near(image.pixelColor(13, 11), red));
        QVERIFY(near(image.pixelColor(11, 11), yellow)); // the gap between the bars
        QVERIFY(near(image.pixelColor(11, 2), yellow));
    }

    void everyTraySizeHasItsOwnPixmap()
    {
        const QIcon icon = trayIcon(TrayIconState::Recording);
        // 16–48 px and the same sizes at 125 % (20, 28, 30, 40, 60).
        for (const int size : {16, 20, 22, 24, 28, 30, 32, 40, 48, 60}) {
            QVERIFY2(icon.availableSizes().contains(QSize(size, size)), qPrintable(QString::number(size)));
        }
    }
};

QTEST_MAIN(TrayIconTest)
#include "trayicontest.moc"
