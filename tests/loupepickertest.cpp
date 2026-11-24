#include "annotations/annotationcanvas.h"
#include "annotations/annotationcontroller.h"
#include "annotations/annotationdocument.h"
#include "colorformat.h"
#include "pixelloupe.h"
#include "quickeditorwindow.h"
#include "targets.h"

#include <KConfig>

#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <memory>

using namespace glimply;

class LoupePickerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void colorFormatConversionsAndCycle();
    void fractionalDpiNativePixelSampling();
    void loupeSmartQuadrantPlacement();
    void storePersistsColorFormatAndLoupeToggle();
    void quickEditorColorPickerCopiesAndAppliesColor();
    void rtlColorPickerLoupeMirrorsSwatchRowOnly();
};

void LoupePickerTest::colorFormatConversionsAndCycle()
{
    QCOMPARE(formatColor(QColor(255, 0, 0), ColorFormat::Hex), QStringLiteral("#FF0000"));
    QCOMPARE(formatColor(QColor(255, 0, 0), ColorFormat::Rgb), QStringLiteral("rgb(255, 0, 0)"));
    QCOMPARE(formatColor(QColor(255, 0, 0), ColorFormat::Hsl), QStringLiteral("hsl(0, 100%, 50%)"));
    QCOMPARE(formatColor(QColor(255, 0, 0), ColorFormat::Hsv), QStringLiteral("hsv(0, 100%, 100%)"));

    QCOMPARE(formatColor(QColor(0, 255, 0), ColorFormat::Hsl), QStringLiteral("hsl(120, 100%, 50%)"));
    QCOMPARE(formatColor(QColor(0, 0, 255), ColorFormat::Hsv), QStringLiteral("hsv(240, 100%, 100%)"));

    // Achromatic gray/white/black must report hue 0, never -1.
    QCOMPARE(formatColor(QColor(255, 255, 255), ColorFormat::Hsl), QStringLiteral("hsl(0, 0%, 100%)"));
    QCOMPARE(formatColor(QColor(0, 0, 0), ColorFormat::Hsv), QStringLiteral("hsv(0, 0%, 0%)"));
    QCOMPARE(formatColor(QColor(128, 128, 128), ColorFormat::Hsl), QStringLiteral("hsl(0, 0%, 50%)"));
    QCOMPARE(formatColor(QColor(128, 128, 128), ColorFormat::Hsv), QStringLiteral("hsv(0, 0%, 50%)"));

    const QColor brandRed(0xB1, 0x12, 0x18);
    QCOMPARE(formatColor(brandRed, ColorFormat::Hex), QStringLiteral("#B11218"));
    QCOMPARE(formatColor(brandRed, ColorFormat::Rgb), QStringLiteral("rgb(177, 18, 24)"));
    QCOMPARE(formatColor(brandRed, ColorFormat::Hsl), QStringLiteral("hsl(358, 82%, 38%)"));
    QCOMPARE(formatColor(brandRed, ColorFormat::Hsv), QStringLiteral("hsv(358, 90%, 69%)"));

    ColorFormat fmt = ColorFormat::Hex;
    fmt = nextColorFormat(fmt);
    QCOMPARE(fmt, ColorFormat::Rgb);
    fmt = nextColorFormat(fmt);
    QCOMPARE(fmt, ColorFormat::Hsl);
    fmt = nextColorFormat(fmt);
    QCOMPARE(fmt, ColorFormat::Hsv);
    fmt = nextColorFormat(fmt);
    QCOMPARE(fmt, ColorFormat::Hex);

    for (const ColorFormat f : {ColorFormat::Hex, ColorFormat::Rgb, ColorFormat::Hsl, ColorFormat::Hsv}) {
        QCOMPARE(colorFormatFromKey(colorFormatConfigKey(f)), f);
    }
    QCOMPARE(colorFormatFromKey(QStringLiteral("unknown")), ColorFormat::Hex);
}

