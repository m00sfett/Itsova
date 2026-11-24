#include "exportdialog.h"
#include "imageoutput.h"
#include "targets.h"
#include "triggerengine.h"
#include "triggerplan.h"

#include <KConfig>

#include <QBuffer>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

using namespace glimply;

namespace
{

QImage makeGradientImage(const QSize &size)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            img.setPixelColor(x, y, QColor((x * 7 + y * 13) % 256, (x * y + x) % 256, (x + y * 5) % 256));
        }
    }
    return img;
}

} // namespace

class ExportTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void extensionAndFilenameAlignment();
    void jpegFlattensTransparentAlphaToBlack();
    void pngCompressionWebPAndOptionalPlugins();
    void metadataStrippedOnExport();
    void targetOverridePrecedenceAndTriggerEngine();
    void exportDialogEstimatesAndUpdatesExtension();
};

void ExportTest::extensionAndFilenameAlignment()
{
    const QDateTime ts(QDate(2026, 11, 24), QTime(14, 15, 30), QTimeZone::UTC);

    QCOMPARE(replaceImageExtension(QStringLiteral("shot.png"), ImageFormat::Jpeg), QStringLiteral("shot.jpg"));
    QCOMPARE(replaceImageExtension(QStringLiteral("shot.v2.webp"), ImageFormat::Avif), QStringLiteral("shot.v2.avif"));
    QCOMPARE(replaceImageExtension(QStringLiteral("plain"), ImageFormat::WebP), QStringLiteral("plain.webp"));

    QCOMPARE(filenameFor(QStringLiteral("{date}_{time}_{area}.png"), QStringLiteral("DP-1"), ts, ImageFormat::Jpeg),
             QStringLiteral("2026-11-24_14-15-30_DP-1.jpg"));
    QCOMPARE(filenameFor(QStringLiteral("{date}_{area}.{ext}"), QStringLiteral("Left"), ts, ImageFormat::WebP),
             QStringLiteral("2026-11-24_Left.webp"));
}

void ExportTest::jpegFlattensTransparentAlphaToBlack()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    QImage img(80, 60, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    for (int y = 0; y < 60; ++y) {
        for (int x = 40; x < 80; ++x) {
            img.setPixelColor(x, y, QColor(220, 120, 40, 255));
        }
    }

    ExportOptions jpegOpts;
    jpegOpts.format = ImageFormat::Jpeg;
    jpegOpts.quality = 95;
    QString jpegPath;
    QString error;
    QVERIFY2(writeUniqueImageFile(tempDir.path(), QStringLiteral("gap.png"), img, jpegOpts, &jpegPath, &error),
             qPrintable(error));
    QCOMPARE(QFileInfo(jpegPath).suffix(), QStringLiteral("jpg"));

    QImageReader jpegReader(jpegPath);
    QCOMPARE(jpegReader.format(), QByteArray("jpeg"));
    const QImage jpegDecoded = jpegReader.read();
    QVERIFY(!jpegDecoded.isNull());
    QVERIFY(!jpegDecoded.hasAlphaChannel());
    QCOMPARE(jpegDecoded.pixelColor(10, 30), QColor(0, 0, 0));
    QVERIFY(jpegDecoded.pixelColor(60, 30).red() > 180);

    ExportOptions pngOpts;
    pngOpts.format = ImageFormat::Png;
    QString pngPath;
    QVERIFY2(writeUniqueImageFile(tempDir.path(), QStringLiteral("gap.png"), img, pngOpts, &pngPath, &error),
             qPrintable(error));
    QCOMPARE(QImage(pngPath).pixelColor(10, 30).alpha(), 0);
}

