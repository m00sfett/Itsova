#include "annotations/annotationcanvas.h"
#include "annotations/annotationcontroller.h"
#include "annotations/annotationdocument.h"
#include "annotations/annotationgeometry.h"
#include "annotations/annotationitem.h"
#include "annotations/annotationrenderer.h"
#include "annotations/annotationsettings.h"
#include "annotations/annotationtoolbar.h"
#include "targets.h"

#include <KConfig>

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>

#include <cmath>

using namespace glimply;

class CoreMarkupToolsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void arrowSnapAndNonProtrudingTip();
    void shapesOutlineAndTranslucentFill();
    void penSmoothingAndSimplification();
    void multiplyHighlighterContrastAndSelfOverlap();
    void stepCounterClickDragRenumberAndContrast();
    void spotlightUnifiedDimAndCalloutLayering();
    void settingsConfigRoundTrip();
    void toolbarProgressiveDisclosureAndLiveSync();
    void toolbarPanelModeKeepsOnlyThePropertyRow();
};

void CoreMarkupToolsTest::arrowSnapAndNonProtrudingTip()
{
    const QPointF snapped = snapAngle15(QPointF(100.0, 100.0), QPointF(195.0, 130.0));
    const qreal deg = std::atan2(snapped.y() - 100.0, snapped.x() - 100.0) * 180.0 / 3.14159265358979323846;
    QVERIFY(std::abs(deg - 15.0) < 0.05);

    // Short thick arrow: shaft round-cap must not poke past end.x() = 126.
    AnnotationDocument doc;
    AnnotationItem arrow;
    arrow.kind = ItemKind::Arrow;
    arrow.start = QPointF(100.0, 60.0);
    arrow.end = QPointF(126.0, 60.0);
    arrow.color = QColor(0xE5, 0x39, 0x35);
    arrow.strokeWidth = 16.0;
    doc.addItem(arrow);

    QImage base(200, 120, QImage::Format_RGB32);
    base.fill(Qt::white);
    const QImage out = renderAnnotatedCrop(base, QRectF(0.0, 0.0, 200.0, 120.0), 1.0, doc);

    for (int x = 128; x < 160; ++x) {
        for (int y = 40; y < 80; ++y) {
            QCOMPARE(out.pixelColor(x, y), QColor(Qt::white));
        }
    }
    QVERIFY(out.pixelColor(118, 60).red() > 180);
    QVERIFY(out.pixelColor(118, 60).green() < 80);
}

void CoreMarkupToolsTest::shapesOutlineAndTranslucentFill()
{
    AnnotationDocument doc;
    AnnotationItem outline;
    outline.kind = ItemKind::Rectangle;
    outline.rect = QRectF(20.0, 20.0, 60.0, 60.0);
    outline.fillMode = ShapeFillMode::Outline;
    outline.color = QColor(0x1E, 0x88, 0xE5);
    outline.strokeWidth = 4.0;
    doc.addItem(outline);

    AnnotationItem filled;
    filled.kind = ItemKind::Ellipse;
    filled.rect = QRectF(110.0, 20.0, 60.0, 60.0);
    filled.fillMode = ShapeFillMode::TranslucentFill;
    filled.fillOpacity = 0.5;
    filled.color = QColor(0x1E, 0x88, 0xE5);
    filled.strokeWidth = 4.0;
    doc.addItem(filled);

    QImage base(200, 100, QImage::Format_RGB32);
    base.fill(Qt::white);
    const QImage out = renderAnnotatedCrop(base, QRectF(0.0, 0.0, 200.0, 100.0), 1.0, doc);

    // Center of Outline rectangle remains untouched white.
    QCOMPARE(out.pixelColor(50, 50), QColor(Qt::white));
    // Center of TranslucentFill ellipse is blended ~50% toward blue.
    const QColor center = out.pixelColor(140, 50);
    QVERIFY(center.blue() > 220);
    QVERIFY(center.red() > 100 && center.red() < 180);
}

