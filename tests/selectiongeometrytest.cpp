#include "selectiongeometry.h"

#include <QtTest>

using namespace glimply;

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
};

QTEST_GUILESS_MAIN(SelectionGeometryTest)
#include "selectiongeometrytest.moc"
