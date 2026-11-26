#include "overlaypanel.h"

#include <QAccessible>
#include <QApplication>
#include <QEnterEvent>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QTextLayout>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include <algorithm>
#include <functional>
#include <cmath>

namespace glimply
{

namespace
{

constexpr int hoverDurationMs = 120;
constexpr qreal fadedIconOpacity = 0.15;
constexpr int titleWidth = PanelButton::side - 8;
constexpr int capsuleShadow = 6;
constexpr int capsulePadding = 4;
constexpr int gripWidth = 12;
constexpr int groupGap = 10;
constexpr int capsuleAlpha = 224; // slightly translucent (Q31)

QFont titleFont(const QFont &base, qreal pixelSize)
{
    QFont font = base;
    // Point sizes keep the half-pixel steps; 0.75 pt per logical pixel at 96 dpi.
    font.setPointSizeF(pixelSize * 0.75);
    font.setWeight(QFont::DemiBold);
    return font;
}

struct WrappedLines
{
    QStringList lines;
    bool tooWide = false;
};

WrappedLines wrap(const QString &text, const QFont &font, int width)
{
    WrappedLines result;
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WordWrap);
    layout.setTextOption(option);
    layout.beginLayout();
    for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
        line.setLineWidth(width);
        if (line.naturalTextWidth() > width + 0.5) {
            result.tooWide = true;
        }
        result.lines.append(text.mid(line.textStart(), line.textLength()).trimmed());
    }
    layout.endLayout();
    return result;
}

bool animationsOff(const QWidget *widget)
{
    // Breeze honours KDE's animation speed here; "instant" gives 0 (Q30).
    return widget->style()->styleHint(QStyle::SH_Widget_Animation_Duration, nullptr, widget) <= 0;
}

// Keyboard use of a panel control (#74, Q30): Enter, Return and Space click it, the arrows move
// along the panel (mirrored right to left), Escape hands the keyboard back to the overlay. Other
// keys stay with the overlay's shortcuts.
bool panelControlKey(QAbstractButton *button, QKeyEvent *event, const std::function<void(bool)> &moveFocus)
{
    if (event->modifiers() & ~Qt::KeypadModifier) {
        return false;
    }
    const bool rtl = button->layoutDirection() == Qt::RightToLeft;
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        if (!event->isAutoRepeat()) {
            button->click();
        }
        return true;
    case Qt::Key_Right:
    case Qt::Key_Left:
        moveFocus((event->key() == Qt::Key_Right) != rtl);
        return true;
    case Qt::Key_Down:
    case Qt::Key_Up:
        moveFocus(event->key() == Qt::Key_Down);
        return true;
    case Qt::Key_Escape:
        button->clearFocus();
        button->window()->setFocus(Qt::OtherFocusReason);
        return true;
    default:
        return false;
    }
}

QPixmap blurred(const QPixmap &source)
{
    if (source.isNull()) {
        return source;
    }
    // Two down/up passes with smooth scaling approximate a small gaussian blur, enough for 32 px.
    QImage image = source.toImage();
    const QSize size = image.size();
    for (int pass = 0; pass < 2; ++pass) {
        image = image.scaled(std::max(1, size.width() / 4), std::max(1, size.height() / 4), Qt::IgnoreAspectRatio,
                             Qt::SmoothTransformation)
                    .scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    QPixmap pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(source.devicePixelRatio());
    return pixmap;
}

QColor withAlpha(QColor color, int alpha)
{
    color.setAlpha(alpha);
    return color;
}

} // namespace

PanelTitleLayout fitPanelTitle(const QString &title, const QFont &font, int width, int maxLines, qreal maxPixelSize,
                               qreal minPixelSize)
{
    PanelTitleLayout result;
    const QString text = title.simplified();
    for (qreal px = maxPixelSize; px >= minPixelSize - 0.01; px -= 0.5) {
        const WrappedLines wrapped = wrap(text, titleFont(font, px), width);
        if (!wrapped.tooWide && wrapped.lines.size() <= maxLines) {
            result.lines = wrapped.lines;
            result.pixelSize = px;
            result.shrunk = px < maxPixelSize;
            return result;
        }
    }
    // Even the smallest size does not fit: keep the words, elide what does not fit.
    const QFont smallest = titleFont(font, minPixelSize);
    const QFontMetricsF metrics(smallest);
    const WrappedLines wrapped = wrap(text, smallest, width);
    for (int i = 0; i < std::min<int>(maxLines, wrapped.lines.size()); ++i) {
        QString line = wrapped.lines.at(i);
        if (i == maxLines - 1 && wrapped.lines.size() > maxLines) {
            line = wrapped.lines.mid(i).join(QLatin1Char(' '));
        }
        result.lines.append(metrics.elidedText(line, Qt::ElideRight, width));
    }
    result.pixelSize = minPixelSize;
    result.shrunk = true;
    result.elided = true;
    return result;
}

