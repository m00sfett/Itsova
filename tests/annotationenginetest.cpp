#include "annotations/annotationcanvas.h"
#include "annotations/annotationcontroller.h"
#include "annotations/annotationdocument.h"
#include "annotations/annotationgeometry.h"
#include "annotations/annotationitem.h"
#include "annotations/annotationrenderer.h"
#include "imageoutput.h"

#include <QApplication>
#include <QColor>
#include <QFocusEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QtTest>

#include <cmath>

using namespace itsova;

namespace
{

int countMatchingPixels(const QImage &img, const std::function<bool(const QColor &)> &pred)
{
    int hits = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            if (pred(img.pixelColor(x, y))) {
                ++hits;
            }
        }
    }
    return hits;
}

void sendMousePress(QWidget *w, const QPointF &pos, Qt::MouseButton btn = Qt::LeftButton,
                    Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QMouseEvent ev(QEvent::MouseButtonPress, pos, w->mapToGlobal(pos), btn, btn, mods);
    QApplication::sendEvent(w, &ev);
}

void sendMouseMove(QWidget *w, const QPointF &pos, Qt::MouseButtons btns = Qt::LeftButton,
                   Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QMouseEvent ev(QEvent::MouseMove, pos, w->mapToGlobal(pos), Qt::NoButton, btns, mods);
    QApplication::sendEvent(w, &ev);
}

void sendMouseRelease(QWidget *w, const QPointF &pos, Qt::MouseButton btn = Qt::LeftButton,
                      Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QMouseEvent ev(QEvent::MouseButtonRelease, pos, w->mapToGlobal(pos), btn, Qt::NoButton, mods);
    QApplication::sendEvent(w, &ev);
}

void sendKey(QWidget *w, QEvent::Type type, int key, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QKeyEvent ev(type, key, mods);
    QApplication::sendEvent(w, &ev);
}

} // namespace

class AnnotationEngineTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void hitTestingAccuracy();
    void modifiersAndCanvasGestures();
    void undoRedoCoalescingDuplicateAndCancel();
    void multiRectangleExportClipping();
    void nativeResolutionCrispnessAndDeterminism();
};

void AnnotationEngineTest::hitTestingAccuracy()
{
    AnnotationItem arrow;
    arrow.kind = ItemKind::Arrow;
    arrow.start = QPointF(20.0, 20.0);
    arrow.end = QPointF(120.0, 20.0);
    arrow.strokeWidth = 4.0;
    QVERIFY(arrow.hitTest(QPointF(70.0, 22.0)));
    QVERIFY(arrow.hitTest(QPointF(118.0, 24.0)));
    QVERIFY(!arrow.hitTest(QPointF(70.0, 40.0)));

    AnnotationItem outlineRect;
    outlineRect.kind = ItemKind::Rectangle;
    outlineRect.rect = QRectF(50.0, 50.0, 100.0, 80.0);
    outlineRect.fillMode = ShapeFillMode::Outline;
    outlineRect.strokeWidth = 4.0;
    QVERIFY(outlineRect.hitTest(QPointF(50.0, 90.0)));
    QVERIFY(!outlineRect.hitTest(QPointF(100.0, 90.0)));

    AnnotationItem filledRect = outlineRect;
    filledRect.fillMode = ShapeFillMode::TranslucentFill;
    QVERIFY(filledRect.hitTest(QPointF(100.0, 90.0)));

    AnnotationItem outlineEllipse;
    outlineEllipse.kind = ItemKind::Ellipse;
    outlineEllipse.rect = QRectF(200.0, 50.0, 100.0, 100.0);
    outlineEllipse.fillMode = ShapeFillMode::Outline;
    outlineEllipse.strokeWidth = 4.0;
    QVERIFY(outlineEllipse.hitTest(QPointF(250.0, 50.0)));
    QVERIFY(!outlineEllipse.hitTest(QPointF(250.0, 100.0)));

    AnnotationItem pen;
    pen.kind = ItemKind::Pen;
    pen.points = {QPointF(10.0, 200.0), QPointF(60.0, 200.0), QPointF(110.0, 240.0)};
    pen.strokeWidth = 4.0;
    QVERIFY(pen.hitTest(QPointF(60.0, 201.0)));
    QVERIFY(!pen.hitTest(QPointF(60.0, 240.0)));

    AnnotationDocument doc;
    AnnotationItem spotlight;
    spotlight.kind = ItemKind::Spotlight;
    spotlight.rect = QRectF(0.0, 0.0, 300.0, 300.0);
    const int spId = doc.addItem(spotlight);
    const int rectId = doc.addItem(outlineRect);
    const int arrowId = doc.addItem(arrow);
    QCOMPARE(doc.topmostHitAt(QPointF(70.0, 20.0)), arrowId);
    QCOMPARE(doc.topmostHitAt(QPointF(50.0, 90.0)), rectId);
    QCOMPARE(doc.topmostHitAt(QPointF(200.0, 200.0)), spId);
}