void CoreMarkupToolsTest::penSmoothingAndSimplification()
{
    const QList<QPointF> raw{
        {10.0, 10.0}, {20.0, 10.1}, {30.0, 10.0}, {40.0, 10.1}, {50.0, 10.0}, {80.0, 50.0}
    };
    const QList<QPointF> simplified = simplifyPolyline(raw, 0.5);
    QCOMPARE(simplified.size(), 3);
    QCOMPARE(simplified.first(), raw.first());
    QCOMPARE(simplified.last(), raw.last());

    AnnotationDocument doc;
    AnnotationItem pen;
    pen.kind = ItemKind::Pen;
    pen.points = simplified;
    pen.color = QColor(0x43, 0xA0, 0x47);
    pen.strokeWidth = 6.0;
    doc.addItem(pen);

    QImage base(120, 80, QImage::Format_RGB32);
    base.fill(Qt::white);
    const QImage out = renderAnnotatedCrop(base, QRectF(0.0, 0.0, 120.0, 80.0), 1.0, doc);
    QVERIFY(out.pixelColor(30, 10).green() > 130);
    QVERIFY(out.pixelColor(30, 10).red() < 100);
}

void CoreMarkupToolsTest::multiplyHighlighterContrastAndSelfOverlap()
{
    QImage base(250, 125, QImage::Format_RGB32);
    base.fill(Qt::white);
    // Crisp black text bar on white background (simulating dark text glyphs).
    for (int y = 45; y <= 55; ++y) {
        for (int x = 30; x < 180; ++x) {
            base.setPixelColor(x, y, Qt::black);
        }
    }

    AnnotationDocument doc;
    AnnotationItem hl;
    hl.kind = ItemKind::Highlighter;
    // Zig-zag back and forth across y = 40..60 twice in the same stroke.
    hl.points = {
        QPointF(20.0, 40.0), QPointF(160.0, 40.0),
        QPointF(20.0, 42.0), QPointF(160.0, 42.0),
    };
    hl.color = QColor(0xFD, 0xD8, 0x35);
    hl.strokeWidth = 24.0;
    doc.addItem(hl);

    // Test at fractional mixed-DPI scale 1.25.
    const QImage out = renderAnnotatedCrop(base, QRectF(0.0, 0.0, 200.0, 100.0), 1.25, doc);

    // Black text pixels remain pure black (0, 0, 0).
    QCOMPARE(out.pixelColor(100, 50), QColor(0, 0, 0));

    // White pixels in both single and overlapping zig-zag band equal exact #FDD835.
    const QColor highlightedWhite = out.pixelColor(100, 35);
    const QColor overlapWhite = out.pixelColor(100, 62);
    QCOMPARE(highlightedWhite, QColor(0xFD, 0xD8, 0x35));
    QCOMPARE(overlapWhite, QColor(0xFD, 0xD8, 0x35));
}