QString panelToolTip(const QString &title, const QString &shortcut)
{
    QString html = QStringLiteral("<b>%1</b>").arg(title.toHtmlEscaped());
    const QStringList keys = shortcut.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (!keys.isEmpty()) {
        QStringList caps;
        for (const QString &key : keys) {
            caps.append(QStringLiteral("<code>&nbsp;%1&nbsp;</code>").arg(key.toHtmlEscaped()));
        }
        // Keys read left to right in every language.
        html += QStringLiteral("&nbsp;&nbsp;&#x2066;%1&#x2069;").arg(caps.join(QStringLiteral("&nbsp;")));
    }
    return html;
}

PanelKey panelKeyAction(PanelState state, bool listDrawerOpen, int key, Qt::KeyboardModifiers modifiers)
{
    const Qt::KeyboardModifiers mods = modifiers & ~Qt::KeypadModifier;
    const bool plain = mods == Qt::NoModifier;
    const bool ctrl = mods == Qt::ControlModifier;
    const bool print = state != PanelState::Area;

    if (plain && key >= Qt::Key_1 && key <= Qt::Key_9) {
        if (listDrawerOpen) {
            return {PanelKeyAction::PickEntry, key - Qt::Key_1};
        }
        if (state == PanelState::Start && key == Qt::Key_1) {
            return {PanelKeyAction::ThisMonitor};
        }
        if (state == PanelState::Start && key == Qt::Key_2) {
            return {PanelKeyAction::Monitors};
        }
        return {};
    }
    if (plain) {
        switch (key) {
        case Qt::Key_W:
            return {state == PanelState::Start ? PanelKeyAction::Window : PanelKeyAction::None};
        case Qt::Key_G:
            return {state != PanelState::Selection ? PanelKeyAction::Areas : PanelKeyAction::None};
        case Qt::Key_D:
            return {state == PanelState::Start ? PanelKeyAction::Delay : PanelKeyAction::None};
        case Qt::Key_F10:
        case Qt::Key_Menu:
            return {print ? PanelKeyAction::More : PanelKeyAction::None};
        default:
            return {};
        }
    }
    if (ctrl) {
        switch (key) {
        case Qt::Key_C:
            return {state == PanelState::Selection ? PanelKeyAction::Copy : PanelKeyAction::None};
        case Qt::Key_B:
            return {state != PanelState::Start ? PanelKeyAction::SaveAsArea : PanelKeyAction::None};
        case Qt::Key_Comma:
            return {print ? PanelKeyAction::Settings : PanelKeyAction::None};
        case Qt::Key_R:
            return {print ? PanelKeyAction::Record : PanelKeyAction::None};
        default:
            return {};
        }
    }
    if (mods == (Qt::ControlModifier | Qt::ShiftModifier) && key == Qt::Key_S && state == PanelState::Selection) {
        return {PanelKeyAction::SaveAs};
    }
    return {};
}

// ------------------------------------------------------------------------------------- PanelButton

PanelButton::PanelButton(const QString &iconName, const QString &title, const QString &shortcut, QWidget *parent)
    : QAbstractButton(parent)
    , iconName_(iconName)
    , title_(title)
    , shortcut_(shortcut)
{
    setFixedSize(side, side);
    // Tab reaches the panel (#74); without focus every key stays with the overlay's shortcuts.
    setFocusPolicy(Qt::TabFocus);
    setCursor(Qt::ArrowCursor);
    setAttribute(Qt::WA_Hover);
    animation_ = new QVariantAnimation(this);
    animation_->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        hover_ = value.toReal();
        update();
    });
    toolTipTimer_ = new QTimer(this);
    toolTipTimer_->setSingleShot(true);
    toolTipTimer_->setInterval(toolTipDelayMs);
    connect(toolTipTimer_, &QTimer::timeout, this, &PanelButton::showToolTipNow);
    updateAccessibility();
}

