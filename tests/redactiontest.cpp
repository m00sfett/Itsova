#include "annotations/annotationcontroller.h"
#include "annotations/annotationdocument.h"
#include "annotations/annotationitem.h"
#include "annotations/annotationrenderer.h"
#include "annotations/annotationsettings.h"
#include "annotations/annotationtoolbar.h"
#include "annotations/redactionprocessor.h"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QKeyEvent>
#include <QPainter>
#include <QSlider>
#include <QToolButton>
#include <QtTest>

#include <algorithm>
#include <cmath>

using namespace itsova;

class RedactionTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void solidBlockDestroysPixelsAndStripsMetadata();
    void fractionalHidpiOutwardSnapLeavesNoFringe();
    void pixelateEnforcesFloorDeterminismAndAlpha();
    void blurEnforcesFloorAndClampsToPatchBounds();
    void zOrderDestructionAndNoExportCornerTicks();
    void toolbarDisclosureAndKeyboardStrength();
};

void RedactionTest::solidBlockDestroysPixelsAndStripsMetadata()
{
    QImage base(200, 120, QImage::Format_ARGB32_Premultiplied);
    base.fill(Qt::white);
    // High-contrast secret text stripe inside the target area.
    for (int y = 30; y < 70; ++y) {
        for (int x = 30; x < 150; ++x) {
            base.setPixelColor(x, y, ((x + y) % 2 == 0) ? QColor(255, 255, 0) : QColor(0, 0, 255));
        }
    }
    base.setText(QStringLiteral("Comment"), QStringLiteral("top-secret-metadata"));
    base.setText(QStringLiteral("Author"), QStringLiteral("alice"));

    // Empty document still strips text metadata.
    AnnotationDocument emptyDoc;
    const QImage strippedEmpty = renderAnnotatedCrop(base, QRectF(0, 0, 200, 120), 1.0, emptyDoc);
    QVERIFY(strippedEmpty.textKeys().isEmpty());

    AnnotationDocument doc;
    AnnotationItem redact;
    redact.kind = ItemKind::Redact;
    redact.redactionMode = RedactionMode::Solid;
    redact.color = QColor(0, 0, 0, 40); // Even if caller sets low alpha, redaction must stay 255.
    redact.rect = QRectF(30, 30, 120, 40);
    doc.addItem(redact);

    const QImage out = renderAnnotatedCrop(base, QRectF(0, 0, 200, 120), 1.0, doc);
    QVERIFY(out.textKeys().isEmpty());
    QCOMPARE(out.devicePixelRatio(), 1.0);

    for (int y = 30; y < 70; ++y) {
        for (int x = 30; x < 150; ++x) {
            const QRgb px = out.pixel(x, y);
            QCOMPARE(qAlpha(px), 255);
            QCOMPARE(QColor::fromRgba(px), QColor(0, 0, 0));
        }
    }
    // Immediate outside border remains intact white.
    QCOMPARE(out.pixelColor(29, 50), QColor(Qt::white));
    QCOMPARE(out.pixelColor(150, 50), QColor(Qt::white));
}

void RedactionTest::fractionalHidpiOutwardSnapLeavesNoFringe()
{
    // 10.2 * 1.25 = 12.75 -> floor 12; 30.2 * 1.25 = 37.75 -> ceil 38 => physRect (12, 12, 26, 26).
    const QRect phys = redactionPhysicalRect(QRectF(10.2, 10.2, 20.0, 20.0), QPointF(0.0, 0.0), 1.25,
                                             QSize(100, 100));
    QCOMPARE(phys, QRect(12, 12, 26, 26));

    QImage native(100, 100, QImage::Format_ARGB32_Premultiplied);
    native.fill(Qt::black);
    // Place bright red secret markers right at physical edges overlapping the fractional box.
    native.setPixelColor(12, 12, Qt::red);
    native.setPixelColor(37, 37, Qt::red);

    AnnotationDocument doc;
    AnnotationItem redact;
    redact.kind = ItemKind::Redact;
    redact.redactionMode = RedactionMode::Solid;
    redact.color = Qt::black;
    redact.rect = QRectF(10.2, 10.2, 20.0, 20.0);
    doc.addItem(redact);

    const QImage out = renderAnnotatedCrop(native, QRectF(0.0, 0.0, 80.0, 80.0), 1.25, doc);
    QCOMPARE(out.pixelColor(12, 12), QColor(0, 0, 0));
    QCOMPARE(out.pixelColor(37, 37), QColor(0, 0, 0));
}