void ExportTest::pngCompressionWebPAndOptionalPlugins()
{
    const QImage img = makeGradientImage(QSize(160, 120));

    ExportOptions pngFast{ImageFormat::Png, 90, 0, false};
    ExportOptions pngMax{ImageFormat::Png, 90, 9, false};
    const qint64 pngFastBytes = estimateEncodedSize(img, pngFast);
    const qint64 pngMaxBytes = estimateEncodedSize(img, pngMax);
    QVERIFY(pngFastBytes > 0);
    QVERIFY(pngMaxBytes > 0);
    QVERIFY2(pngMaxBytes < pngFastBytes,
             qPrintable(QStringLiteral("max=%1 fast=%2").arg(pngMaxBytes).arg(pngFastBytes)));

    QVERIFY(isImageFormatSupported(ImageFormat::WebP));
    const qint64 webpLow = estimateEncodedSize(img, ExportOptions{ImageFormat::WebP, 20, -1, false});
    const qint64 webpHigh = estimateEncodedSize(img, ExportOptions{ImageFormat::WebP, 95, -1, false});
    QVERIFY(webpLow > 0);
    QVERIFY(webpHigh > webpLow);

    QBuffer losslessBuf;
    QVERIFY(losslessBuf.open(QIODevice::ReadWrite));
    QString error;
    QVERIFY2(encodeImageToDevice(&losslessBuf, img, ExportOptions{ImageFormat::WebP, 90, -1, true}, &error),
             qPrintable(error));
    losslessBuf.seek(0);
    QImageReader webpReader(&losslessBuf, "webp");
    const QImage webpDecoded = webpReader.read();
    QCOMPARE(webpDecoded.size(), img.size());
    QCOMPARE(webpDecoded.pixelColor(17, 29), img.pixelColor(17, 29));

    for (const ImageFormat opt : {ImageFormat::Avif, ImageFormat::Jxl}) {
        if (!isImageFormatSupported(opt)) {
            continue;
        }
        QBuffer optBuf;
        QVERIFY(optBuf.open(QIODevice::ReadWrite));
        QVERIFY2(encodeImageToDevice(&optBuf, img, ExportOptions{opt, 85, -1, false}, &error), qPrintable(error));
        QVERIFY(optBuf.size() > 0);
        optBuf.seek(0);
        QImageReader optReader(&optBuf, qtFormatName(opt));
        QCOMPARE(optReader.read().size(), img.size());
    }
}

void ExportTest::metadataStrippedOnExport()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    QImage img(64, 48, QImage::Format_RGB32);
    img.fill(Qt::darkGreen);
    img.setText(QStringLiteral("Comment"), QStringLiteral("secret-note"));

    const QString outPath = QDir(tempDir.path()).filePath(QStringLiteral("stripped.png"));
    QString error;
    QVERIFY2(writeImageToFile(outPath, img, ExportOptions{ImageFormat::Png, 90, -1, false}, &error), qPrintable(error));

    const QImage loaded(outPath);
    QVERIFY(!loaded.textKeys().contains(QStringLiteral("Comment")));
    QVERIFY(loaded.text(QStringLiteral("Comment")).isEmpty());
}