void PanelButton::setTitle(const QString &title)
{
    if (title_ != title) {
        title_ = title;
        updateAccessibility();
        update();
    }
}

QString PanelButton::title() const
{
    return title_;
}

void PanelButton::setShortcutText(const QString &shortcut)
{
    shortcut_ = shortcut;
    updateAccessibility();
}

QString PanelButton::shortcutText() const
{
    return shortcut_;
}

void PanelButton::setIconName(const QString &iconName)
{
    if (iconName_ != iconName) {
        iconName_ = iconName;
        iconCacheDpr_ = 0;
        update();
    }
}

void PanelButton::setGlyphText(const QString &text)
{
    glyph_ = text;
    update();
}

void PanelButton::setPrimary(bool primary)
{
    if (primary_ != primary) {
        primary_ = primary;
        update();
    }
}

bool PanelButton::isPrimary() const
{
    return primary_;
}

void PanelButton::setActive(bool active)
{
    if (active_ != active) {
        active_ = active;
        update();
    }
}

bool PanelButton::isActive() const
{
    return active_;
}

void PanelButton::setOpensDrawer(bool opens)
{
    opensDrawer_ = opens;
    update();
}

void PanelButton::setBadge(const QString &badge)
{
    if (badge_ != badge) {
        badge_ = badge;
        update();
    }
}

void PanelButton::setForcedHover(bool forced)
{
    if (forcedHover_ != forced) {
        forcedHover_ = forced;
        animateHover(forced || hovered_);
    }
}

void PanelButton::setKeycapVisible(bool visible)
{
    if (keycapVisible_ == visible) {
        return;
    }
    keycapVisible_ = visible;
    setFixedSize(sizeHint());
    update();
}

bool PanelButton::isKeycapVisible() const
{
    return keycapVisible_;
}

QString PanelButton::keycapText() const
{
    // Several keys ("V A R E B M T N X S") are listed in the overview's table instead.
    return shortcut_.contains(QLatin1Char(' ')) ? QString() : shortcut_;
}

QSize PanelButton::sizeHint() const
{
    // Every button of the overview grows, with or without a keycap, so the row stays aligned.
    return {side, keycapVisible_ ? side + keycapHeight : side};
}

bool PanelButton::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        return true; // shown by our own timer, after 500 ms
    }
    return QAbstractButton::event(event);
}

void PanelButton::enterEvent(QEnterEvent *event)
{
    for (QWidget *ancestor = parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
        if (auto *panel = qobject_cast<CapsulePanel *>(ancestor)) {
            Q_EMIT panel->buttonHovered();
            break;
        }
    }
    hovered_ = true;
    animateHover(true);
    if (!toolTipsSuppressed_) {
        toolTipTimer_->start();
    }
    QAbstractButton::enterEvent(event);
}

void PanelButton::leaveEvent(QEvent *event)
{
    hovered_ = false;
    animateHover(forcedHover_);
    toolTipTimer_->stop();
    QToolTip::hideText();
    QAbstractButton::leaveEvent(event);
}

void PanelButton::focusInEvent(QFocusEvent *event)
{
    // Keyboard focus looks like hover: title on the faded icon, the tooltip right away.
    animateHover(true);
    if (!toolTipsSuppressed_) {
        QToolTip::showText(mapToGlobal(QPoint(width() / 2, height() + 6)), panelToolTip(title_, shortcut_), this);
    }
    QAbstractButton::focusInEvent(event);
}

void PanelButton::focusOutEvent(QFocusEvent *event)
{
    animateHover(hovered_ || forcedHover_);
    if (!hovered_) {
        QToolTip::hideText();
    }
    QAbstractButton::focusOutEvent(event);
}

void PanelButton::keyPressEvent(QKeyEvent *event)
{
    if (!panelControlKey(this, event, [this](bool next) { focusNextPrevChild(next); })) {
        event->ignore();
    }
}

void PanelButton::keyReleaseEvent(QKeyEvent *event)
{
    event->ignore(); // clicks happen on press; never QAbstractButton's Space-release click
}