void RedactionTest::pixelateEnforcesFloorDeterminismAndAlpha()
{
    QImage base(200, 160, QImage::Format_ARGB32_Premultiplied);
    base.fill(Qt::white);
    // Alternating vertical fine bars inside [20, 140) x [20, 100)
    for (int y = 20; y < 100; ++y) {
        for (int x = 20; x < 140; ++x) {
            base.setPixelColor(x, y, (x % 4 < 2) ? QColor(20, 20, 20) : QColor(240, 240, 240));
        }
    }

    AnnotationDocument doc;
    AnnotationItem redact;
    redact.kind = ItemKind::Redact;
    redact.redactionMode = RedactionMode::Pixelate;
    redact.redactionStrength = 2; // Deliberately below hard 12 px secrecy floor.
    redact.jitterSeed = 0x12345678u;
    redact.rect = QRectF(20, 20, 120, 80);
    doc.addItem(redact);

    const QImage out1 = renderAnnotatedCrop(base, QRectF(0, 0, 200, 160), 1.0, doc);
    const QImage out2 = renderAnnotatedCrop(base, QRectF(0, 0, 200, 160), 1.0, doc);
    QCOMPARE(out1, out2);

    // Every pixel in patch has alpha 255, and blocks align on 12-px grid (not 2-px!):
    // Cell (gx=2, gy=2) spans physical x=[24, 36), y=[24, 36).
    const QRgb cellSample = out1.pixel(24, 24);
    for (int y = 24; y < 36; ++y) {
        for (int x = 24; x < 36; ++x) {
            QCOMPARE(qAlpha(out1.pixel(x, y)), 255);
            QCOMPARE(out1.pixel(x, y), cellSample);
        }
    }
    // Outside patch remains untouched white.
    QCOMPARE(out1.pixelColor(19, 50), QColor(Qt::white));
    QCOMPARE(out1.pixelColor(140, 50), QColor(Qt::white));
}

void RedactionTest::blurEnforcesFloorAndClampsToPatchBounds()
{
    QImage base(200, 160, QImage::Format_ARGB32_Premultiplied);
    base.fill(Qt::white);
    // Border sentinel right outside the patch: pure bright green, plus black/white stripes inside.
    for (int y = 20; y < 100; ++y) {
        for (int x = 20; x < 140; ++x) {
            base.setPixelColor(x, y, (x % 8 < 4) ? Qt::black : Qt::white);
        }
    }
    for (int y = 18; y < 102; ++y) {
        base.setPixelColor(19, y, Qt::green);
        base.setPixelColor(140, y, Qt::green);
    }

    AnnotationDocument doc;
    AnnotationItem redact;
    redact.kind = ItemKind::Redact;
    redact.redactionMode = RedactionMode::Blur;
    redact.redactionStrength = 1; // Below hard 14 px floor -> clamped up to 14 px.
    redact.rect = QRectF(20, 20, 120, 80);
    doc.addItem(redact);

    const QImage out = renderAnnotatedCrop(base, QRectF(0, 0, 200, 160), 1.0, doc);

    // Inside blurred region: fine black/white contrast is averaged to mid-gray with zero green leak!
    const QColor mid = out.pixelColor(80, 60);
    QCOMPARE(qAlpha(out.pixel(80, 60)), 255);
    QVERIFY(mid.red() > 90 && mid.red() < 170);
    QVERIFY(std::abs(mid.green() - mid.red()) <= 2);

    // Edge inside blur must not bleed green from x=19 or x=140.
    const QColor insideEdge = out.pixelColor(20, 60);
    QVERIFY(std::abs(insideEdge.green() - insideEdge.red()) <= 2);

    // Outside pixels remain pure untouched green.
    QCOMPARE(out.pixelColor(19, 60), QColor(Qt::green));
    QCOMPARE(out.pixelColor(140, 60), QColor(Qt::green));
}