void AnnotationEngineTest::modifiersAndCanvasGestures()
{
    AnnotationCanvas canvas;
    canvas.resize(500, 400);
    QImage bg(500, 400, QImage::Format_RGB32);
    bg.fill(Qt::white);
    canvas.setImage(bg, 1.0);

    AnnotationController *ctrl = canvas.controller();
    AnnotationDocument *doc = canvas.document();

    // 1) Shift creates 1:1 square rectangle.
    ctrl->setTool(ToolKind::Rectangle);
    sendMousePress(&canvas, QPointF(50.0, 50.0), Qt::LeftButton, Qt::ShiftModifier);
    sendMouseMove(&canvas, QPointF(150.0, 90.0), Qt::LeftButton, Qt::ShiftModifier);
    sendMouseRelease(&canvas, QPointF(150.0, 90.0), Qt::LeftButton, Qt::ShiftModifier);
    QCOMPARE(doc->count(), 1);
    const AnnotationItem *sq = doc->selectedItem();
    QVERIFY(sq != nullptr);
    QCOMPARE(sq->rect.width(), sq->rect.height());
    QCOMPARE(sq->rect, QRectF(50.0, 50.0, 100.0, 100.0));

    // 2) Alt creates centered shape; Shift+Alt creates centered square.
    ctrl->setTool(ToolKind::Ellipse);
    sendMousePress(&canvas, QPointF(250.0, 200.0), Qt::LeftButton, Qt::ShiftModifier | Qt::AltModifier);
    sendMouseMove(&canvas, QPointF(290.0, 220.0), Qt::LeftButton, Qt::ShiftModifier | Qt::AltModifier);
    sendMouseRelease(&canvas, QPointF(290.0, 220.0), Qt::LeftButton, Qt::ShiftModifier | Qt::AltModifier);
    QCOMPARE(doc->count(), 2);
    const AnnotationItem *circ = doc->selectedItem();
    QVERIFY(circ != nullptr);
    QCOMPARE(circ->rect, QRectF(210.0, 160.0, 80.0, 80.0));

    // 3) Shift snaps Arrow angle to 15-degree increments (e.g. ~31 deg -> 30 deg).
    ctrl->setTool(ToolKind::Arrow);
    sendMousePress(&canvas, QPointF(100.0, 300.0), Qt::LeftButton, Qt::ShiftModifier);
    const QPointF rawTarget(186.6025, 355.0);
    sendMouseMove(&canvas, rawTarget, Qt::LeftButton, Qt::ShiftModifier);
    sendMouseRelease(&canvas, rawTarget, Qt::LeftButton, Qt::ShiftModifier);
    QCOMPARE(doc->count(), 3);
    const AnnotationItem *arr = doc->selectedItem();
    QVERIFY(arr != nullptr);
    const qreal angleDeg = std::atan2(arr->end.y() - arr->start.y(), arr->end.x() - arr->start.x())
                         * 180.0 / 3.14159265358979323846;
    QVERIFY(std::abs(angleDeg - 30.0) < 0.05);

    // 4) Space held mid-drag translates the in-progress rectangle cleanly.
    ctrl->setTool(ToolKind::Rectangle);
    sendMousePress(&canvas, QPointF(300.0, 50.0));
    sendMouseMove(&canvas, QPointF(360.0, 90.0));
    sendKey(&canvas, QEvent::KeyPress, Qt::Key_Space);
    sendMouseMove(&canvas, QPointF(380.0, 120.0)); // shifted by (+20, +30)
    sendKey(&canvas, QEvent::KeyRelease, Qt::Key_Space);
    sendMouseRelease(&canvas, QPointF(380.0, 120.0));
    QCOMPARE(doc->count(), 4);
    const AnnotationItem *movedDraft = doc->selectedItem();
    QVERIFY(movedDraft != nullptr);
    QCOMPARE(movedDraft->rect, QRectF(320.0, 80.0, 60.0, 40.0));

    // 5) Ctrl held temporarily switches to Select to move an item while rectangle tool is active.
    QCOMPARE(ctrl->tool(), ToolKind::Rectangle);
    QCOMPARE(ctrl->effectiveTool(Qt::ControlModifier), ToolKind::Select);
    // Press along the top edge (away from TopLeft (320,80) and Top (350,80) handles) to drag-move.
    sendMousePress(&canvas, QPointF(335.0, 80.0), Qt::LeftButton, Qt::ControlModifier);
    sendMouseMove(&canvas, QPointF(345.0, 90.0), Qt::LeftButton, Qt::ControlModifier);
    sendMouseRelease(&canvas, QPointF(345.0, 90.0), Qt::LeftButton, Qt::ControlModifier);
    QCOMPARE(doc->count(), 4);
    QCOMPARE(doc->selectedItem()->rect, QRectF(330.0, 90.0, 60.0, 40.0));
}

