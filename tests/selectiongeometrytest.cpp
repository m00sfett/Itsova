#include "imageoutput.h"
#include "selectiongeometry.h"

#include <QtTest>

using namespace itsova;

namespace
{
const QSize bounds(1000, 800);
constexpr SelectionConstraints none{};
constexpr SelectionConstraints centre{true, false};
constexpr SelectionConstraints square{false, true};
constexpr SelectionConstraints centredSquare{true, true};
} // namespace

class SelectionGeometryTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // #102: arrow keys nudge the dragged corner or edge by native pixels.
    void draggedEdgesFollowThePointerSide()
    {
        QCOMPARE(draggedEdges(QPoint(10, 10), QPoint(50, 40)), Qt::Edges(Qt::RightEdge | Qt::BottomEdge));
        QCOMPARE(draggedEdges(QPoint(10, 10), QPoint(2, 40)), Qt::Edges(Qt::LeftEdge | Qt::BottomEdge));
        QCOMPARE(draggedEdges(QPoint(10, 10), QPoint(50, 3)), Qt::Edges(Qt::RightEdge | Qt::TopEdge));
    }

    void nudgeMovesOnlyTheDraggedEdges()
    {
        QCOMPARE(nudgedMargins(QMargins(), Qt::RightEdge | Qt::BottomEdge, QPoint(1, 0)), QMargins(0, 0, 1, 0));
        QCOMPARE(nudgedMargins(QMargins(0, 0, 1, 0), Qt::RightEdge | Qt::BottomEdge, QPoint(0, -10)),
                 QMargins(0, 0, 1, -10));
        QCOMPARE(nudgedMargins(QMargins(), Qt::LeftEdge, QPoint(-1, 5)), QMargins(-1, 0, 0, 0)); // y ignored
    }

    void nudgeStepsOneNativePixel_data()
    {
        QTest::addColumn<qreal>("scale");
        QTest::newRow("1.0") << 1.0;
        QTest::newRow("1.25") << 1.25;
        QTest::newRow("1.5") << 1.5;
    }

    void nudgeStepsOneNativePixel()
    {
        QFETCH(qreal, scale);
        const QRect logical(11, 9, 37, 29);
        const QRect base = nativeAreaFor(logical, scale);
        const QRect once = nudgedNativeArea(base, QMargins(0, 0, 1, 0));
        QCOMPARE(once.width(), base.width() + 1);
        QCOMPARE(once.left(), base.left());
        const QRect up = nudgedNativeArea(base, QMargins(0, -1, 0, 0));
        QCOMPARE(up.top(), base.top() - 1);
        QCOMPARE(up.height(), base.height() + 1);
        // The crop takes exactly the nudged pixels.
        QImage frame(240, 240, QImage::Format_ARGB32);
        frame.fill(Qt::white);
        const QImage crop = cropLogicalArea(frame, logical, scale, nullptr, QMargins(0, -1, 1, 0));
        QCOMPARE(crop.size(), QSize(base.width() + 1, base.height() + 1));
    }

    void deadZoneIgnoresSmallMovesAndContinuesWithoutAJump()
    {
        NudgeDeadZone zone;
        QVERIFY(!zone.active());
        QCOMPARE(zone.filter(QPoint(100, 100)), std::optional<QPoint>(QPoint(100, 100))); // no key yet
        zone.keyPressed(QPoint(100, 100));
        QVERIFY(zone.active());
        QCOMPARE(zone.filter(QPoint(104, 103)), std::optional<QPoint>()); // jitter: ignored
        QCOMPARE(zone.filter(QPoint(110, 100)), std::optional<QPoint>()); // exactly 10: still ignored
        // Past 10 logical px the mouse takes over, from where the corner was: no jump.
        QCOMPARE(zone.filter(QPoint(111, 100)), std::optional<QPoint>(QPoint(100, 100)));
        QVERIFY(!zone.active());
        QCOMPARE(zone.filter(QPoint(121, 105)), std::optional<QPoint>(QPoint(110, 105)));
        // A new key press starts a new zone at the pointer; the offset stays.
        zone.keyPressed(QPoint(121, 105));
        QCOMPARE(zone.filter(QPoint(125, 105)), std::optional<QPoint>());
        QCOMPARE(zone.filter(QPoint(140, 105)), std::optional<QPoint>(QPoint(110, 105)));
    }

    void drawPlain()
    {
        QCOMPARE(drawnSelection({100, 100}, {300, 200}, none, bounds), QRect(100, 100, 200, 100));
        // Dragging up and left works the same way.
        QCOMPARE(drawnSelection({300, 200}, {100, 100}, none, bounds), QRect(100, 100, 200, 100));
    }

    void drawFromCentre()
    {
        QCOMPARE(drawnSelection({500, 400}, {600, 450}, centre, bounds), QRect(400, 350, 200, 100));
        QCOMPARE(drawnSelection({500, 400}, {400, 350}, centre, bounds), QRect(400, 350, 200, 100));
    }

    void drawSquare()
    {
        QCOMPARE(drawnSelection({100, 100}, {300, 150}, square, bounds), QRect(100, 100, 200, 200));
        // The square grows in the direction of the drag.
        QCOMPARE(drawnSelection({300, 300}, {250, 100}, square, bounds), QRect(100, 100, 200, 200));
    }

    void drawCentredSquare()
    {
        QCOMPARE(drawnSelection({500, 400}, {550, 420}, centredSquare, bounds), QRect(450, 350, 100, 100));
    }

    void drawClampsAndKeepsConstraints()
    {
        // Plain: each side stops at the screen edge.
        QCOMPARE(drawnSelection({900, 700}, {1200, 900}, none, bounds), QRect(900, 700, 100, 100));
        // From the centre: both sides shrink together, the centre stays.
        QCOMPARE(drawnSelection({100, 400}, {300, 500}, centre, bounds), QRect(0, 300, 200, 200));
        // Square: the shorter free space decides.
        QCOMPARE(drawnSelection({900, 100}, {1000, 600}, square, bounds), QRect(900, 100, 100, 100));
        // Centred square near a corner.
        QCOMPARE(drawnSelection({50, 50}, {300, 300}, centredSquare, bounds), QRect(0, 0, 100, 100));
    }

    void resizeEdgePlain()
    {
        const QRect start(200, 200, 200, 100);
        QCOMPARE(resizedSelection(start, Qt::RightEdge, {50, 30}, none, bounds), QRect(200, 200, 250, 100));
        QCOMPARE(resizedSelection(start, Qt::TopEdge, {0, -20}, none, bounds), QRect(200, 180, 200, 120));
        // Past the opposite edge the rectangle flips.
        QCOMPARE(resizedSelection(start, Qt::RightEdge, {-250, 0}, none, bounds), QRect(150, 200, 50, 100));
    }

    void resizeEdgeFromCentre()
    {
        const QRect start(200, 200, 200, 100);
        // The left edge mirrors the right one.
        QCOMPARE(resizedSelection(start, Qt::RightEdge, {50, 0}, centre, bounds), QRect(150, 200, 300, 100));
        QCOMPARE(resizedSelection(start, Qt::TopEdge, {0, 20}, centre, bounds), QRect(200, 220, 200, 60));
    }

    void resizeCornerFromCentre()
    {
        const QRect start(200, 200, 200, 100);
        QCOMPARE(resizedSelection(start, Qt::BottomEdge | Qt::RightEdge, {50, 25}, centre, bounds),
                 QRect(150, 175, 300, 150));
        QCOMPARE(resizedSelection(start, Qt::TopEdge | Qt::LeftEdge, {-50, -25}, centre, bounds),
                 QRect(150, 175, 300, 150));
    }

    void resizeCornerSquare()
    {
        const QRect start(200, 200, 200, 100);
        // The opposite corner stays, the longer side wins.
        QCOMPARE(resizedSelection(start, Qt::BottomEdge | Qt::RightEdge, {0, 0}, square, bounds),
                 QRect(200, 200, 200, 200));
        QCOMPARE(resizedSelection(start, Qt::TopEdge | Qt::LeftEdge, {0, 0}, square, bounds),
                 QRect(200, 100, 200, 200));
    }

    void resizeEdgeSquare()
    {
        const QRect start(200, 200, 200, 100);
        // The height follows the width, centred on the old vertical centre.
        QCOMPARE(resizedSelection(start, Qt::RightEdge, {0, 0}, square, bounds), QRect(200, 150, 200, 200));
        QCOMPARE(resizedSelection(start, Qt::BottomEdge, {0, 50}, square, bounds), QRect(225, 200, 150, 150));
    }

    void resizeCentredSquare()
    {
        const QRect start(200, 200, 200, 100);
        QCOMPARE(resizedSelection(start, Qt::BottomEdge | Qt::RightEdge, {0, 0}, centredSquare, bounds),
                 QRect(200, 150, 200, 200));
    }

    void resizeClampsFromCentre()
    {
        const QRect start(50, 300, 100, 100);
        // The left side hits the screen edge first; the right side stops at the mirrored distance.
        QCOMPARE(resizedSelection(start, Qt::RightEdge, {200, 0}, centre, bounds), QRect(0, 300, 200, 100));
    }

    void oddCentreKeepsExactSquare()
    {
        const QRect start(201, 201, 101, 51);
        const QRect result = resizedSelection(start, Qt::RightEdge, {0, 0}, square, bounds);
        QCOMPARE(result.width(), result.height());
        QCOMPARE(result.width(), 101);
    }

    void arrowsMove()
    {
        const QRect start(100, 100, 50, 40);
        QCOMPARE(steppedSelection(start, {1, 0}, none, 4, bounds), QRect(101, 100, 50, 40));
        QCOMPARE(steppedSelection(start, {0, -10}, none, 4, bounds), QRect(100, 90, 50, 40));
        // Moving stops at the screen border without shrinking.
        QCOMPARE(steppedSelection(QRect(995, 0, 5, 5), {10, 0}, none, 4, bounds), QRect(995, 0, 5, 5));
    }

    void arrowsResizeAroundCentre()
    {
        const QRect start(100, 100, 50, 40);
        QCOMPARE(steppedSelection(start, {1, 0}, centre, 4, bounds), QRect(99, 100, 52, 40));
        QCOMPARE(steppedSelection(start, {-1, 0}, centre, 4, bounds), QRect(101, 100, 48, 40));
        QCOMPARE(steppedSelection(start, {0, 10}, centre, 4, bounds), QRect(100, 90, 50, 60));
        QCOMPARE(steppedSelection(start, {0, -10}, centre, 4, bounds), QRect(100, 110, 50, 20));
        // An odd width keeps its odd parity, so the centre never shifts by half a pixel.
        QCOMPARE(steppedSelection(QRect(100, 100, 51, 40), {-1, 0}, centre, 4, bounds), QRect(101, 100, 49, 40));
    }

    void arrowsResizeAroundCentreStopAtLimits()
    {
        // Growing stops symmetrically at the border: 5 px room on the left allows 10 px more.
        QCOMPARE(steppedSelection(QRect(5, 100, 50, 40), {10, 0}, centre, 4, bounds), QRect(0, 100, 60, 40));
        // Shrinking stops at the minimum with the same parity.
        QCOMPARE(steppedSelection(QRect(100, 100, 6, 40), {-10, 0}, centre, 4, bounds), QRect(101, 100, 4, 40));
        QCOMPARE(steppedSelection(QRect(100, 100, 7, 40), {-10, 0}, centre, 4, bounds), QRect(101, 100, 5, 40));
    }

    void arrowsSquare()
    {
        // The first press makes a square of the width, then grows or shrinks it.
        QCOMPARE(steppedSelection(QRect(100, 100, 50, 30), {1, 0}, square, 4, bounds), QRect(100, 100, 51, 51));
        QCOMPARE(steppedSelection(QRect(100, 100, 50, 50), {0, 10}, square, 4, bounds), QRect(100, 100, 60, 60));
        QCOMPARE(steppedSelection(QRect(100, 100, 50, 50), {0, -10}, square, 4, bounds), QRect(100, 100, 40, 40));
        // Bounded by the nearer of right and bottom border.
        QCOMPARE(steppedSelection(QRect(900, 700, 90, 90), {10, 0}, square, 4, bounds), QRect(900, 700, 100, 100));
        QCOMPARE(steppedSelection(QRect(900, 750, 40, 40), {10, 0}, square, 4, bounds), QRect(900, 750, 50, 50));
    }

    void arrowsCentredSquare()
    {
        QCOMPARE(steppedSelection(QRect(100, 100, 50, 50), {1, 0}, centredSquare, 4, bounds), QRect(99, 99, 52, 52));
        // A rectangle becomes a square of its width around its own centre.
        QCOMPARE(steppedSelection(QRect(100, 100, 50, 30), {-1, 0}, centredSquare, 4, bounds),
                 QRect(101, 91, 48, 48));
        // The nearer border limits both sides.
        QCOMPARE(steppedSelection(QRect(10, 300, 40, 40), {10, 0}, centredSquare, 4, bounds),
                 QRect(0, 290, 60, 60));
    }

    void panelPlacement()
    {
        const QSize panel(600, 80);
        const QSize overlay(1920, 1080);
        // Top centre without a selection or when the selection is elsewhere.
        QCOMPARE(placeOverlayPanel(panel, overlay, QRect(), std::nullopt, 16), QPoint(660, 16));
        QCOMPARE(placeOverlayPanel(panel, overlay, QRect(100, 500, 300, 300), std::nullopt, 16), QPoint(660, 16));
        // A selection under the panel sends it to the bottom.
        QCOMPARE(placeOverlayPanel(panel, overlay, QRect(700, 50, 400, 300), std::nullopt, 16), QPoint(660, 984));
        // Both sides covered: the side that covers less wins.
        QCOMPARE(placeOverlayPanel(panel, overlay, QRect(0, 0, 1920, 1000), std::nullopt, 16), QPoint(660, 984));
        QCOMPARE(placeOverlayPanel(panel, overlay, QRect(0, 60, 1920, 1020), std::nullopt, 16), QPoint(660, 16));
        // A position chosen by hand wins and stays on the monitor.
        QCOMPARE(placeOverlayPanel(panel, overlay, QRect(700, 50, 400, 300), QPoint(40, 30), 16), QPoint(40, 30));
        QCOMPARE(placeOverlayPanel(panel, overlay, QRect(), QPoint(1800, 1050), 16), QPoint(1320, 1000));
        // A panel wider than the monitor starts at the left edge.
        QCOMPARE(placeOverlayPanel(QSize(2000, 80), overlay, QRect(), std::nullopt, 16), QPoint(0, 16));
    }

    void rtlOverlayChromePlacement()
    {
        const QRect roomy(100, 50, 200, 120);
        const QRect screenBounds(0, 0, 1000, 800);
        const QRect ltrClose = overlayCloseButtonRect(roomy, screenBounds, 24, Qt::LeftToRight);
        const QRect rtlClose = overlayCloseButtonRect(roomy, screenBounds, 24, Qt::RightToLeft);
        QCOMPARE(ltrClose.center(), QPoint(roomy.right() - 20, roomy.top() + 20));
        QCOMPARE(rtlClose.center(), QPoint(roomy.left() + 20, roomy.top() + 20));

        const QRect small(100, 50, 30, 30);
        QVERIFY(overlayCloseButtonRect(small, screenBounds, 24, Qt::LeftToRight).center().x() > small.right());
        QVERIFY(overlayCloseButtonRect(small, screenBounds, 24, Qt::RightToLeft).center().x() < small.left());

        const QRectF frame(100.0, 50.0, 200.0, 120.0);
        QCOMPARE(overlaySelectionBadgeCenter(frame, Qt::LeftToRight), QPointF(114.0, 64.0));
        QCOMPARE(overlaySelectionBadgeCenter(frame, Qt::RightToLeft), QPointF(286.0, 64.0));
    }
};

QTEST_GUILESS_MAIN(SelectionGeometryTest)
#include "selectiongeometrytest.moc"
