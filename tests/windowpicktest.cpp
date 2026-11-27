#include "windowpick.h"

#include <QTest>

using namespace itsova;

namespace
{

WindowInfo window(const QString &handle, const QRect &geometry, int layer = 2, bool fullscreen = false)
{
    WindowInfo w;
    w.handle = handle;
    w.caption = handle;
    w.geometry = geometry;
    w.layer = layer;
    w.fullscreen = fullscreen;
    return w;
}

ScreenInfo screen(const QString &connector, const QRect &geometry)
{
    ScreenInfo s;
    s.connector = connector;
    s.geometry = geometry;
    return s;
}

} // namespace

class WindowPickTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void pickableWindowsDropHiddenOnesAndSortByLayer()
    {
        WindowInfo minimized = window(QStringLiteral("min"), {0, 0, 100, 100});
        minimized.minimized = true;
        WindowInfo elsewhere = window(QStringLiteral("other-desktop"), {0, 0, 100, 100});
        elsewhere.onCurrentDesktop = false;
        const QList<WindowInfo> list{window(QStringLiteral("a"), {0, 0, 100, 100}), minimized, elsewhere,
                                     window(QStringLiteral("above"), {0, 0, 50, 50}, 4),
                                     window(QStringLiteral("b"), {0, 0, 100, 100})};
        const QList<WindowInfo> pickable = pickableWindows(list);
        QCOMPARE(pickable.size(), 3);
        QCOMPARE(pickable.at(0).handle, QStringLiteral("above")); // keep-above layer first
        QCOMPARE(pickable.at(1).handle, QStringLiteral("a"));     // then most recently used
        QCOMPARE(pickable.at(2).handle, QStringLiteral("b"));
    }

    void topmostWindowUnderThePointerWins()
    {
        const QList<ScreenInfo> screens{screen(QStringLiteral("DP-1"), {0, 0, 1920, 1080})};
        const QList<WindowInfo> windows{window(QStringLiteral("front"), {100, 100, 400, 300}),
                                        window(QStringLiteral("back"), {50, 50, 1000, 800})};
        PickTarget pick = pickTargetAt({200, 200}, windows, screens);
        QCOMPARE(pick.kind, PickTarget::Kind::Window);
        QCOMPARE(pick.window.handle, QStringLiteral("front"));
        QCOMPARE(pick.rect, QRect(100, 100, 400, 300));
        pick = pickTargetAt({900, 700}, windows, screens);
        QCOMPARE(pick.window.handle, QStringLiteral("back"));
    }

    void desktopAndFullScreenPickTheMonitor()
    {
        const QList<ScreenInfo> screens{screen(QStringLiteral("DP-1"), {0, 0, 1920, 1080}),
                                        screen(QStringLiteral("DP-2"), {1920, 0, 2560, 1440})};
        const QList<WindowInfo> windows{window(QStringLiteral("video"), {1920, 0, 2560, 1440}, 6, true),
                                        window(QStringLiteral("small"), {10, 10, 100, 100})};
        PickTarget pick = pickTargetAt({500, 500}, windows, screens); // desktop
        QCOMPARE(pick.kind, PickTarget::Kind::Monitor);
        QCOMPARE(pick.connector, QStringLiteral("DP-1"));
        QCOMPARE(pick.rect, QRect(0, 0, 1920, 1080));
        pick = pickTargetAt({2500, 300}, windows, screens); // full-screen window
        QCOMPARE(pick.kind, PickTarget::Kind::Monitor);
        QCOMPARE(pick.connector, QStringLiteral("DP-2"));
        pick = pickTargetAt({5000, 5000}, windows, screens); // nowhere
        QCOMPARE(pick.kind, PickTarget::Kind::None);
    }
};

QTEST_GUILESS_MAIN(WindowPickTest)
#include "windowpicktest.moc"