void LoupePickerTest::fractionalDpiNativePixelSampling()
{
    for (const qreal scale : {1.0, 1.25, 1.5, 2.0}) {
        const QSize nativeSize(400, 300);
        QImage native(nativeSize, QImage::Format_RGB32);
        native.fill(Qt::black);
        // Set distinctive pixel stripes to catch off-by-one scaling errors.
        for (int y = 0; y < native.height(); ++y) {
            for (int x = 0; x < native.width(); ++x) {
                native.setPixelColor(x, y, QColor((x * 5) % 256, (y * 7) % 256, ((x ^ y) * 3) % 256));
            }
        }

        const QPointF origin(-100.0, 50.0);
        const QPointF logicalCursor(origin.x() + 80.0, origin.y() + 40.0);
        const QPoint nativePx = logicalToNativePixel(logicalCursor, origin, scale, nativeSize);
        const int expectedX = int(std::floor(80.0 * scale));
        const int expectedY = int(std::floor(40.0 * scale));
        QCOMPARE(nativePx, QPoint(expectedX, expectedY));

        const QColor sampled = sampleNativePixel(native, nativePx);
        QCOMPARE(sampled, native.pixelColor(expectedX, expectedY));

        const QImage patch11 = extractNativePatch(native, nativePx, 11);
        QCOMPARE(patch11.size(), QSize(11, 11));
        QCOMPARE(patch11.pixelColor(5, 5), sampled);

        const QImage patch15 = extractNativePatch(native, nativePx, 15);
        QCOMPARE(patch15.size(), QSize(15, 15));
        QCOMPARE(patch15.pixelColor(7, 7), sampled);
    }

    // Out-of-bounds pixels near top-left edge (0, 0) stay transparent in the extracted patch.
    QImage corner(20, 20, QImage::Format_RGB32);
    corner.fill(QColor(12, 34, 56));
    const QImage edgePatch = extractNativePatch(corner, QPoint(0, 0), 11);
    QCOMPARE(qAlpha(edgePatch.pixel(0, 0)), 0);
    QCOMPARE(qAlpha(edgePatch.pixel(4, 4)), 0);
    QCOMPARE(qAlpha(edgePatch.pixel(5, 5)), 255);
    QCOMPARE(edgePatch.pixelColor(5, 5), QColor(12, 34, 56));
}

void LoupePickerTest::loupeSmartQuadrantPlacement()
{
    const QRect screen(0, 0, 1920, 1080);
    const QSize loupe(170, 200);

    for (const QPoint &cursor : {QPoint(960, 540), QPoint(5, 5), QPoint(1915, 5), QPoint(5, 1075), QPoint(1915, 1075)}) {
        const QRect placed = placeLoupeRect(cursor, loupe, screen);
        QCOMPARE(placed.size(), loupe);
        QVERIFY2(screen.contains(placed), qPrintable(QStringLiteral("cursor=%1,%2").arg(cursor.x()).arg(cursor.y())));
        QVERIFY2(!placed.contains(cursor), qPrintable(QStringLiteral("cursor=%1,%2").arg(cursor.x()).arg(cursor.y())));
    }
}

void LoupePickerTest::storePersistsColorFormatAndLoupeToggle()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("glimplyrc"));

    TargetStore defaults;
    QVERIFY(defaults.showSelectionLoupe);
    QCOMPARE(defaults.colorPickerFormat, ColorFormat::Hex);

    for (const ColorFormat fmt : {ColorFormat::Hsl, ColorFormat::Hsv, ColorFormat::Rgb, ColorFormat::Hex}) {
        TargetStore store;
        store.showSelectionLoupe = false;
        store.colorPickerFormat = fmt;
        {
            KConfig cfg(path, KConfig::SimpleConfig);
            store.save(cfg);
        }
        TargetStore reloaded;
        {
            KConfig cfg(path, KConfig::SimpleConfig);
            reloaded.load(cfg);
        }
        QVERIFY(!reloaded.showSelectionLoupe);
        QCOMPARE(reloaded.colorPickerFormat, fmt);
    }
}

void LoupePickerTest::quickEditorColorPickerCopiesAndAppliesColor()
{
    QImage img(200, 100, QImage::Format_RGB32);
    {
        QPainter p(&img);
        p.fillRect(0, 0, 100, 100, QColor(220, 30, 40));
        p.fillRect(100, 0, 100, 100, QColor(30, 80, 220));
    }

    TargetStore store;
    store.colorPickerFormat = ColorFormat::Hex;
    QuickEditorWindow window(store, img, 1.0, std::make_unique<AnnotationDocument>(), QStringLiteral("picker"));
    QVERIFY(window.isValid());
    window.resize(700, 500);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    AnnotationCanvas *canvas = window.canvas();
    QVERIFY(canvas != nullptr);

    // 1. Key 'I' enables picker, 'Tab' cycles format to RGB, click red half copies rgb(220, 30, 40).
    canvas->setFocus();
    QTest::keyClick(canvas, Qt::Key_I);
    QVERIFY(canvas->isColorPickerActive());

    QTest::keyClick(canvas, Qt::Key_Tab);
    QCOMPARE(store.colorPickerFormat, ColorFormat::Rgb);
    QCOMPARE(canvas->colorFormat(), ColorFormat::Rgb);

    const QPoint redWidgetPos = canvas->logicalToWidget(QPointF(40.0, 50.0)).toPoint();
    QTest::mouseMove(canvas, redWidgetPos);
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, redWidgetPos);
    QVERIFY(!canvas->isColorPickerActive());
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("rgb(220, 30, 40)"));

    // 2. Toggle picker again, Alt+Click blue half updates active annotation color too.
    canvas->controller()->setTool(ToolKind::Rectangle);
    QTest::keyClick(canvas, Qt::Key_I);
    QVERIFY(canvas->isColorPickerActive());
    const QPoint blueWidgetPos = canvas->logicalToWidget(QPointF(150.0, 50.0)).toPoint();
    QMouseEvent moveBlue(QEvent::MouseMove, QPointF(blueWidgetPos), canvas->mapToGlobal(blueWidgetPos), Qt::NoButton,
                         Qt::NoButton, Qt::AltModifier);
    QMouseEvent pressBlue(QEvent::MouseButtonPress, QPointF(blueWidgetPos), canvas->mapToGlobal(blueWidgetPos),
                          Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
    QMouseEvent releaseBlue(QEvent::MouseButtonRelease, QPointF(blueWidgetPos), canvas->mapToGlobal(blueWidgetPos),
                            Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
    QApplication::sendEvent(canvas, &moveBlue);
    QApplication::sendEvent(canvas, &pressBlue);
    QApplication::sendEvent(canvas, &releaseBlue);
    QVERIFY(!canvas->isColorPickerActive());
    QCOMPARE(canvas->controller()->strokeColor(), QColor(30, 80, 220));
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("rgb(30, 80, 220)"));

    // 3. Escape cancels picker without altering clipboard.
    QTest::keyClick(canvas, Qt::Key_I);
    QVERIFY(canvas->isColorPickerActive());
    QTest::keyClick(canvas, Qt::Key_Escape);
    QVERIFY(!canvas->isColorPickerActive());
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("rgb(30, 80, 220)"));

    window.setCloseDecisionOverride(QuickEditorWindow::CloseDecision::Discard);
    window.close();
}

