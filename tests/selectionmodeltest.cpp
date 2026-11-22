#include "selectionmodel.h"

#include <QtTest>

using namespace glimply;

class SelectionModelTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void addMakesActiveAndTopmost()
    {
        SelectionModel model;
        const int a = model.add(QStringLiteral("DP-1"), QRect(0, 0, 100, 100));
        const int b = model.add(QStringLiteral("DP-1"), QRect(50, 50, 100, 100));
        QCOMPARE(model.activeId(), b);
        // In the overlap the newer rectangle wins.
        QCOMPARE(model.topmostAt(QStringLiteral("DP-1"), QPoint(60, 60)), b);
        QCOMPARE(model.topmostAt(QStringLiteral("DP-1"), QPoint(10, 10)), a);
        QCOMPARE(model.topmostAt(QStringLiteral("DP-1"), QPoint(300, 300)), 0);
        // Other monitors never match.
        QCOMPARE(model.topmostAt(QStringLiteral("HDMI-A-1"), QPoint(10, 10)), 0);
    }

    void activateMovesOnTop()
    {
        SelectionModel model;
        const int a = model.add(QStringLiteral("DP-1"), QRect(0, 0, 100, 100));
        model.add(QStringLiteral("DP-1"), QRect(50, 50, 100, 100));
        model.activate(a);
        QCOMPARE(model.activeId(), a);
        QCOMPARE(model.topmostAt(QStringLiteral("DP-1"), QPoint(60, 60)), a);
        QCOMPARE(model.stack().last().id, a);
    }

    void numbersFollowDrawingOrder()
    {
        SelectionModel model;
        const int a = model.add(QStringLiteral("DP-1"), QRect(0, 0, 10, 10));
        const int b = model.add(QStringLiteral("DP-1"), QRect(20, 0, 10, 10));
        const int c = model.add(QStringLiteral("DP-1"), QRect(40, 0, 10, 10));
        model.activate(a);
        QCOMPARE(model.number(a), 1);
        QCOMPARE(model.number(b), 2);
        QCOMPARE(model.number(c), 3);
        model.remove(b);
        // Numbers close the gap, so files stay numbered 1..n.
        QCOMPARE(model.number(c), 2);
        QCOMPARE(model.inDrawingOrder().first().id, a);
        QCOMPARE(model.number(b), 0);
    }

    void removingActiveLeavesNoneActive()
    {
        SelectionModel model;
        const int a = model.add(QStringLiteral("DP-1"), QRect(0, 0, 10, 10));
        model.remove(a);
        QCOMPARE(model.activeId(), 0);
        QVERIFY(model.active() == nullptr);
        QVERIFY(model.isEmpty());
    }

    void deactivateKeepsRectangles()
    {
        SelectionModel model;
        model.add(QStringLiteral("DP-1"), QRect(0, 0, 10, 10));
        model.deactivate();
        QCOMPARE(model.activeId(), 0);
        QCOMPARE(model.count(), 1);
    }

    void setRectUpdates()
    {
        SelectionModel model;
        const int a = model.add(QStringLiteral("DP-1"), QRect(0, 0, 10, 10));
        model.setRect(a, QRect(5, 5, 20, 20));
        QCOMPARE(model.find(a)->rect, QRect(5, 5, 20, 20));
    }
};

QTEST_GUILESS_MAIN(SelectionModelTest)
#include "selectionmodeltest.moc"