void AnnotationEngineTest::undoRedoCoalescingDuplicateAndCancel()
{
    AnnotationCanvas canvas;
    canvas.resize(400, 300);
    QImage bg(400, 300, QImage::Format_RGB32);
    bg.fill(Qt::white);
    canvas.setImage(bg, 1.0);

    AnnotationController *ctrl = canvas.controller();
    AnnotationDocument *doc = canvas.document();

    AnnotationItem r1;
    r1.kind = ItemKind::Rectangle;
    r1.rect = QRectF(40.0, 40.0, 60.0, 40.0);
    const int id1 = doc->addItem(r1);
    QCOMPARE(doc->count(), 1);

    // Consecutive arrow nudges coalesce into a single undo entry.
    sendKey(&canvas, QEvent::KeyPress, Qt::Key_Right, Qt::ShiftModifier);
    sendKey(&canvas, QEvent::KeyPress, Qt::Key_Right);
    sendKey(&canvas, QEvent::KeyPress, Qt::Key_Down);
    QCOMPARE(doc->findItem(id1)->rect, QRectF(51.0, 41.0, 60.0, 40.0));
    QCOMPARE(doc->undoStack().count(), 2);

    // Ctrl+Z undoes all coalesced nudges at once; Ctrl+Shift+Z and Ctrl+Y redo.
    sendKey(&canvas, QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(doc->findItem(id1)->rect, QRectF(40.0, 40.0, 60.0, 40.0));
    sendKey(&canvas, QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(doc->findItem(id1)->rect, QRectF(51.0, 41.0, 60.0, 40.0));
    sendKey(&canvas, QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
    sendKey(&canvas, QEvent::KeyPress, Qt::Key_Y, Qt::ControlModifier);
    QCOMPARE(doc->findItem(id1)->rect, QRectF(51.0, 41.0, 60.0, 40.0));

    // Alt+Drag on item edge (away from handles) in Select mode duplicates without mutating original.
    ctrl->setTool(ToolKind::Select);
    sendMousePress(&canvas, QPointF(66.0, 41.0), Qt::LeftButton, Qt::AltModifier);
    sendMouseMove(&canvas, QPointF(116.0, 71.0), Qt::LeftButton, Qt::AltModifier);
    sendMouseRelease(&canvas, QPointF(116.0, 71.0), Qt::LeftButton, Qt::AltModifier);
    QCOMPARE(doc->count(), 2);
    QCOMPARE(doc->findItem(id1)->rect, QRectF(51.0, 41.0, 60.0, 40.0));
    QCOMPARE(doc->selectedItem()->rect, QRectF(101.0, 71.0, 60.0, 40.0));

    // Z-order reorder + undo.
    const int id2 = doc->selectedId();
    doc->sendToBack(id2);
    QCOMPARE(doc->items().first().id, id2);
    doc->undoStack().undo();
    QCOMPARE(doc->items().last().id, id2);

    // Interrupted drag reset via Escape and focusOutEvent restores prior state cleanly.
    const int stackCountBefore = doc->undoStack().count();
    sendMousePress(&canvas, QPointF(101.0, 90.0));
    sendMouseMove(&canvas, QPointF(200.0, 200.0));
    sendKey(&canvas, QEvent::KeyPress, Qt::Key_Escape);
    QCOMPARE(doc->findItem(id2)->rect, QRectF(101.0, 71.0, 60.0, 40.0));
    QCOMPARE(doc->undoStack().count(), stackCountBefore);

    sendMousePress(&canvas, QPointF(101.0, 90.0));
    sendMouseMove(&canvas, QPointF(220.0, 210.0));
    QFocusEvent focusOut(QEvent::FocusOut);
    QApplication::sendEvent(&canvas, &focusOut);
    QCOMPARE(doc->findItem(id2)->rect, QRectF(101.0, 71.0, 60.0, 40.0));
    QCOMPARE(doc->undoStack().count(), stackCountBefore);
}

void AnnotationEngineTest::multiRectangleExportClipping()
{
    QImage frame(1600, 1200, QImage::Format_RGB32);
    frame.fill(Qt::white);

    AnnotationDocument doc;
    AnnotationItem redInA;
    redInA.kind = ItemKind::Rectangle;
    redInA.rect = QRectF(130.0, 130.0, 80.0, 60.0);
    redInA.color = QColor(220, 20, 20);
    redInA.strokeWidth = 6.0;
    doc.addItem(redInA);

    AnnotationItem blueInB;
    blueInB.kind = ItemKind::Rectangle;
    blueInB.rect = QRectF(640.0, 540.0, 100.0, 80.0);
    blueInB.color = QColor(20, 60, 230);
    blueInB.strokeWidth = 6.0;
    doc.addItem(blueInB);

    AnnotationItem greenAcrossA;
    greenAcrossA.kind = ItemKind::Arrow;
    greenAcrossA.start = QPointF(320.0, 260.0);
    greenAcrossA.end = QPointF(460.0, 260.0);
    greenAcrossA.color = QColor(20, 190, 40);
    greenAcrossA.strokeWidth = 6.0;
    doc.addItem(greenAcrossA);

    const QRect logicalCropA(100, 100, 300, 200);
    const QRect logicalCropB(600, 500, 300, 200);
    const qreal scale = 1.25;

    const QImage rawA = cropLogicalArea(frame, logicalCropA, scale);
    const QImage rawB = cropLogicalArea(frame, logicalCropB, scale);
    QCOMPARE(rawA.size(), QSize(375, 250));
    QCOMPARE(rawB.size(), QSize(375, 250));

    const QImage outA = renderAnnotatedCrop(rawA, logicalCropA, scale, doc);
    const QImage outB = renderAnnotatedCrop(rawB, logicalCropB, scale, doc);

    const auto isRed = [](const QColor &c) { return c.red() > 170 && c.green() < 80 && c.blue() < 80; };
    const auto isBlue = [](const QColor &c) { return c.blue() > 170 && c.red() < 80 && c.green() < 110; };
    const auto isGreen = [](const QColor &c) { return c.green() > 140 && c.red() < 80 && c.blue() < 90; };

    QVERIFY(countMatchingPixels(outA, isRed) > 100);
    QVERIFY(countMatchingPixels(outA, isGreen) > 50);
    QCOMPARE(countMatchingPixels(outA, isBlue), 0);

    QVERIFY(countMatchingPixels(outB, isBlue) > 100);
    QCOMPARE(countMatchingPixels(outB, isRed), 0);
    QCOMPARE(countMatchingPixels(outB, isGreen), 0);
}

void AnnotationEngineTest::nativeResolutionCrispnessAndDeterminism()
{
    AnnotationDocument doc;
    AnnotationItem lineRect;
    lineRect.kind = ItemKind::Rectangle;
    lineRect.rect = QRectF(40.0, 40.0, 120.0, 40.0);
    lineRect.color = QColor(220, 20, 20);
    lineRect.strokeWidth = 4.0;
    doc.addItem(lineRect);

    const QRectF cropLogical(0.0, 0.0, 200.0, 100.0);

    const auto renderAtScale = [&](qreal scale) {
        QImage base(int(std::lround(200.0 * scale)), int(std::lround(100.0 * scale)), QImage::Format_RGB32);
        base.fill(Qt::white);
        return renderAnnotatedCrop(base, cropLogical, scale, doc);
    };

    const QImage out1x = renderAtScale(1.0);
    const QImage out125xA = renderAtScale(1.25);
    const QImage out125xB = renderAtScale(1.25);
    const QImage out2x = renderAtScale(2.0);

    QCOMPARE(out1x.size(), QSize(200, 100));
    QCOMPARE(out125xA.size(), QSize(250, 125));
    QCOMPARE(out2x.size(), QSize(400, 200));
    QCOMPARE(out125xA.devicePixelRatio(), 1.0);

    // Deterministic output across repeated renders.
    QCOMPARE(QByteArrayView(out125xA.constBits(), out125xA.sizeInBytes()),
             QByteArrayView(out125xB.constBits(), out125xB.sizeInBytes()));

    // Measure vertical red stroke thickness across top edge at x = midpoint:
    // scale 2.0 must render ~2x the physical pixel band of scale 1.0.
    const auto redRowCountAtMidX = [](const QImage &img) {
        const int midX = img.width() / 2;
        int rows = 0;
        for (int y = 0; y < img.height() / 2; ++y) {
            const QColor c = img.pixelColor(midX, y);
            if (c.red() > 160 && c.green() < 100) {
                ++rows;
            }
        }
        return rows;
    };
    const int thickness1x = redRowCountAtMidX(out1x);
    const int thickness2x = redRowCountAtMidX(out2x);
    QVERIFY(thickness1x >= 3 && thickness1x <= 6);
    QVERIFY(thickness2x >= 7 && thickness2x <= 10);
}

int main(int argc, char *argv[])
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    AnnotationEngineTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "annotationenginetest.moc"