void PanelButton::mousePressEvent(QMouseEvent *event)
{
    toolTipTimer_->stop();
    QToolTip::hideText();
    QAbstractButton::mousePressEvent(event);
}

void PanelButton::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        iconCacheDpr_ = 0;
    }
    QAbstractButton::changeEvent(event);
}

void PanelButton::setToolTipsSuppressed(bool suppressed)
{
    toolTipsSuppressed_ = suppressed;
    if (suppressed) {
        toolTipTimer_->stop();
        if (QToolTip::isVisible() && (underMouse() || hasFocus())) {
            QToolTip::hideText();
        }
    }
}

bool PanelButton::toolTipsSuppressed() const
{
    return toolTipsSuppressed_;
}

void PanelButton::showToolTipNow()
{
    if (underMouse() && !title_.isEmpty() && !toolTipsSuppressed_) {
        QToolTip::showText(mapToGlobal(QPoint(width() / 2, height() + 6)), panelToolTip(title_, shortcut_), this);
    }
}

void PanelButton::animateHover(bool on)
{
    const qreal target = on ? 1.0 : 0.0;
    animation_->stop();
    if (animationsOff(this)) {
        hover_ = target;
        update();
        return;
    }
    animation_->setDuration(hoverDurationMs);
    animation_->setStartValue(hover_);
    animation_->setEndValue(target);
    animation_->start();
}

void PanelButton::updateAccessibility()
{
    setAccessibleName(shortcut_.isEmpty() ? title_ : tr("%1 (%2)").arg(title_, shortcut_));
    setAccessibleDescription(shortcut_.isEmpty() ? QString() : tr("Shortcut: %1").arg(shortcut_));
}

void PanelButton::rebuildIconCache()
{
    const qreal dpr = devicePixelRatioF();
    if (iconCacheDpr_ == dpr && iconCachePrimary_ == primary_) {
        return;
    }
    iconCacheDpr_ = dpr;
    iconCachePrimary_ = primary_;
    QIcon icon = QIcon::fromTheme(iconName_);
    if (icon.isNull() && iconName_.endsWith(QLatin1String("-symbolic"))) {
        icon = QIcon::fromTheme(iconName_.chopped(9)); // themes without symbolic variants
    }
    const QIcon::Mode mode = primary_ ? QIcon::Selected : QIcon::Normal;
    if (iconName_.endsWith(QLatin1String("-symbolic"))) {
        // Symbolic device icons only exist in small sizes; at 32 px the theme picks the coloured
        // device icon. Render the 24 px variant at the scale of a 32 px icon instead.
        constexpr int symbolicSide = 24;
        iconSharp_ = icon.pixmap(QSize(symbolicSide, symbolicSide), dpr * iconSide / symbolicSide, mode);
        iconSharp_.setDevicePixelRatio(dpr);
    } else {
        iconSharp_ = icon.pixmap(QSize(iconSide, iconSide), dpr, mode);
    }
    iconBlurred_ = blurred(iconSharp_);
}