void CoreMarkupToolsTest::stepCounterClickDragRenumberAndContrast()
{
    AnnotationCanvas canvas;
    canvas.resize(400, 300);
    QImage bg(400, 300, QImage::Format_RGB32);
    bg.fill(Qt::white);
    canvas.setImage(bg, 1.0);

    AnnotationController *ctrl = canvas.controller();
    AnnotationDocument *doc = canvas.document();
    ctrl->setTool(ToolKind::Step);

    // Click -> badge #1 without pointer.
    QMouseEvent p1(QEvent::MouseButtonPress, QPointF(40, 40), QPointF(40, 40), Qt::LeftButton, Qt::LeftButton,
                   Qt::NoModifier);
    QMouseEvent r1(QEvent::MouseButtonRelease, QPointF(42, 41), QPointF(42, 41), Qt::LeftButton, Qt::NoButton,
                   Qt::NoModifier);
    QApplication::sendEvent(&canvas, &p1);
    QApplication::sendEvent(&canvas, &r1);

    // Drag -> badge #2 with attached pointer.
    QMouseEvent p2(QEvent::MouseButtonPress, QPointF(120, 40), QPointF(120, 40), Qt::LeftButton, Qt::LeftButton,
                   Qt::NoModifier);
    QMouseEvent m2(QEvent::MouseMove, QPointF(200, 90), QPointF(200, 90), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent r2(QEvent::MouseButtonRelease, QPointF(200, 90), QPointF(200, 90), Qt::LeftButton, Qt::NoButton,
                   Qt::NoModifier);
    QApplication::sendEvent(&canvas, &p2);
    QApplication::sendEvent(&canvas, &m2);
    QApplication::sendEvent(&canvas, &r2);

    // Click -> badge #3.
    QMouseEvent p3(QEvent::MouseButtonPress, QPointF(280, 40), QPointF(280, 40), Qt::LeftButton, Qt::LeftButton,
                   Qt::NoModifier);
    QMouseEvent r3(QEvent::MouseButtonRelease, QPointF(280, 40), QPointF(280, 40), Qt::LeftButton, Qt::NoButton,
                   Qt::NoModifier);
    QApplication::sendEvent(&canvas, &p3);
    QApplication::sendEvent(&canvas, &r3);

    QCOMPARE(doc->count(), 3);
    QVERIFY(!doc->items()[0].hasPointer);
    QVERIFY(doc->items()[1].hasPointer);
    QCOMPARE(doc->items()[0].stepNumber, 1);
    QCOMPARE(doc->items()[1].stepNumber, 2);
    QCOMPARE(doc->items()[2].stepNumber, 3);

    // Delete #2 -> remaining renumber to 1, 2; Undo restores 1, 2, 3.
    const int id2 = doc->items()[1].id;
    doc->removeItem(id2);
    QCOMPARE(doc->count(), 2);
    QCOMPARE(doc->items()[0].stepNumber, 1);
    QCOMPARE(doc->items()[1].stepNumber, 2);

    doc->undoStack().undo();
    QCOMPARE(doc->count(), 3);
    QCOMPARE(doc->items()[0].stepNumber, 1);
    QCOMPARE(doc->items()[1].stepNumber, 2);
    QCOMPARE(doc->items()[2].stepNumber, 3);

    // WCAG contrast checks: every color achieves > 4.5:1 against white or black.
    QCOMPARE(contrastingTextColor(QColor(0x11, 0x11, 0x11)), QColor(0xFF, 0xFF, 0xFF));
    QCOMPARE(contrastingTextColor(QColor(0xB1, 0x12, 0x18)), QColor(0xFF, 0xFF, 0xFF));
    QCOMPARE(contrastingTextColor(QColor(0xE5, 0x39, 0x35)), QColor(0x00, 0x00, 0x00));
    QCOMPARE(contrastingTextColor(QColor(0xFD, 0xD8, 0x35)), QColor(0x00, 0x00, 0x00));
    QCOMPARE(contrastingTextColor(QColor(0xFF, 0xFF, 0xFF)), QColor(0x00, 0x00, 0x00));
}

void CoreMarkupToolsTest::spotlightUnifiedDimAndCalloutLayering()
{
    AnnotationDocument doc;

    AnnotationItem sp1;
    sp1.kind = ItemKind::Spotlight;
    sp1.rect = QRectF(40.0, 40.0, 80.0, 80.0);
    sp1.dimOpacity = 0.60;
    doc.addItem(sp1);

    AnnotationItem sp2;
    sp2.kind = ItemKind::Spotlight;
    sp2.rect = QRectF(80.0, 40.0, 80.0, 80.0); // overlaps sp1 on x in [80, 120]
    sp2.dimOpacity = 0.60;
    doc.addItem(sp2);

    AnnotationItem arrow;
    arrow.kind = ItemKind::Arrow;
    arrow.start = QPointF(20.0, 150.0);
    arrow.end = QPointF(180.0, 150.0);
    arrow.color = QColor(0xE5, 0x39, 0x35);
    arrow.strokeWidth = 6.0;
    doc.addItem(arrow);

    QImage base(220, 180, QImage::Format_RGB32);
    base.fill(Qt::white);
    const QImage out = renderAnnotatedCrop(base, QRectF(0.0, 0.0, 220.0, 180.0), 1.0, doc);

    // Inside overlap of sp1 & sp2: pure un-dimmed white.
    QCOMPARE(out.pixelColor(100, 80), QColor(Qt::white));

    // Outside both spotlights: dimmed ONCE (255 * 0.40 = 102), never double-dimmed (41).
    const QColor outside = out.pixelColor(10, 10);
    QVERIFY(std::abs(outside.red() - 102) <= 3);
    QVERIFY(std::abs(outside.green() - 102) <= 3);
    QVERIFY(std::abs(outside.blue() - 102) <= 3);

    // Callout arrow drawn in dimmed zone stays full bright red above dim layer.
    const QColor arrowPx = out.pixelColor(100, 150);
    QVERIFY(arrowPx.red() > 215);
    QVERIFY(arrowPx.green() < 75);
}

void CoreMarkupToolsTest::settingsConfigRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("glimplyrc"));

    TargetStore store;
    store.annotations.strokeColor = QColor(0x1E, 0x88, 0xE5);
    store.annotations.highlighterColor = QColor(0x43, 0xA0, 0x47);
    store.annotations.strokeWidth = 9.0;
    store.annotations.highlighterWidth = 32.0;
    store.annotations.shapeFillMode = ShapeFillMode::TranslucentFill;
    store.annotations.shapeFillOpacity = 0.45;
    store.annotations.spotlightShape = SpotlightShape::Ellipse;
    store.annotations.spotlightDimOpacity = 0.75;
    {
        KConfig cfg(path, KConfig::SimpleConfig);
        store.save(cfg);
    }

    TargetStore loaded;
    {
        KConfig cfg(path, KConfig::SimpleConfig);
        loaded.load(cfg);
    }
    QCOMPARE(loaded.annotations.strokeColor, QColor(0x1E, 0x88, 0xE5));
    QCOMPARE(loaded.annotations.highlighterColor, QColor(0x43, 0xA0, 0x47));
    QCOMPARE(loaded.annotations.strokeWidth, 9.0);
    QCOMPARE(loaded.annotations.highlighterWidth, 32.0);
    QCOMPARE(loaded.annotations.shapeFillMode, ShapeFillMode::TranslucentFill);
    QCOMPARE(loaded.annotations.shapeFillOpacity, 0.45);
    QCOMPARE(loaded.annotations.spotlightShape, SpotlightShape::Ellipse);
    QCOMPARE(loaded.annotations.spotlightDimOpacity, 0.75);
}

