#include "imageoutput.h"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QColor>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QThread>
#include <QtTest>

#include <array>
#include <set>

using namespace glimply;

class ImageOutputTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void cropsAtMonitorScale();
    void rejectsAreasOutsideFrame();
    void writesPngNamesWithoutOverwriting();
    void concurrentWritesChooseDistinctNames();
};

void ImageOutputTest::cropsAtMonitorScale()
{
    const QImage frame(1600, 1200, QImage::Format_RGB32);

    const QImage scaledCrop = cropLogicalArea(frame, QRect(0, 0, 400, 300), 1.25);
    QCOMPARE(scaledCrop.size(), QSize(500, 375));

    const QImage unscaledCrop = cropLogicalArea(frame, QRect(0, 0, 400, 300), 1.0);
    QCOMPARE(unscaledCrop.size(), QSize(400, 300));

    const QImage offsetCrop = cropLogicalArea(frame, QRect(10, 20, 400, 300), 1.25);
    QCOMPARE(offsetCrop.size(), QSize(500, 375));
}

void ImageOutputTest::rejectsAreasOutsideFrame()
{
    const QImage frame(400, 300, QImage::Format_RGB32);
    QString error;

    QVERIFY(cropLogicalArea(frame, QRect(300, 0, 200, 100), 1.0, &error).isNull());
    QVERIFY(!error.isEmpty());
}

void ImageOutputTest::writesPngNamesWithoutOverwriting()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    QImage frame(400, 300, QImage::Format_RGB32);
    frame.fill(QColor(12, 34, 56));
    const QImage firstCrop = cropLogicalArea(frame, QRect(10, 20, 32, 24), 1.0);
    const QImage secondCrop = cropLogicalArea(frame, QRect(10, 20, 32, 24), 1.0);
    QVERIFY(!firstCrop.isNull());
    QVERIFY(!secondCrop.isNull());
    const QDateTime timestamp(QDate(2026, 11, 22), QTime(14, 5, 33), QTimeZone::UTC);
    const OutputWriter writer(temporaryDirectory.path());
    QString firstPath;
    QString secondPath;
    QString error;

    QVERIFY2(writer.writeImage(firstCrop, QStringLiteral("DP 1/2"), timestamp, &firstPath, &error),
             qPrintable(error));
    QVERIFY2(writer.writeImage(secondCrop, QStringLiteral("DP 1/2"), timestamp, &secondPath, &error),
             qPrintable(error));

    QCOMPARE(QFileInfo(firstPath).fileName(), QStringLiteral("2026-11-22_14-05-33_DP-12.png"));
    QCOMPARE(QFileInfo(secondPath).fileName(), QStringLiteral("2026-11-22_14-05-33_DP-12-2.png"));
    QVERIFY(firstPath != secondPath);

    QFile firstFile(firstPath);
    QFile secondFile(secondPath);
    QVERIFY(firstFile.open(QIODevice::ReadOnly));
    QVERIFY(secondFile.open(QIODevice::ReadOnly));
    QCOMPARE(firstFile.readAll(), secondFile.readAll());

    QImageReader reader(firstPath);
    QCOMPARE(reader.format(), QByteArray("png"));
    QCOMPARE(reader.read().size(), firstCrop.size());
}

void ImageOutputTest::concurrentWritesChooseDistinctNames()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    constexpr std::size_t writerCount = 8;
    const QDateTime timestamp(QDate(2026, 11, 22), QTime(14, 5, 33), QTimeZone::UTC);
    const OutputWriter writer(temporaryDirectory.path());
    QImage image(64, 48, QImage::Format_RGB32);
    image.fill(QColor(90, 80, 70));

    QSemaphore start;
    std::array<bool, writerCount> succeeded{};
    std::array<QString, writerCount> paths{};
    std::array<QString, writerCount> errors{};
    std::array<QThread *, writerCount> threads{};
    for (std::size_t index = 0; index < writerCount; ++index) {
        threads[index] = QThread::create([&, index] {
            start.acquire();
            succeeded[index] = writer.writeImage(
                image, QStringLiteral("DP-1-1"), timestamp, &paths[index], &errors[index]);
        });
        threads[index]->start();
    }

    start.release(static_cast<int>(writerCount));
    for (QThread *thread : threads) {
        QVERIFY(thread->wait());
        delete thread;
    }

    std::set<QString> distinctPaths;
    for (std::size_t index = 0; index < writerCount; ++index) {
        QVERIFY2(succeeded[index], qPrintable(errors[index]));
        QVERIFY(QFileInfo::exists(paths[index]));
        distinctPaths.insert(paths[index]);
    }
    QCOMPARE(distinctPaths.size(), writerCount);
}

QTEST_GUILESS_MAIN(ImageOutputTest)
#include "imageoutputtest.moc"