void PanelButton::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    if (!isEnabled()) {
        painter.setOpacity(0.4);
    }
    const QPalette pal = palette();
    const QColor text = primary_ ? pal.color(QPalette::HighlightedText) : pal.color(QPalette::WindowText);
    const QRect face(0, 0, side, side); // the keycap row of the F1 overview sits below
    const QRectF circle = QRectF(face).adjusted(1, 1, -1, -1);

    if (primary_) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(pal.color(QPalette::Highlight));
        painter.drawEllipse(circle);
    }
    // The F1 overview shows every title at once; the hover disc stays for the pointer only.
    const qreal disc = forcedHover_ && !hovered_ ? 0.0 : hover_;
    if (disc > 0 || isDown()) {
        painter.setPen(Qt::NoPen);
        const int alpha = isDown() ? 40 : int(20 * disc);
        painter.setBrush(withAlpha(primary_ ? pal.color(QPalette::HighlightedText) : pal.color(QPalette::WindowText),
                                   alpha));
        painter.drawEllipse(circle);
    }

    // Icon (or glyph): sharp fades out, the blurred copy stays at about 15 %.
    const qreal sharpOpacity = 1.0 - hover_;
    const qreal blurOpacity = fadedIconOpacity * hover_;
    const QRect iconRect((side - iconSide) / 2, (side - iconSide) / 2, iconSide, iconSide);
    const qreal base = painter.opacity();
    if (glyph_.isEmpty()) {
        rebuildIconCache();
        if (sharpOpacity > 0) {
            painter.setOpacity(base * sharpOpacity);
            painter.drawPixmap(iconRect, iconSharp_);
        }
        if (blurOpacity > 0) {
            // KDE animation speed "instant": no fade (see animateHover) and no blur either (Q30).
            painter.setOpacity(base * blurOpacity);
            painter.drawPixmap(iconRect, animationsOff(this) ? iconSharp_ : iconBlurred_);
        }
    } else {
        QFont glyphFont = font();
        glyphFont.setPointSizeF(15);
        glyphFont.setWeight(QFont::Light);
        painter.setFont(glyphFont);
        painter.setPen(text);
        painter.setOpacity(base * (sharpOpacity + blurOpacity));
        painter.drawText(face, Qt::AlignCenter, glyph_);
    }
    painter.setOpacity(base);

    // Title on top of the faded icon.
    if (hover_ > 0 && !title_.isEmpty()) {
        const PanelTitleLayout layout = fitPanelTitle(title_, font(), titleWidth);
        const QFont font = titleFont(this->font(), layout.pixelSize);
        const QFontMetricsF metrics(font);
        const qreal lineHeight = metrics.height() * 0.95;
        const qreal total = lineHeight * layout.lines.size();
        qreal y = (side - total) / 2.0;
        painter.setFont(font);
        painter.setPen(text);
        painter.setOpacity(base * hover_);
        for (const QString &line : layout.lines) {
            painter.drawText(QRectF(4, y, side - 8, lineHeight), Qt::AlignCenter, line);
            y += lineHeight;
        }
        painter.setOpacity(base);
    }

    // Accent bar under the active button, or a dot for a closed drawer.
    if (active_) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(pal.color(QPalette::Highlight));
        painter.drawRoundedRect(QRectF(side / 2.0 - 8, side - 5, 16, 3), 1.5, 1.5);
    } else if (opensDrawer_) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(withAlpha(text, int(140 * (1.0 - hover_))));
        painter.drawEllipse(QPointF(side / 2.0, side - 6), 1.5, 1.5);
    }

    if (!badge_.isEmpty()) {
        QFont badgeFont = font();
        badgeFont.setPointSizeF(7.5);
        badgeFont.setWeight(QFont::Bold);
        const QFontMetricsF metrics(badgeFont);
        const qreal w = std::max(16.0, metrics.horizontalAdvance(badge_) + 8);
        const bool rtl = layoutDirection() == Qt::RightToLeft;
        const QRectF pill(rtl ? 4 : side - 4 - w, 4, w, 16);
        painter.setPen(Qt::NoPen);
        painter.setBrush(pal.color(QPalette::Highlight));
        painter.drawRoundedRect(pill, 8, 8);
        painter.setFont(badgeFont);
        painter.setPen(pal.color(QPalette::HighlightedText));
        painter.drawText(pill, Qt::AlignCenter, badge_);
    }

    // F1 overview: the shortcut as a keycap under the button (#72, Q15).
    const QString keycap = keycapText();
    if (keycapVisible_ && !keycap.isEmpty()) {
        QFont capFont = font();
        capFont.setPointSizeF(8);
        capFont.setWeight(QFont::DemiBold);
        const QFontMetricsF metrics(capFont);
        const qreal w = std::min<qreal>(side - 2, std::max(18.0, metrics.horizontalAdvance(keycap) + 10));
        const QRectF cap((side - w) / 2.0, side + 2, w, keycapHeight - 4);
        painter.setPen(withAlpha(pal.color(QPalette::WindowText), 90));
        painter.setBrush(withAlpha(pal.color(QPalette::WindowText), 22));
        painter.drawRoundedRect(cap.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        painter.setFont(capFont);
        painter.setPen(pal.color(QPalette::WindowText));
        painter.drawText(cap, Qt::AlignCenter, metrics.elidedText(keycap, Qt::ElideRight, w - 4));
    }
}

// ------------------------------------------------------------------------------------ PanelModeCap