void CoreMarkupToolsTest::toolbarProgressiveDisclosureAndLiveSync()
{
    AnnotationDocument doc;
    AnnotationController ctrl(&doc);
    AnnotationSettings settings;
    AnnotationToolbar toolbar(&ctrl);
    toolbar.setSettings(&settings);

    // Select with no selection: property bar stays hidden.
    QCOMPARE(ctrl.tool(), ToolKind::Select);
    QVERIFY(!toolbar.isPropertyBarVisible());

    // Arrow tool: color + size visible; fill + spotlight hidden.
    ctrl.setTool(ToolKind::Arrow);
    QVERIFY(toolbar.isPropertyBarVisible());
    QVERIFY(toolbar.isColorGroupVisible());
    QVERIFY(toolbar.isSizeGroupVisible());
    QVERIFY(!toolbar.isFillGroupVisible());
    QVERIFY(!toolbar.isSpotlightGroupVisible());

    // Rectangle Outline: fill group visible, opacity hidden until Fill chosen.
    ctrl.setTool(ToolKind::Rectangle);
    QVERIFY(toolbar.isFillGroupVisible());
    QVERIFY(!toolbar.isFillOpacityVisible());
    ctrl.setFillMode(ShapeFillMode::TranslucentFill);
    QVERIFY(toolbar.isFillOpacityVisible());

    // Spotlight: only spotlight controls visible.
    ctrl.setTool(ToolKind::Spotlight);
    QVERIFY(!toolbar.isColorGroupVisible());
    QVERIFY(!toolbar.isSizeGroupVisible());
    QVERIFY(!toolbar.isFillGroupVisible());
    QVERIFY(toolbar.isSpotlightGroupVisible());

    // Selecting an item under Select reveals its property bar and edits it undoably.
    AnnotationItem arr;
    arr.kind = ItemKind::Arrow;
    arr.start = QPointF(10, 10);
    arr.end = QPointF(80, 10);
    arr.color = QColor(0xE5, 0x39, 0x35);
    const int arrId = doc.addItem(arr);
    ctrl.setTool(ToolKind::Select);
    doc.select(arrId);
    QVERIFY(toolbar.isPropertyBarVisible());
    QVERIFY(toolbar.isColorGroupVisible());

    // Click swatch index 4 (#1E88E5 Breeze Blue).
    QList<QToolButton *> swatches = toolbar.swatchButtons();
    QVERIFY(swatches.size() >= 5);
    swatches[4]->click();
    QCOMPARE(doc.findItem(arrId)->color, QColor(0x1E, 0x88, 0xE5));
    QCOMPARE(settings.strokeColor, QColor(0x1E, 0x88, 0xE5));
    doc.undoStack().undo();
    QCOMPARE(doc.findItem(arrId)->color, QColor(0xE5, 0x39, 0x35));
}

