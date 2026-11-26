#pragma once

#include <QAbstractButton>
#include <QFont>
#include <QIcon>
#include <QPixmap>
#include <QPointer>
#include <QStringList>
#include <QWidget>

class QHBoxLayout;
class QTimer;
class QVariantAnimation;
class QVBoxLayout;

namespace glimply
{

// The title of an unlabelled panel button, laid out for its hover state (#69): at most maxLines
// lines inside width, the font shrinks from maxPixelSize to minPixelSize in half-pixel steps, and
// only when even the smallest size does not fit is the last line elided. Words are never split.
struct PanelTitleLayout
{
    QStringList lines;
    qreal pixelSize = 0;
    bool shrunk = false;
    bool elided = false;
};
[[nodiscard]] PanelTitleLayout fitPanelTitle(const QString &title, const QFont &font, int width, int maxLines = 3,
                                             qreal maxPixelSize = 11.0, qreal minPixelSize = 8.0);

// Rich text for a panel tooltip: the full title and the shortcut as keycaps. A shortcut with
// spaces ("V A R E B M T N X S") becomes one keycap per key, never an abbreviation.
[[nodiscard]] QString panelToolTip(const QString &title, const QString &shortcut);

// The panel's own keys (#70, Q24). Tool letters, Enter, Escape, Space, F1, I, P, O and Ctrl+E are
// handled elsewhere; this covers the keys that open drawers or run main-row and ⋯ entries.
enum class PanelState { Start, Selection, Area };
enum class PanelKeyAction {
    None,
    ThisMonitor,
    AllMonitors,
    Window,
    Areas,
    Delay,
    More,
    Copy,
    SaveAs,
    SaveAsArea,
    Settings,
    Record, // Ctrl+R (#30): opens the Record drawer, or records the selection
    PickEntry, // 1–9 in an open list drawer; entry is 0-based
};
struct PanelKey
{
    PanelKeyAction action = PanelKeyAction::None;
    int entry = -1;
};
[[nodiscard]] PanelKey panelKeyAction(PanelState state, bool listDrawerOpen, int key, Qt::KeyboardModifiers modifiers);

// A 64 × 64 button without a label (#69). On hover the icon fades to about 15 % and blurs, and the
// title shows on top of it; after 500 ms a tooltip adds the shortcut.
class PanelButton : public QAbstractButton
{
    Q_OBJECT

public:
    static constexpr int side = 64;
    static constexpr int iconSide = 32;
    static constexpr int toolTipDelayMs = 500;
    static constexpr int keycapHeight = 20;

    PanelButton(const QString &iconName, const QString &title, const QString &shortcut, QWidget *parent);

    void setTitle(const QString &title);
    [[nodiscard]] QString title() const;
    void setShortcutText(const QString &shortcut);
    [[nodiscard]] QString shortcutText() const;
    void setIconName(const QString &iconName);
    // A short text drawn instead of the icon, e.g. "5 s" in the Delay drawer.
    void setGlyphText(const QString &text);
    // Primary action of the current state: filled with the highlight colour.
    void setPrimary(bool primary);
    [[nodiscard]] bool isPrimary() const;
    // The button whose drawer is open, or a chosen entry in a drawer: an accent bar underneath.
    void setActive(bool active);
    [[nodiscard]] bool isActive() const;
    // Opens a drawer: a small dot at the bottom edge.
    void setOpensDrawer(bool opens);
    // A small counter or value in the corner, e.g. the chosen delay.
    void setBadge(const QString &badge);
    // Shows the hover state without the pointer (F1 overview, #72).
    void setForcedHover(bool forced);
    // F1 overview: the shortcut as a keycap below the button, which grows by keycapHeight. A
    // shortcut of several keys shows no keycap.
    void setKeycapVisible(bool visible);
    [[nodiscard]] bool isKeycapVisible() const;
    [[nodiscard]] QString keycapText() const;

    [[nodiscard]] QSize sizeHint() const override;

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void changeEvent(QEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

private:
    void animateHover(bool on);
    void updateAccessibility();
    void rebuildIconCache();
    void showToolTipNow();

    QString iconName_;
    QString title_;
    QString shortcut_;
    QString glyph_;
    QString badge_;
    bool primary_ = false;
    bool active_ = false;
    bool opensDrawer_ = false;
    bool forcedHover_ = false;
    bool keycapVisible_ = false;
    bool hovered_ = false;
    qreal hover_ = 0; // 0 = icon, 1 = title
    QVariantAnimation *animation_ = nullptr;
    QTimer *toolTipTimer_ = nullptr;
    QPixmap iconSharp_;
    QPixmap iconBlurred_;
    qreal iconCacheDpr_ = 0;
    bool iconCachePrimary_ = false;
};

// The round Freeze/Live end cap of the capsule (#68 direction C). Toggles on click and shows the
// action a click performs (#94): "Freeze" with a pause icon while live, "Live" with a play icon
// while frozen.
class PanelModeCap : public QAbstractButton
{
    Q_OBJECT

public:
    explicit PanelModeCap(QWidget *parent);
    void setFrozen(bool frozen);
    void setTexts(const QString &freezeTitle, const QString &liveTitle, const QString &shortcut);
    [[nodiscard]] QString actionTitle() const;
    [[nodiscard]] QString actionIconName() const;
    [[nodiscard]] QString toolTipText() const;
    [[nodiscard]] QSize sizeHint() const override;

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

private:
    bool frozen_ = true;
    bool hovered_ = false;
    QString freezeTitle_;
    QString liveTitle_;
    QString shortcut_;
    QTimer *toolTipTimer_ = nullptr;
};

// The one panel of a capture session (#69): a capsule with the Frozen/Live end cap, a grip and the
// main row; an open drawer grows below the main row inside the same capsule. Dragging the grip or
// the free capsule margin moves the panel; a double-click on the grip resets its position.
class CapsulePanel : public QWidget
{
    Q_OBJECT

public:
    explicit CapsulePanel(QWidget *parent);

    [[nodiscard]] PanelModeCap *modeCap() const;
    // Removes every button of the main row.
    void clearMainRow();
    void addMainButton(PanelButton *button);
    // Space between two groups of buttons; groups are separated by spacing only.
    void addGroupGap();
    // Replaces the drawer content (the panel owns it); nullptr closes the drawer.
    void setDrawer(QWidget *content);
    [[nodiscard]] QWidget *drawer() const;
    [[nodiscard]] bool drawerOpen() const;

Q_SIGNALS:
    void dragStarted();
    // Global logical position of the pointer while the panel is dragged.
    void dragMoved(const QPoint &globalPos, const QPoint &grabOffset);
    void dragFinished(const QPoint &globalPos);
    void resetRequested();
    // The pointer entered one of the panel's buttons (dismisses the first-start hint, #72).
    void buttonHovered();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    [[nodiscard]] bool onGrip(const QPoint &pos) const;

    PanelModeCap *modeCap_ = nullptr;
    QWidget *grip_ = nullptr;
    QHBoxLayout *mainRow_ = nullptr;
    QVBoxLayout *drawerBox_ = nullptr;
    QPointer<QWidget> drawer_;
    bool dragging_ = false;
    QPoint grabOffset_;
};

} // namespace glimply