PanelModeCap::PanelModeCap(QWidget *parent)
    : QAbstractButton(parent)
{
    setFixedSize(PanelButton::side, PanelButton::side);
    setFocusPolicy(Qt::TabFocus);
    setCursor(Qt::ArrowCursor);
    toolTipTimer_ = new QTimer(this);
    toolTipTimer_->setSingleShot(true);
    toolTipTimer_->setInterval(PanelButton::toolTipDelayMs);
    connect(toolTipTimer_, &QTimer::timeout, this, [this] {
        if (underMouse()) {
            QToolTip::showText(mapToGlobal(QPoint(width() / 2, height() + 6)),
                               toolTipText(), this);
        }
    });
}

void PanelModeCap::setFrozen(bool frozen)
{
    if (frozen_ != frozen) {
        frozen_ = frozen;
        setAccessibleName(actionTitle());
        update();
    }
}

void PanelModeCap::setTexts(const QString &freezeTitle, const QString &liveTitle, const QString &shortcut)
{
    freezeTitle_ = freezeTitle;
    liveTitle_ = liveTitle;
    shortcut_ = shortcut;
    setAccessibleName(actionTitle());
}

QString PanelModeCap::actionTitle() const
{
    return frozen_ ? liveTitle_ : freezeTitle_;
}

QString PanelModeCap::actionIconName() const
{
    return frozen_ ? QStringLiteral("media-playback-start") : QStringLiteral("media-playback-pause");
}

QString PanelModeCap::toolTipText() const
{
    return panelToolTip(actionTitle(), shortcut_);
}

QSize PanelModeCap::sizeHint() const
{
    return {PanelButton::side, PanelButton::side};
}

bool PanelModeCap::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        return true;
    }
    return QAbstractButton::event(event);
}

void PanelModeCap::enterEvent(QEnterEvent *event)
{
    hovered_ = true;
    toolTipTimer_->start();
    update();
    QAbstractButton::enterEvent(event);
}

void PanelModeCap::focusInEvent(QFocusEvent *event)
{
    QToolTip::showText(mapToGlobal(QPoint(width() / 2, height() + 6)),
                       toolTipText(), this);
    update();
    QAbstractButton::focusInEvent(event);
}

void PanelModeCap::focusOutEvent(QFocusEvent *event)
{
    if (!hovered_) {
        QToolTip::hideText();
    }
    update();
    QAbstractButton::focusOutEvent(event);
}

void PanelModeCap::keyPressEvent(QKeyEvent *event)
{
    if (!panelControlKey(this, event, [this](bool next) { focusNextPrevChild(next); })) {
        event->ignore();
    }
}

void PanelModeCap::keyReleaseEvent(QKeyEvent *event)
{
    event->ignore();
}

void PanelModeCap::leaveEvent(QEvent *event)
{
    hovered_ = false;
    toolTipTimer_->stop();
    QToolTip::hideText();
    update();
    QAbstractButton::leaveEvent(event);
}

void PanelModeCap::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled()) {
        painter.setOpacity(0.4);
    }
    const QPalette pal = palette();
    const QColor highlight = pal.color(QPalette::Highlight);
    const QRectF circle = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
    // At rest it weighs like its neighbours (#94): no fill, no ring. Hover and press fill it,
    // keyboard focus adds a full ring (#74).
    if (isDown() || hovered_ || hasFocus()) {
        painter.setBrush(withAlpha(highlight, isDown() ? 110 : 80));
        painter.setPen(hasFocus() ? QPen(highlight, 2) : Qt::NoPen);
        painter.drawEllipse(circle);
    }
    // The icon shows what a click does: pause freezes the live screen, play goes live again.
    const QIcon icon = QIcon::fromTheme(actionIconName());
    const int s = 22;
    icon.paint(&painter, QRect((width() - s) / 2, (height() - s) / 2, s, s));
}

// ------------------------------------------------------------------------------------ CapsulePanel

namespace
{

// The ⋮⋮ grip: only paints; the panel handles the mouse.
class GripDots : public QWidget
{
public:
    explicit GripDots(QWidget *parent)
        : QWidget(parent)
    {
        setFixedWidth(gripWidth);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(withAlpha(palette().color(QPalette::WindowText), 130));
        const qreal cx = width() / 2.0;
        const qreal cy = height() / 2.0;
        for (int row = -1; row <= 1; ++row) {
            for (const qreal dx : {-2.5, 2.5}) {
                painter.drawEllipse(QPointF(cx + dx, cy + row * 5.5), 1.3, 1.3);
            }
        }
    }
};

} // namespace