int main(int argc, char *argv[])
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    CoreMarkupToolsTest test;
    return QTest::qExec(&test, argc, argv);
}

void CoreMarkupToolsTest::toolbarPanelModeKeepsOnlyThePropertyRow()
{
    AnnotationDocument doc;
    AnnotationController ctrl(&doc);
    AnnotationToolbar toolbar(&ctrl);
    QVERIFY(toolbar.isToolRowVisible());

    // #71: the overlay panel draws the tools; Undo/Redo move into the property row.
    toolbar.setPanelMode(true);
    QVERIFY(!toolbar.isToolRowVisible());
    QCOMPARE(toolbar.undoButton()->parentWidget(), toolbar.redoButton()->parentWidget());
    QVERIFY(toolbar.undoButton()->parentWidget() != toolbar.toolButton(ToolKind::Arrow)->parentWidget());

    // Select without a selection: the row stays for Undo/Redo, with no property group.
    QCOMPARE(ctrl.tool(), ToolKind::Select);
    QVERIFY(toolbar.isPropertyBarVisible());
    QVERIFY(!toolbar.isColorGroupVisible());
    QVERIFY(!toolbar.isSizeGroupVisible());

    ctrl.setTool(ToolKind::Redact);
    QVERIFY(toolbar.isRedactGroupVisible());
    ctrl.setTool(ToolKind::Select);
    QVERIFY(!toolbar.isRedactGroupVisible());

    // The widest row is measured without changing what is shown.
    const int widest = toolbar.widestPropertyRowWidth();
    QVERIFY(widest > 0);
    QVERIFY(!toolbar.isColorGroupVisible());
    QVERIFY(!toolbar.isTextGroupVisible());
    ctrl.setTool(ToolKind::Text);
    QVERIFY(toolbar.sizeHint().width() <= widest + 1);
    ctrl.setTool(ToolKind::Select);

    // Back to the editor layout: Undo/Redo return to the tool row.
    toolbar.setPanelMode(false);
    QVERIFY(toolbar.isToolRowVisible());
    QCOMPARE(toolbar.undoButton()->parentWidget(), toolbar.toolButton(ToolKind::Arrow)->parentWidget());
    QVERIFY(!toolbar.isPropertyBarVisible());
}

#include "coremarkuptoolstest.moc"