void LoupePickerTest::rtlColorPickerLoupeMirrorsSwatchRowOnly()
{
    QImage native(100, 100, QImage::Format_RGB32);
    {
        QPainter p(&native);
        p.fillRect(0, 0, 50, 100, QColor(220, 30, 30));
        p.fillRect(50, 0, 50, 100, QColor(30, 60, 220));
    }
    // Single green pixel sampled in the swatch without coloring the left/right grid halves.
    native.setPixelColor(50, 50, QColor(0, 230, 40));

    auto render = [&](Qt::LayoutDirection direction) {
        QImage target(400, 400, QImage::Format_ARGB32_Premultiplied);
        target.fill(Qt::white);
        QPainter painter(&target);
        painter.setLayoutDirection(direction);
        PixelLoupe::paintColorPickerLoupe(painter, QPoint(40, 40), target.rect(), QGuiApplication::palette(), native,
                                          QPoint(50, 50), ColorFormat::Hex, false);
        return target;
    };

    auto analyze = [](const QImage &rendered, double *redMeanX, double *blueMeanX, double *swatchMeanX) {
        long redSum = 0, redCount = 0;
        long blueSum = 0, blueCount = 0;
        int maxGridY = -1;
        for (int y = 0; y < rendered.height(); ++y) {
            for (int x = 0; x < rendered.width(); ++x) {
                const QColor c = rendered.pixelColor(x, y);
                if (c.red() > 180 && c.green() < 80 && c.blue() < 80) {
                    redSum += x;
                    ++redCount;
                    maxGridY = std::max(maxGridY, y);
                } else if (c.blue() > 180 && c.red() < 80 && c.green() < 100) {
                    blueSum += x;
                    ++blueCount;
                    maxGridY = std::max(maxGridY, y);
                }
            }
        }
        QVERIFY(redCount > 0);
        QVERIFY(blueCount > 0);
        *redMeanX = double(redSum) / double(redCount);
        *blueMeanX = double(blueSum) / double(blueCount);

        long swatchSum = 0, swatchCount = 0;
        for (int y = maxGridY + 2; y < rendered.height(); ++y) {
            for (int x = 0; x < rendered.width(); ++x) {
                const QColor c = rendered.pixelColor(x, y);
                if (c.green() > 190 && c.red() < 40 && c.blue() < 80) {
                    swatchSum += x;
                    ++swatchCount;
                }
            }
        }
        QVERIFY(swatchCount > 0);
        *swatchMeanX = double(swatchSum) / double(swatchCount);
    };

    double ltrRedX = 0, ltrBlueX = 0, ltrSwatchX = 0;
    double rtlRedX = 0, rtlBlueX = 0, rtlSwatchX = 0;
    analyze(render(Qt::LeftToRight), &ltrRedX, &ltrBlueX, &ltrSwatchX);
    analyze(render(Qt::RightToLeft), &rtlRedX, &rtlBlueX, &rtlSwatchX);

    // The magnified grid always preserves physical left (red) vs right (blue).
    QVERIFY(ltrRedX < ltrBlueX);
    QVERIFY(rtlRedX < rtlBlueX);
    const double gridCenterX = 0.5 * (rtlRedX + rtlBlueX);
    // In LTR the color swatch leads on the left of the text; in RTL it leads on the right.
    QVERIFY(ltrSwatchX < gridCenterX);
    QVERIFY(rtlSwatchX > gridCenterX);
}

QTEST_MAIN(LoupePickerTest)
#include "loupepickertest.moc"