CapsulePanel::CapsulePanel(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, false);
    setCursor(Qt::ArrowCursor);
    auto *root = new QVBoxLayout(this);
    root->setSizeConstraint(QLayout::SetFixedSize);
    const int m = capsuleShadow + capsulePadding;
    root->setContentsMargins(m, m, m, m + 2);
    root->setSpacing(0);

    auto *top = new QHBoxLayout;
    top->setSpacing(0);
    modeCap_ = new PanelModeCap(this);
    top->addWidget(modeCap_);
    grip_ = new GripDots(this);
    top->addWidget(grip_);
    top->addSpacing(2);
    mainRow_ = new QHBoxLayout;
    mainRow_->setSpacing(0);
    top->addLayout(mainRow_);
    top->addSpacing(groupGap);
    root->addLayout(top);

    auto *drawerRow = new QHBoxLayout;
    drawerRow->setSpacing(0);
    drawerRow->addSpacing(PanelButton::side + gripWidth + 2);
    drawerBox_ = new QVBoxLayout;
    drawerBox_->setContentsMargins(0, 2, groupGap, 4);
    drawerRow->addLayout(drawerBox_);
    root->addLayout(drawerRow);
}

PanelModeCap *CapsulePanel::modeCap() const
{
    return modeCap_;
}

void CapsulePanel::clearMainRow()
{
    while (QLayoutItem *item = mainRow_->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
}

void CapsulePanel::addMainButton(PanelButton *button)
{
    button->setParent(this);
    mainRow_->addWidget(button);
    button->show();
}

void CapsulePanel::addGroupGap()
{
    mainRow_->addSpacing(groupGap);
}

void CapsulePanel::setDrawer(QWidget *content)
{
    if (drawer_ == content) {
        return;
    }
    if (drawer_) {
        drawerBox_->removeWidget(drawer_);
        drawer_->hide();
        drawer_->deleteLater();
    }
    drawer_ = content;
    if (content) {
        content->setParent(this);
        drawerBox_->addWidget(content, 0, Qt::AlignLeft); // leading edge, mirrored in RTL
        content->show();
    }
    adjustSize();
    update();
}

QWidget *CapsulePanel::drawer() const
{
    return drawer_;
}

bool CapsulePanel::drawerOpen() const
{
    return !drawer_.isNull();
}

bool CapsulePanel::onGrip(const QPoint &pos) const
{
    return grip_->geometry().adjusted(-4, 0, 4, 0).contains(pos);
}

void CapsulePanel::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF body = QRectF(rect()).adjusted(capsuleShadow, capsuleShadow, -capsuleShadow, -capsuleShadow - 2);
    const qreal radius = drawerOpen() ? 30.0 : body.height() / 2.0;
    for (int i = capsuleShadow; i > 0; --i) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 7));
        painter.drawRoundedRect(body.adjusted(-i, -i + 2, i, i + 2), radius + i, radius + i);
    }
    painter.setBrush(withAlpha(palette().color(QPalette::Window), capsuleAlpha));
    painter.setPen(QPen(withAlpha(palette().color(QPalette::WindowText), 32), 1));
    painter.drawRoundedRect(body.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
}

void CapsulePanel::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    dragging_ = true;
    grabOffset_ = event->position().toPoint();
    setCursor(Qt::ClosedHandCursor);
    Q_EMIT dragStarted();
    event->accept();
}

void CapsulePanel::mouseMoveEvent(QMouseEvent *event)
{
    if (dragging_) {
        Q_EMIT dragMoved(mapToParent(event->position().toPoint()), grabOffset_);
        event->accept();
        return;
    }
    setCursor(onGrip(event->position().toPoint()) ? Qt::OpenHandCursor : Qt::ArrowCursor);
    QWidget::mouseMoveEvent(event);
}

void CapsulePanel::mouseReleaseEvent(QMouseEvent *event)
{
    if (dragging_ && event->button() == Qt::LeftButton) {
        dragging_ = false;
        setCursor(onGrip(event->position().toPoint()) ? Qt::OpenHandCursor : Qt::ArrowCursor);
        Q_EMIT dragFinished(mapToParent(event->position().toPoint()));
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void CapsulePanel::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && onGrip(event->position().toPoint())) {
        Q_EMIT resetRequested();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

} // namespace glimply
