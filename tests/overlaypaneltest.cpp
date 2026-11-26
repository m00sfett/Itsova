#include "overlaypanel.h"

#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLabel>
#include <QSignalSpy>
#include <QTest>

using namespace glimply;

namespace
{
// Hiding it calls back into the panel, like a hover outline that refreshes the panel does.
class ReentrantDrawer : public QWidget
{
public:
    explicit ReentrantDrawer(CapsulePanel *panel)
        : panel_(panel)
    {
    }

protected:
    void hideEvent(QHideEvent *event) override
    {
        panel_->setDrawer(nullptr);
        QWidget::hideEvent(event);
    }

private:
    CapsulePanel *panel_;
};
} // namespace

class OverlayPanelTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void shortTitleKeepsTheLargestSize()
    {
        const PanelTitleLayout layout = fitPanelTitle(QStringLiteral("Save"), QFont(), 56);
        QCOMPARE(layout.lines, QStringList{QStringLiteral("Save")});
        QCOMPARE(layout.pixelSize, 11.0);
        QVERIFY(!layout.shrunk);
        QVERIFY(!layout.elided);
    }

    void longTitleWrapsIntoAtMostThreeLines()
    {
        const PanelTitleLayout layout = fitPanelTitle(QStringLiteral("Im Fenster bearbeiten"), QFont(), 56);
        QVERIFY(!layout.elided);
        QVERIFY(layout.lines.size() >= 2);
        QVERIFY(layout.lines.size() <= 3);
        QCOMPARE(layout.lines.join(QLatin1Char(' ')), QStringLiteral("Im Fenster bearbeiten"));
    }

    void wordsAreNeverSplitBeforeEliding()
    {
        // One word wider than the button at every size: shrunk to the minimum, then elided.
        const PanelTitleLayout layout =
            fitPanelTitle(QStringLiteral("Donaudampfschifffahrtsgesellschaftskapitän"), QFont(), 56);
        QVERIFY(layout.elided);
        QCOMPARE(layout.pixelSize, 8.0);
        QCOMPARE(layout.lines.size(), 1);
        QVERIFY(layout.lines.first().endsWith(QChar(0x2026)));
    }

    void linesFitTheWidth()
    {
        const QStringList titles{QStringLiteral("Добавить прямоугольник"), QStringLiteral("Tous les écrans"),
                                 QStringLiteral("Alle Bildschirme"), QStringLiteral("すべてのディスプレイ")};
        for (const QString &title : titles) {
            const PanelTitleLayout layout = fitPanelTitle(title, QFont(), 56);
            QFont font;
            font.setPointSizeF(layout.pixelSize * 0.75);
            font.setWeight(QFont::DemiBold);
            const QFontMetricsF metrics(font);
            QVERIFY2(layout.lines.size() <= 3, qPrintable(title));
            for (const QString &line : layout.lines) {
                QVERIFY2(metrics.horizontalAdvance(line) <= 56.5, qPrintable(title + QStringLiteral(": ") + line));
            }
        }
    }

    void toolTipShowsEveryKey()
    {
        const QString tip = panelToolTip(QStringLiteral("Annotate"), QStringLiteral("V A R E B M T N X S"));
        QVERIFY(tip.contains(QStringLiteral("<b>Annotate</b>")));
        QCOMPARE(tip.count(QStringLiteral("<code>")), 10);
        QVERIFY(!tip.contains(QChar(0x2026)));
        QVERIFY(!panelToolTip(QStringLiteral("More"), QString()).contains(QStringLiteral("<code>")));
        // Titles are escaped.
        QVERIFY(panelToolTip(QStringLiteral("A & <B>"), QString()).contains(QStringLiteral("A &amp; &lt;B&gt;")));
    }

    void panelKeysFollowTheState()
    {
        using A = PanelKeyAction;
        using S = PanelState;
        const auto action = [](S state, bool list, int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
            return panelKeyAction(state, list, key, mods).action;
        };
        // Start (Q11/Q24).
        QCOMPARE(action(S::Start, false, Qt::Key_1), A::ThisMonitor);
        QCOMPARE(action(S::Start, false, Qt::Key_2), A::Monitors);
        QCOMPARE(action(S::Start, false, Qt::Key_3), A::None);
        QCOMPARE(action(S::Start, false, Qt::Key_W), A::Window);
        QCOMPARE(action(S::Start, false, Qt::Key_G), A::Areas);
        QCOMPARE(action(S::Start, false, Qt::Key_D), A::Delay);
        QCOMPARE(action(S::Start, false, Qt::Key_F10), A::More);
        QCOMPARE(action(S::Start, false, Qt::Key_Menu), A::More);
        QCOMPARE(action(S::Start, false, Qt::Key_Comma, Qt::ControlModifier), A::Settings);
        QCOMPARE(action(S::Start, false, Qt::Key_C, Qt::ControlModifier), A::None); // nothing to copy yet
        QCOMPARE(action(S::Start, false, Qt::Key_B, Qt::ControlModifier), A::None);
        // Selection (Q12): drawers of the start row are gone.
        QCOMPARE(action(S::Selection, false, Qt::Key_C, Qt::ControlModifier), A::Copy);
        QCOMPARE(action(S::Selection, false, Qt::Key_S, Qt::ControlModifier | Qt::ShiftModifier), A::SaveAs);
        QCOMPARE(action(S::Selection, false, Qt::Key_B, Qt::ControlModifier), A::SaveAsArea);
        QCOMPARE(action(S::Selection, false, Qt::Key_F10), A::More);
        QCOMPARE(action(S::Selection, false, Qt::Key_1), A::None);
        QCOMPARE(action(S::Selection, false, Qt::Key_W), A::None);
        QCOMPARE(action(S::Selection, false, Qt::Key_D), A::None);
        // Area editing without Print (Q19).
        QCOMPARE(action(S::Area, false, Qt::Key_G), A::Areas);
        QCOMPARE(action(S::Area, false, Qt::Key_B, Qt::ControlModifier), A::SaveAsArea);
        QCOMPARE(action(S::Area, false, Qt::Key_F10), A::None);
        QCOMPARE(action(S::Area, false, Qt::Key_Comma, Qt::ControlModifier), A::None);
        // Tool letters, Ctrl+D and Ctrl+S stay with their own handlers.
        QCOMPARE(action(S::Selection, false, Qt::Key_S, Qt::ControlModifier), A::None);
        QCOMPARE(action(S::Selection, false, Qt::Key_D, Qt::ControlModifier), A::None);
        QCOMPARE(action(S::Start, false, Qt::Key_A), A::None);
        QCOMPARE(action(S::Start, false, Qt::Key_W, Qt::AltModifier), A::None);
    }

    void digitsPickEntriesInOpenListDrawers()
    {
        PanelKey key = panelKeyAction(PanelState::Start, true, Qt::Key_1, Qt::NoModifier);
        QCOMPARE(key.action, PanelKeyAction::PickEntry);
        QCOMPARE(key.entry, 0);
        key = panelKeyAction(PanelState::Area, true, Qt::Key_9, Qt::KeypadModifier);
        QCOMPARE(key.action, PanelKeyAction::PickEntry);
        QCOMPARE(key.entry, 8);
        QCOMPARE(panelKeyAction(PanelState::Start, true, Qt::Key_0, Qt::NoModifier).action, PanelKeyAction::None);
        QCOMPARE(panelKeyAction(PanelState::Start, true, Qt::Key_1, Qt::ControlModifier).action, PanelKeyAction::None);
    }

    void buttonHasFixedSizeAndAccessibleName()
    {
        PanelButton button(QStringLiteral("document-save"), QStringLiteral("Save"), QStringLiteral("Enter"), nullptr);
        QCOMPARE(button.size(), QSize(64, 64));
        QCOMPARE(button.accessibleName(), QStringLiteral("Save (Enter)"));
        button.setShortcutText(QString());
        QCOMPARE(button.accessibleName(), QStringLiteral("Save"));
        QSignalSpy clicked(&button, &QAbstractButton::clicked);
        button.click();
        QCOMPARE(clicked.count(), 1);
    }

    void keycapsGrowTheButtonForTheOverview()
    {
        PanelButton button(QStringLiteral("edit-copy"), QStringLiteral("Copy"), QStringLiteral("Ctrl+C"), nullptr);
        button.setKeycapVisible(true);
        QCOMPARE(button.size(), QSize(64, 64 + PanelButton::keycapHeight));
        button.setKeycapVisible(false);
        QCOMPARE(button.size(), QSize(64, 64));
        // Several keys go into the overview's table, not under the button.
        PanelButton annotate(QStringLiteral("draw-freehand"), QStringLiteral("Annotate"),
                             QStringLiteral("V A R E B M T N X S"), nullptr);
        QVERIFY(annotate.keycapText().isEmpty());
        annotate.setKeycapVisible(true);
        QCOMPARE(annotate.size(), QSize(64, 64 + PanelButton::keycapHeight)); // the row stays aligned
    }

    void buttonsWorkFromTheKeyboard()
    {
        // #74 (Q30): Tab reaches the buttons, Enter/Space click, the name and description carry the key.
        PanelButton button(QStringLiteral("document-save"), QStringLiteral("Save"), QStringLiteral("Enter"), nullptr);
        QCOMPARE(button.focusPolicy(), Qt::TabFocus);
        QCOMPARE(button.accessibleDescription(), QStringLiteral("Shortcut: Enter"));
        QSignalSpy clicked(&button, &QAbstractButton::clicked);
        QTest::keyClick(&button, Qt::Key_Return);
        QTest::keyClick(&button, Qt::Key_Space);
        QCOMPARE(clicked.count(), 2);
        // Keys the panel does not use stay with the overlay.
        QKeyEvent other(QEvent::KeyPress, Qt::Key_W, Qt::NoModifier);
        QCoreApplication::sendEvent(&button, &other);
        QVERIFY(!other.isAccepted());
        QKeyEvent shifted(QEvent::KeyPress, Qt::Key_Return, Qt::ShiftModifier);
        QCoreApplication::sendEvent(&button, &shifted);
        QVERIFY(!shifted.isAccepted());
        QCOMPARE(clicked.count(), 2);
    }

    void drawerGrowsInsideThePanel()
    {
        CapsulePanel panel(nullptr);
        for (int i = 0; i < 4; ++i) {
            panel.addMainButton(new PanelButton(QStringLiteral("edit-copy"), QStringLiteral("Copy"), QString(), &panel));
        }
        panel.adjustSize();
        const QSize closed = panel.size();
        QVERIFY(closed.height() >= 64);
        QVERIFY(!panel.drawerOpen());

        auto *content = new QLabel(QStringLiteral("drawer"));
        content->setFixedSize(500, 120);
        panel.setDrawer(content);
        QVERIFY(panel.drawerOpen());
        QVERIFY(panel.size().height() >= closed.height() + 120);
        QVERIFY(panel.size().width() > closed.width()); // a drawer may be wider than the main row

        panel.setDrawer(nullptr);
        QVERIFY(!panel.drawerOpen());
        QCOMPARE(panel.size(), closed);
    }

    // #96: arrow-key navigation through the panel and its drawers.
    void navigationFromNothingFocused()
    {
        PanelNavState s;
        s.mainOpensDrawer = {false, true, true, true, true, false, true, true, false}; // last = Close
        // No selection: Right/Down focus the first item, Left/Up the last.
        QCOMPARE(panelNavigate(s, Qt::Key_Right).action, PanelNavAction::FocusMain);
        QCOMPARE(panelNavigate(s, Qt::Key_Right).index, 0);
        QCOMPARE(panelNavigate(s, Qt::Key_Down).index, 0);
        QCOMPARE(panelNavigate(s, Qt::Key_Left).index, 8);
        QCOMPARE(panelNavigate(s, Qt::Key_Up).index, 8);
        QCOMPARE(panelNavigate(s, Qt::Key_Left, true).index, 0); // mirrored right to left
        // A selection exists: arrows stay with the selection.
        s.hasSelection = true;
        QCOMPARE(panelNavigate(s, Qt::Key_Right).action, PanelNavAction::Pass);
        QCOMPARE(panelNavigate(s, Qt::Key_Escape).action, PanelNavAction::Pass);
    }

    void navigationAlongThePanelDoesNotWrap()
    {
        PanelNavState s;
        s.mainOpensDrawer = {false, true, false};
        s.focus = PanelNavFocus::Main;
        s.mainIndex = 0;
        QCOMPARE(panelNavigate(s, Qt::Key_Right).action, PanelNavAction::FocusMain);
        QCOMPARE(panelNavigate(s, Qt::Key_Right).index, 1);
        QCOMPARE(panelNavigate(s, Qt::Key_Left).action, PanelNavAction::Swallow); // no wrap
        s.mainIndex = 2;
        QCOMPARE(panelNavigate(s, Qt::Key_Right).action, PanelNavAction::Swallow);
        QCOMPARE(panelNavigate(s, Qt::Key_Up).action, PanelNavAction::Swallow);
    }

    void downAndEnterOpenMenusOrTrigger()
    {
        PanelNavState s;
        s.mainOpensDrawer = {false, true};
        s.focus = PanelNavFocus::Main;
        s.mainIndex = 1;
        QCOMPARE(panelNavigate(s, Qt::Key_Down).action, PanelNavAction::OpenDrawer);
        QCOMPARE(panelNavigate(s, Qt::Key_Down).index, 1);
        QCOMPARE(panelNavigate(s, Qt::Key_Return).action, PanelNavAction::OpenDrawer);
        s.mainIndex = 0;
        QCOMPARE(panelNavigate(s, Qt::Key_Down).action, PanelNavAction::Swallow);
        QCOMPARE(panelNavigate(s, Qt::Key_Return).action, PanelNavAction::ClickMain);
        // An already open drawer of the focused item: Down goes into it.
        s.mainIndex = 1;
        s.drawerOwner = 1;
        s.entryCount = 3;
        QCOMPARE(panelNavigate(s, Qt::Key_Down).action, PanelNavAction::FocusEntry);
        QCOMPARE(panelNavigate(s, Qt::Key_Down).index, 0);
    }

    void menuEntriesUpDownLeftRight()
    {
        PanelNavState s;
        s.mainOpensDrawer = {false, true, true, false};
        s.focus = PanelNavFocus::Entry;
        s.drawerOwner = 1;
        s.entryCount = 3;
        s.entryIndex = 0;
        QCOMPARE(panelNavigate(s, Qt::Key_Down).action, PanelNavAction::FocusEntry);
        QCOMPARE(panelNavigate(s, Qt::Key_Down).index, 1);
        // Up on the top entry returns to the pill.
        QCOMPARE(panelNavigate(s, Qt::Key_Up).action, PanelNavAction::CloseDrawerFocusMain);
        QCOMPARE(panelNavigate(s, Qt::Key_Up).index, 1);
        s.entryIndex = 2;
        QCOMPARE(panelNavigate(s, Qt::Key_Down).action, PanelNavAction::Swallow); // no wrap
        QCOMPARE(panelNavigate(s, Qt::Key_Return).action, PanelNavAction::ClickEntry);
        // Right: the neighbour opens its menu; Left: the neighbour has none, the menu closes.
        QCOMPARE(panelNavigate(s, Qt::Key_Right).action, PanelNavAction::OpenDrawer);
        QCOMPARE(panelNavigate(s, Qt::Key_Right).index, 2);
        QCOMPARE(panelNavigate(s, Qt::Key_Left).action, PanelNavAction::CloseDrawerFocusMain);
        QCOMPARE(panelNavigate(s, Qt::Key_Left).index, 0);
    }

    void escapeLevels()
    {
        PanelNavState s;
        s.mainOpensDrawer = {false, true};
        s.focus = PanelNavFocus::Entry;
        s.drawerOwner = 1;
        s.entryCount = 2;
        s.entryIndex = 1;
        QCOMPARE(panelNavigate(s, Qt::Key_Escape).action, PanelNavAction::CloseDrawerFocusMain);
        s.focus = PanelNavFocus::Main;
        s.mainIndex = 1;
        QCOMPARE(panelNavigate(s, Qt::Key_Escape).action, PanelNavAction::CloseDrawer); // menu first
        s.drawerOwner = -1;
        QCOMPARE(panelNavigate(s, Qt::Key_Escape).action, PanelNavAction::CloseOverlay);
    }

    // #94: the mode cap shows what a click does, never the current state.
    void modeCapShowsTheNextState()
    {
        PanelModeCap cap(nullptr);
        cap.setTexts(QStringLiteral("Freeze"), QStringLiteral("Live"), QStringLiteral("Space"));
        cap.setFrozen(false); // live: a click freezes
        QCOMPARE(cap.actionTitle(), QStringLiteral("Freeze"));
        QCOMPARE(cap.actionIconName(), QStringLiteral("media-playback-pause"));
        QCOMPARE(cap.accessibleName(), QStringLiteral("Freeze"));
        cap.setFrozen(true); // frozen: a click goes live
        QCOMPARE(cap.actionTitle(), QStringLiteral("Live"));
        QCOMPARE(cap.actionIconName(), QStringLiteral("media-playback-start"));
        QCOMPARE(cap.accessibleName(), QStringLiteral("Live"));
        QVERIFY(cap.toolTipText().contains(QStringLiteral("Live")));
        QVERIFY(cap.toolTipText().contains(QStringLiteral("Space")));
    }

    // A nested setDrawer() while the old drawer hides crashed (#96 follow-up).
    void drawerSwitchSurvivesReentrancy()
    {
        CapsulePanel panel(nullptr);
        panel.show();
        panel.setDrawer(new ReentrantDrawer(&panel));
        // The nested request (close) comes last and wins; nothing is deleted twice.
        panel.setDrawer(new QLabel(QStringLiteral("next")));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!panel.drawerOpen());
        auto *again = new QLabel(QStringLiteral("again"));
        panel.setDrawer(again);
        QVERIFY(panel.drawer() == again);
    }

    void clearingTheMainRowRemovesButtons()
    {
        CapsulePanel panel(nullptr);
        panel.addMainButton(new PanelButton(QStringLiteral("edit-copy"), QStringLiteral("Copy"), QString(), &panel));
        panel.addGroupGap();
        panel.addMainButton(new PanelButton(QStringLiteral("window-close"), QStringLiteral("Close"), QString(), &panel));
        panel.adjustSize();
        const int withButtons = panel.width();
        panel.clearMainRow();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        panel.adjustSize();
        QVERIFY(panel.width() < withButtons);
        QCOMPARE(panel.findChildren<PanelButton *>().size(), 0);
    }
};

QTEST_MAIN(OverlayPanelTest)
#include "overlaypaneltest.moc"