void RedactionTest::zOrderDestructionAndNoExportCornerTicks()
{
    QImage base(240, 160, QImage::Format_ARGB32_Premultiplied);
    base.fill(QColor(220, 220, 220));

    AnnotationDocument doc;
    // 1. Arrow below redaction.
    AnnotationItem arrowBefore;
    arrowBefore.kind = ItemKind::Arrow;
    arrowBefore.start = QPointF(20, 60);
    arrowBefore.end = QPointF(200, 60);
    arrowBefore.color = QColor(0xE5, 0x39, 0x35);
    arrowBefore.strokeWidth = 6.0;
    doc.addItem(arrowBefore);

    // 2. Solid black redaction overwriting center segment of arrowBefore.
    AnnotationItem redact;
    redact.kind = ItemKind::Redact;
    redact.redactionMode = RedactionMode::Solid;
    redact.color = QColor(0, 0, 0);
    redact.rect = QRectF(60, 30, 100, 70);
    const int redactId = doc.addItem(redact);

    // 3. Bright rectangle drawn AFTER redaction -> appears on top of the black bar.
    AnnotationItem afterRect;
    afterRect.kind = ItemKind::Rectangle;
    afterRect.rect = QRectF(95, 50, 30, 30);
    afterRect.fillMode = ShapeFillMode::TranslucentFill;
    afterRect.fillOpacity = 1.0;
    afterRect.color = QColor(0xFD, 0xD8, 0x35);
    doc.addItem(afterRect);

    // Select the redaction so corner L-ticks appear in overlay preview.
    doc.select(redactId);

    // Verify overlay preview draws corner ticks inside inset (x=66, y=36 is white tick clear of handle).
    QImage previewImg(240, 160, QImage::Format_ARGB32_Premultiplied);
    previewImg.fill(QColor(220, 220, 220));
    {
        QPainter p(&previewImg);
        paintAnnotationOverlay(p, QRectF(0, 0, 240, 160), doc, QColor(0x1E, 0x88, 0xE5), redactId, nullptr, &base,
                               1.0, QPointF(0, 0));
    }
    QVERIFY(previewImg.pixelColor(66, 36).red() > 180);

    // Export NEVER draws corner ticks, destroys arrowBefore under redact, and keeps afterRect on top!
    const QImage exported = renderAnnotatedCrop(base, QRectF(0, 0, 240, 160), 1.0, doc);
    QCOMPARE(exported.pixelColor(66, 36), QColor(0, 0, 0));
    QCOMPARE(exported.pixelColor(70, 60), QColor(0, 0, 0)); // arrowBefore destroyed here
    QCOMPARE(exported.pixelColor(110, 65), QColor(0xFD, 0xD8, 0x35)); // afterRect intact above bar
    QVERIFY(exported.pixelColor(35, 60).red() > 180); // arrowBefore outside bar intact
}

void RedactionTest::toolbarDisclosureAndKeyboardStrength()
{
    AnnotationDocument doc;
    AnnotationController ctrl(&doc);
    AnnotationSettings settings;
    AnnotationToolbar toolbar(&ctrl);
    toolbar.setSettings(&settings);

    QVERIFY(toolbar.toolButton(ToolKind::Redact) != nullptr);
    QVERIFY(!toolbar.toolButton(ToolKind::Redact)->isHidden());

    // Activating Redact shows Redact controls; initial default is Solid Block.
    ctrl.setTool(ToolKind::Redact);
    QVERIFY(toolbar.isPropertyBarVisible());
    QVERIFY(toolbar.isRedactGroupVisible());
    QVERIFY(toolbar.redactionModeButton(RedactionMode::Solid)->isChecked());
    QVERIFY(toolbar.isColorGroupVisible());
    QVERIFY(!toolbar.isRedactStrengthVisible());
    QVERIFY(toolbar.redactionModeButton(RedactionMode::Solid)->toolTip().contains(
        QStringLiteral("recommended for sensitive text")));
    QVERIFY(toolbar.redactionModeButton(RedactionMode::Pixelate)->toolTip().contains(
        QStringLiteral("strict text secrecy")));

    // Switch to Pixelate -> swatches hide, strength slider 12..40 shows.
    toolbar.redactionModeButton(RedactionMode::Pixelate)->click();
    QVERIFY(!toolbar.isColorGroupVisible());
    QVERIFY(toolbar.isRedactStrengthVisible());
    QCOMPARE(toolbar.redactionStrengthSlider()->minimum(), 12);
    QCOMPARE(toolbar.redactionStrengthSlider()->maximum(), 40);
    QCOMPARE(settings.redactionMode, RedactionMode::Pixelate);

    // Switch to Blur -> strength slider 14..48 shows.
    toolbar.redactionModeButton(RedactionMode::Blur)->click();
    QVERIFY(!toolbar.isColorGroupVisible());
    QVERIFY(toolbar.isRedactStrengthVisible());
    QCOMPARE(toolbar.redactionStrengthSlider()->minimum(), 14);
    QCOMPARE(toolbar.redactionStrengthSlider()->maximum(), 48);
    QCOMPARE(settings.redactionMode, RedactionMode::Blur);

    // '[' and ']' adjust strength while respecting floor 14 px.
    ctrl.setRedactionStrength(14);
    QKeyEvent shrink(QEvent::KeyPress, Qt::Key_BracketLeft, Qt::NoModifier);
    QKeyEvent grow(QEvent::KeyPress, Qt::Key_BracketRight, Qt::NoModifier);
    QVERIFY(ctrl.keyPress(&shrink));
    QCOMPARE(ctrl.redactionStrength(), 14);
    QVERIFY(ctrl.keyPress(&grow));
    QCOMPARE(ctrl.redactionStrength(), 16);
}

int main(int argc, char *argv[])
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    RedactionTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "redactiontest.moc"