void ExportTest::targetOverridePrecedenceAndTriggerEngine()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const ScreenInfo screen{QStringLiteral("DP-1"), QRect(0, 0, 1920, 1080), QStringLiteral("Dell"),
                            QStringLiteral("U27"), QStringLiteral("ABC")};

    TargetStore store;
    store.captureDirectory = tempDir.path();
    store.exportOptions = ExportOptions{ImageFormat::WebP, 80, -1, false};

    QString a1Id;
    {
        Area &a1 = store.addArea(screen, QRect(10, 10, 40, 30));
        a1.name = QStringLiteral("area-inherit");
        a1Id = a1.id;
    }
    QString a2Id;
    {
        Area &a2 = store.addArea(screen, QRect(60, 10, 40, 30));
        a2.name = QStringLiteral("area-png");
        a2.exportOverride = {true, ExportOptions{ImageFormat::Png, 90, 9, false}};
        a2Id = a2.id;
    }

    QString groupId;
    {
        Group &group = store.addGroup(QStringLiteral("MixedGroup"));
        group.targetIds = {a1Id, a2Id};
        group.exportOverride = {true, ExportOptions{ImageFormat::Jpeg, 75, -1, false}};
        groupId = group.id;
    }

    store.setTargetExportOverride(screenTargetId(screen.connector), {true, ExportOptions{ImageFormat::Jpeg, 65, -1, false}});

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

    QCOMPARE(loaded.exportOptions, store.exportOptions);
    QCOMPARE(loaded.effectiveExportOptions().format, ImageFormat::WebP);
    QCOMPARE(loaded.effectiveExportOptions(screenTargetId(screen.connector)).format, ImageFormat::Jpeg);
    QCOMPARE(loaded.effectiveExportOptions(screenTargetId(screen.connector)).quality, 65);
    QCOMPARE(loaded.effectiveExportOptions(a1Id).format, ImageFormat::WebP);
    QCOMPARE(loaded.effectiveExportOptions(a1Id, groupId).format, ImageFormat::Jpeg);
    QCOMPARE(loaded.effectiveExportOptions(a1Id, groupId).quality, 75);
    QCOMPARE(loaded.effectiveExportOptions(a2Id, groupId).format, ImageFormat::Png);

    const TriggerPlan plan = planTrigger(loaded, groupId, {screen});
    const auto fakeCapturer = [](const QString &, bool) -> CaptureResult {
        CaptureResult res;
        res.scale = 1.0;
        res.image = makeGradientImage(QSize(200, 100));
        return res;
    };
    const QDateTime ts(QDate(2026, 11, 24), QTime(14, 20, 0), QTimeZone::UTC);
    const TriggerResult result =
        TriggerEngine::execute(plan, fakeCapturer, loaded.effectiveCaptureDirectory(),
                               loaded.effectiveFilenameTemplate(), ts);

    QCOMPARE(result.errors, QStringList{});
    QCOMPARE(result.writtenPaths.size(), 2);
    QCOMPARE(QFileInfo(result.writtenPaths[0]).suffix(), QStringLiteral("jpg"));
    QCOMPARE(QFileInfo(result.writtenPaths[1]).suffix(), QStringLiteral("png"));
    QCOMPARE(QImageReader(result.writtenPaths[0]).format(), QByteArray("jpeg"));
    QCOMPARE(QImageReader(result.writtenPaths[1]).format(), QByteArray("png"));
}

void ExportTest::exportDialogEstimatesAndUpdatesExtension()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QImage img = makeGradientImage(QSize(180, 120));
    const QString initialPath = QDir(tempDir.path()).filePath(QStringLiteral("capture.png"));
    ExportDialog dialog(img, initialPath, ExportOptions{ImageFormat::Png, 90, -1, false});

    QTRY_VERIFY_WITH_TIMEOUT(dialog.estimatedBytes() > 0, 5000);
    QVERIFY(dialog.estimatedSizeText().startsWith(QStringLiteral("≈")));

    const int jpegIdx = dialog.formatCombo()->findData(int(ImageFormat::Jpeg));
    QVERIFY(jpegIdx >= 0);
    dialog.formatCombo()->setCurrentIndex(jpegIdx);
    QCOMPARE(QFileInfo(dialog.filePath()).suffix(), QStringLiteral("jpg"));

    dialog.valueSpin()->setValue(95);
    QTRY_VERIFY_WITH_TIMEOUT(dialog.estimatedBytes() > 0, 5000);
    const qint64 highQualityBytes = dialog.estimatedBytes();

    dialog.valueSpin()->setValue(25);
    QTRY_VERIFY_WITH_TIMEOUT(dialog.estimatedBytes() > 0, 5000);
    const qint64 lowQualityBytes = dialog.estimatedBytes();

    QVERIFY2(lowQualityBytes < highQualityBytes,
             qPrintable(QStringLiteral("low=%1 high=%2").arg(lowQualityBytes).arg(highQualityBytes)));
}

QTEST_MAIN(ExportTest)
#include "exporttest.moc"
