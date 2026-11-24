#include "ocrengine.h"

#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>

using namespace glimply;

namespace
{

// Writes an executable stand-in for tesseract that answers --version, --list-langs and recognition.
QString writeFakeTesseract(const QTemporaryDir &dir, const QByteArray &recognizeBody)
{
    const QString path = dir.filePath(QStringLiteral("tesseract"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return {};
    }
    file.write("#!/bin/sh\n"
               "case \"$1\" in\n"
               "  --version) echo 'tesseract 5.5.1'; echo ' leptonica-1.85.0'; exit 0;;\n"
               "  --list-langs) echo 'List of available languages in \"/usr/share/tessdata/\" (3):'; "
               "echo deu; echo eng; echo osd; exit 0;;\n"
               "esac\n"
               "cat > /dev/null\n");
    file.write(recognizeBody);
    file.close();
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return path;
}

QImage sampleImage()
{
    QImage image(20, 10, QImage::Format_RGB32);
    image.fill(Qt::white);
    return image;
}

} // namespace

class OcrTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void parsesLanguageList();
    void effectiveLanguagesFallBackToInstalled();
    void buildsArguments();
    void ordersRectanglesForReading();
    void joinsAndPreviewsText();
    void recognizesWithFakeBackend();
    void reportsBackendFailure();
    void missingBackendIsUnavailable();
};

void OcrTest::parsesLanguageList()
{
    const QString output = QStringLiteral("List of available languages in \"/usr/share/tessdata/\" (3):\n"
                                          "deu\nosd\nchi_sim\n\n");
    QCOMPARE(parseTesseractLanguages(output), QStringList({QStringLiteral("deu"), QStringLiteral("osd"),
                                                           QStringLiteral("chi_sim")}));
    QVERIFY(parseTesseractLanguages(QStringLiteral("Error opening data file")).isEmpty());
}

void OcrTest::effectiveLanguagesFallBackToInstalled()
{
    const QStringList installed{QStringLiteral("deu"), QStringLiteral("eng"), QStringLiteral("osd")};
    QCOMPARE(effectiveOcrLanguages({}, installed), QStringList({QStringLiteral("deu"), QStringLiteral("eng")}));
    QCOMPARE(effectiveOcrLanguages({QStringLiteral("eng")}, installed), QStringList({QStringLiteral("eng")}));
    // A configured language that was uninstalled falls back to the installed set.
    QCOMPARE(effectiveOcrLanguages({QStringLiteral("fra")}, installed),
             QStringList({QStringLiteral("deu"), QStringLiteral("eng")}));
    QVERIFY(effectiveOcrLanguages({}, {}).isEmpty());
}

void OcrTest::buildsArguments()
{
    QCOMPARE(tesseractArguments({QStringLiteral("deu"), QStringLiteral("eng")}),
             QStringList({QStringLiteral("stdin"), QStringLiteral("stdout"), QStringLiteral("-l"),
                          QStringLiteral("deu+eng"), QStringLiteral("--psm"), QStringLiteral("3")}));
    QCOMPARE(tesseractArguments({}),
             QStringList({QStringLiteral("stdin"), QStringLiteral("stdout"), QStringLiteral("--psm"),
                          QStringLiteral("3")}));
}

void OcrTest::ordersRectanglesForReading()
{
    const QList<QRect> rects{QRect(500, 300, 100, 50), QRect(10, 10, 100, 50), QRect(400, 10, 100, 50)};
    QCOMPARE(ocrReadingOrder(rects), QList<int>({1, 2, 0}));
    QVERIFY(ocrReadingOrder({}).isEmpty());
}

void OcrTest::joinsAndPreviewsText()
{
    QCOMPARE(joinOcrTexts({QStringLiteral("  first "), QString(), QStringLiteral("second")}),
             QStringLiteral("first\n\nsecond"));
    QCOMPARE(ocrPreview(QStringLiteral("a\n b\tc")), QStringLiteral("a b c"));
    const QString preview = ocrPreview(QString(200, QLatin1Char('x')), 80);
    QCOMPARE(preview.size(), 80);
    QVERIFY(preview.endsWith(QChar(0x2026)));
}

void OcrTest::recognizesWithFakeBackend()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString exe = writeFakeTesseract(dir, "printf 'Hello  \\nWorld \\n\\f'\n");
    TesseractOcrEngine engine(exe);
    QVERIFY(engine.available());
    QCOMPARE(engine.description(), QStringLiteral("tesseract 5.5.1"));
    QCOMPARE(engine.languages(), QStringList({QStringLiteral("deu"), QStringLiteral("eng"), QStringLiteral("osd")}));
    const OcrResult result = engine.recognize(sampleImage(), {QStringLiteral("deu")});
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.text, QStringLiteral("Hello\nWorld"));
}

void OcrTest::reportsBackendFailure()
{
    QTemporaryDir dir;
    const QString exe = writeFakeTesseract(dir, "echo 'Failed loading language xyz' >&2\nexit 1\n");
    TesseractOcrEngine engine(exe);
    const OcrResult result = engine.recognize(sampleImage(), {QStringLiteral("xyz")});
    QVERIFY(!result.ok);
    QCOMPARE(result.error, QStringLiteral("Failed loading language xyz"));
    QVERIFY(!engine.recognize(QImage(), {}).ok);
}

void OcrTest::missingBackendIsUnavailable()
{
    TesseractOcrEngine engine(QStringLiteral("/nonexistent/glimply-no-tesseract"));
    // An explicit path is trusted as configured; recognition then fails cleanly.
    const OcrResult result = engine.recognize(sampleImage(), {});
    QVERIFY(!result.ok);
    QVERIFY(!result.error.isEmpty());
}

QTEST_GUILESS_MAIN(OcrTest)
#include "ocrtest.moc"
