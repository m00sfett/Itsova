#include "captureoverlay_p.h"
#include "captureoverlay.h"

#include "logging.h"
#include "annotations/annotationrenderer.h"
#include "annotations/annotationtoolbar.h"
#include "annotations/inlinetexteditor.h"
#include "annotations/overlayexport.h"
#include "app.h"
#include "colorformat.h"
#include "exportdialog.h"
#include "imageoutput.h"
#include "latencytrace.h"
#include "overlaypanel.h"
#include "ocrengine.h"
#include "pixelloupe.h"
#include "screens.h"
#include "selectiongeometry.h"
#include "shortcutmanager.h"
#include "triggerplan.h"

#include <KKeySequenceWidget>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImageWriter>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>

namespace itsova
{
namespace
{

constexpr int minimumAreaSize = 4;
// Time the compositor gets to take the hidden overlays off the screen before a live capture.
constexpr int liveHideDelayMs = 250;
constexpr qreal handleRadius = 5.0;
constexpr int maxGrabDistance = 10;
constexpr int minGrabDistance = 4;
constexpr int cornerBonus = 4;
constexpr int detailMinimumSide = 40;
constexpr int closeButtonSize = 22;
// Distance of the panel from the monitor edge.
constexpr int panelMargin = 16;
constexpr int frozenDim = 140;
// Live: lighter, so a video underneath stays watchable.
constexpr int liveDim = 70;

// A floating panel in the window colours of the desktop theme, with a soft shadow.
class OverlayPanel : public QWidget
{
public:
    explicit OverlayPanel(QWidget *parent)
        : QWidget(parent)
    {
        setAutoFillBackground(false);
        setAttribute(Qt::WA_StyledBackground, false);
        setContentsMargins(shadow, shadow, shadow, shadow + 2);
        setCursor(Qt::ArrowCursor);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF body = QRectF(rect()).adjusted(shadow, shadow, -shadow, -shadow - 2);
        for (int i = shadow; i > 0; --i) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, 6));
            painter.drawRoundedRect(body.adjusted(-i, -i + 2, i, i + 2), radius + i, radius + i);
        }
        painter.setBrush(palette().color(QPalette::Window));
        QColor border = palette().color(QPalette::WindowText);
        border.setAlpha(45);
        painter.setPen(QPen(border, 1));
        painter.drawRoundedRect(body.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    }

private:
    static constexpr int shadow = 6;
    static constexpr qreal radius = 8;
};

QToolButton *barButton(QWidget *parent, const QString &icon, const QString &text, const QString &tip)
{
    auto *button = new QToolButton(parent);
    button->setIcon(QIcon::fromTheme(icon));
    button->setText(text);
    button->setToolTip(tip);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

// Outlines an area on screen while the mouse is over its row in the Areas list.
class HoverOutline : public QObject
{
public:
    HoverOutline(QObject *parent, CaptureSession *session, QList<CaptureSession::Highlight> outline)
        : QObject(parent)
        , session_(session)
        , outline_(std::move(outline))
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        // Keyboard focus on the row's button outlines it like the pointer does (#96).
        if (event->type() == QEvent::Enter || event->type() == QEvent::HoverEnter || event->type() == QEvent::FocusIn) {
            session_->setHighlights(outline_);
        } else if (event->type() == QEvent::Leave || event->type() == QEvent::HoverLeave
                   || event->type() == QEvent::FocusOut) {
            session_->setHighlights({});
        }
        return QObject::eventFilter(watched, event);
    }

private:
    CaptureSession *session_;
    QList<CaptureSession::Highlight> outline_;
};

// The rows of a list drawer (#95): section captions, numbered rows (keys 1–9), and the on-screen
// outline of what a row picks while it is hovered.
// The default-target pin of a record-mode entry (#124): faint while the row is hovered or has the
// keyboard, filled on the default; a click sets or clears the default (P does it from the keyboard).
// Not a button, so the arrow keys keep moving between the entries only.
class PinMark : public QWidget
{
public:
    PinMark(CaptureSession *session, QString targetId, QWidget *row, QAbstractButton *main)
        : QWidget(row)
        , session_(session)
        , targetId_(std::move(targetId))
    {
        setFixedSize(26, 26);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setToolTip(session->defaultRecordingTip());
        setAccessibleName(session->defaultRecordingTip());
        row->setAttribute(Qt::WA_Hover);
        row->installEventFilter(this);
        main->installEventFilter(this);
        main->setProperty(pinTargetProperty, targetId_);
        QObject::connect(session, &CaptureSession::defaultRecordingTargetChanged, this, [this] { update(); });
    }

    static constexpr const char *pinTargetProperty = kOverlayPinTargetProperty;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::Enter:
        case QEvent::HoverEnter:
            if (watched == parent()) {
                rowHovered_ = true;
                update();
            }
            break;
        case QEvent::Leave:
        case QEvent::HoverLeave:
            if (watched == parent()) {
                rowHovered_ = false;
                update();
            }
            break;
        case QEvent::FocusIn:
            focused_ = true;
            update();
            break;
        case QEvent::FocusOut:
            focused_ = false;
            update();
            break;
        default:
            break;
        }
        return false;
    }

    void paintEvent(QPaintEvent *) override
    {
        const bool pinned = session_->defaultRecordingTarget() == targetId_;
        const bool hovered = underMouse();
        if (!pinned && !rowHovered_ && !focused_ && !hovered) {
            return;
        }
        constexpr int iconSize = 16;
        const qreal dpr = devicePixelRatioF();
        QPixmap pixmap = QIcon::fromTheme(QStringLiteral("pin")).pixmap(QSize(iconSize, iconSize), dpr);
        QColor color = palette().color(pinned ? QPalette::Highlight : QPalette::WindowText);
        if (!pinned) {
            color.setAlphaF(hovered ? 0.85 : 0.4);
        }
        // One colour for the symbolic shape: the accent on the default, faint text colour otherwise.
        QPainter tint(&pixmap);
        tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tint.fillRect(pixmap.rect(), color);
        tint.end();
        QPainter painter(this);
        if (pinned || hovered) {
            QColor back = palette().color(QPalette::Highlight);
            back.setAlphaF(pinned ? 0.18 : 0.10);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(back);
            painter.drawRoundedRect(QRectF(rect()).adjusted(2, 2, -2, -2), 5, 5);
        }
        painter.drawPixmap((width() - iconSize) / 2, (height() - iconSize) / 2, pixmap);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            session_->toggleDefaultRecordingTarget(targetId_);
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

private:
    CaptureSession *session_;
    QString targetId_;
    bool rowHovered_ = false;
    bool focused_ = false;
};

class EntryList
{
public:
    EntryList(CaptureSession *session, QList<std::function<void()>> *entries)
        : session_(session)
        , entries_(entries)
        , content_(new QWidget)
        , column_(new QVBoxLayout(content_))
    {
        column_->setContentsMargins(0, 2, 0, 2);
        column_->setSpacing(0);
    }

    [[nodiscard]] QWidget *content() const { return content_; }
    [[nodiscard]] QVBoxLayout *column() const { return column_; }
    [[nodiscard]] bool isEmpty() const { return entries_->isEmpty(); }

    void caption(const QString &text)
    {
        auto *label = new QLabel(text, content_);
        QFont font = label->font();
        font.setPointSizeF(font.pointSizeF() * 0.9);
        label->setFont(font);
        QPalette pal = label->palette();
        QColor muted = pal.color(QPalette::WindowText);
        muted.setAlphaF(0.65);
        pal.setColor(QPalette::WindowText, muted);
        label->setPalette(pal);
        label->setContentsMargins(22, 8, 0, 2); // aligned with the row text, past the number column
        column_->addWidget(label);
    }

    // Returns the row's layout so a caller can add a second button.
    QHBoxLayout *addRow(const QString &icon, const QString &text, bool available,
                        const QList<CaptureSession::Highlight> &outline, const std::function<void()> &action,
                        QToolButton **mainOut = nullptr, const QString &pinTarget = QString())
    {
        auto *row = new QWidget(content_);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);
        layout->addWidget(entryNumberLabel(int(entries_->size()), row));
        auto *main = barButton(row, icon, text, QString());
        main->setEnabled(available);
        layout->addWidget(main);
        layout->addStretch(); // rows start at their number, not centred
        if (!outline.isEmpty()) {
            row->setAttribute(Qt::WA_Hover);
            auto *hover = new HoverOutline(row, session_, outline);
            row->installEventFilter(hover);
            main->installEventFilter(hover);
        }
        if (!pinTarget.isEmpty()) {
            layout->addWidget(new PinMark(session_, pinTarget, row, main));
        }
        QObject::connect(main, &QToolButton::clicked, row, action);
        entries_->append(available ? action : std::function<void()>());
        column_->addWidget(row);
        if (mainOut) {
            *mainOut = main;
        }
        return layout;
    }

private:
    static QLabel *entryNumberLabel(int index, QWidget *parent)
    {
        auto *label = new QLabel(index < 9 ? QString::number(index + 1) : QString(), parent);
        label->setAlignment(Qt::AlignCenter);
        label->setFixedWidth(20);
        QPalette pal = label->palette();
        QColor muted = pal.color(QPalette::WindowText);
        muted.setAlphaF(0.6);
        pal.setColor(QPalette::WindowText, muted);
        label->setPalette(pal);
        label->setAccessibleName(QString()); // the row's button carries the name
        return label;
    }

    CaptureSession *session_;
    QList<std::function<void()>> *entries_;
    QWidget *content_;
    QVBoxLayout *column_;
};

QString elided(const QString &text, int length)
{
    return text.size() <= length ? text : text.left(length - 1) + QStringLiteral("…");
}

} // namespace


void CaptureOverlay::buildPanel()
{
    panel_ = new CapsulePanel(this);
    panel_->installEventFilter(this);
    PanelModeCap *cap = panel_->modeCap();
    //: Fixed product term in every language: the action that freezes the live screen.
    const QString freeze = tr("Freeze");
    //: Fixed product term in every language: the action that returns to the live screen.
    const QString live = tr("Live");
    cap->setTexts(freeze, live, QStringLiteral("Space"));
    connect(cap, &QAbstractButton::clicked, this, [this] { session_->setFrozen(!session_->frozen()); });

    connect(panel_, &CapsulePanel::dragMoved, this, [this](const QPoint &pos, const QPoint &grabOffset) {
        panelDragging_ = true;
        panelGrabOffset_ = grabOffset;
        const QSize size = panel_->size();
        QPoint topLeft = pos - grabOffset;
        topLeft.setX(std::clamp(topLeft.x(), 0, std::max(0, width() - size.width())));
        topLeft.setY(std::clamp(topLeft.y(), 0, std::max(0, height() - size.height())));
        panel_->move(topLeft);
        placeHint();
    });
    connect(panel_, &CapsulePanel::dragFinished, this, [this](const QPoint &pos) {
        const bool moved = std::exchange(panelDragging_, false);
        if (!moved) {
            return;
        }
        const QPoint pointer = toGlobalPoint(pos);
        if (!screen_.geometry.contains(pointer)) {
            // Dropped on another monitor: the panel moves there, at the drop position (Q18).
            session_->movePanelToMonitor(pointer, pointer - panelGrabOffset_);
            return;
        }
        session_->setPanelPosition(screen_.connector, panel_->pos());
    });
    connect(panel_, &CapsulePanel::buttonHovered, this, [this] { session_->dismissFirstHint(); });
    connect(panel_, &CapsulePanel::resetRequested, this,
            [this] { session_->setPanelPosition(screen_.connector, std::nullopt); });

    auto *hintPanel = new OverlayPanel(this);
    hintPanel_ = hintPanel;
    auto *hintLayout = new QHBoxLayout(hintPanel);
    hintLayout->setContentsMargins(6, 0, 6, 0);
    hint_ = new QLabel(hintPanel);
    hint_->setAlignment(Qt::AlignCenter);
    hint_->setWordWrap(true);
    hint_->setMaximumWidth(720);
    QPalette hintPalette = hint_->palette();
    QColor muted = hintPalette.color(QPalette::WindowText);
    muted.setAlphaF(0.8);
    hintPalette.setColor(QPalette::WindowText, muted);
    hint_->setPalette(hintPalette);
    hintLayout->addWidget(hint_);
    hintPanel->hide();
    panel_->hide();
}

QString CaptureOverlay::panelState() const
{
    if (session_->purpose() != CaptureSession::Purpose::Print) {
        return QStringLiteral("area");
    }
    return session_->selections().isEmpty() ? QStringLiteral("start") : QStringLiteral("selection");
}

void CaptureOverlay::rebuildMainRow(const QString &state)
{
    using Drawer = CaptureSession::PanelDrawer;
    panel_->clearMainRow();
    drawerButtons_.clear();
    mainButtons_.clear();
    saveButton_ = nullptr;
    delayButton_ = nullptr;

    const auto add = [this](const QString &icon, const QString &title, const QString &shortcut,
                            const std::function<void()> &action) {
        auto *button = new PanelButton(icon, title, shortcut, panel_);
        connect(button, &QAbstractButton::clicked, this, action);
        button->installEventFilter(this); // arrow-key navigation (#96)
        panel_->addMainButton(button);
        mainButtons_.append(button);
        return button;
    };
    const auto drawer = [&](Drawer kind, const QString &icon, const QString &title, const QString &shortcut) {
        PanelButton *button = add(icon, title, shortcut, [this, kind] { session_->togglePanelDrawer(kind); });
        button->setOpensDrawer(true);
        drawerButtons_.insert(int(kind), button);
        return button;
    };
    const auto close = [&] {
        add(QStringLiteral("window-close"), tr("Close"), QStringLiteral("Esc"), [this] { session_->close(); });
    };
    // Every tool letter opens Markup; listed one by one, never abbreviated.
    const QString toolKeys = QStringLiteral("V A R E B M T N X S");

    if (state == QLatin1String("area")) {
        drawer(Drawer::Areas, QStringLiteral("select-rectangular"), tr("Areas"), QStringLiteral("G"));
        panel_->addGroupGap();
        add(QStringLiteral("list-add"), tr("Add Rectangle"), QString(), [this] { session_->setForceNew(true); });
        PanelButton *save = drawer(Drawer::SaveArea, QStringLiteral("bookmark-new"), tr("Save as Area"),
                                   QStringLiteral("Enter"));
        save->setOpensDrawer(false);
        save->setPrimary(true);
        saveButton_ = save;
        panel_->addGroupGap();
        close();
        return;
    }
    if (state == QLatin1String("selection") && session_->recordMode()) {
        // Opened to record (#30): recording the selection is the main action.
        PanelButton *record = add(QStringLiteral("media-record"), tr("Record"), QStringLiteral("Enter"),
                                  [this] { session_->recordSelection(); });
        record->setPrimary(true);
        // #124: the drawn rectangle as the default of the recording shortcut.
        PanelButton *pin = add(QStringLiteral("pin"), session_->defaultRecordingTip(), QStringLiteral("P"),
                               [this] { session_->toggleDefaultRecordingTarget(session_->selectionRecordingTargetId()); });
        const QPointer<PanelButton> pinButton = pin;
        const auto syncPin = [this, pinButton] {
            if (pinButton) {
                pinButton->setActive(!session_->defaultRecordingTarget().isEmpty()
                                     && session_->defaultRecordingTarget() == session_->selectionRecordingTargetId());
            }
        };
        syncPin();
        connect(session_, &CaptureSession::defaultRecordingTargetChanged, pin, syncPin);
        panel_->addGroupGap();
        drawer(Drawer::Record, QStringLiteral("view-list-details"), tr("Monitors, Windows and Areas"), QString());
        panel_->addGroupGap();
        close();
        return;
    }
    if (state == QLatin1String("selection")) {
        PanelButton *save =
            add(QStringLiteral("document-save"), tr("Save"), QStringLiteral("Enter"), [this] { session_->save(); });
        save->setPrimary(true);
        saveButton_ = save;
        add(QStringLiteral("edit-copy"), tr("Copy"), QStringLiteral("Ctrl+C"), [this] { session_->copy(); });
        add(QStringLiteral("window-pin"), tr("Pin"), QStringLiteral("P"), [this] { session_->pin(); });
        add(QStringLiteral("edit-select-text"), tr("Copy Text"), QStringLiteral("O"),
            [this] { session_->copyText(screen_.connector); });
        add(QStringLiteral("media-record"), tr("Record"), QStringLiteral("Ctrl+R"),
            [this] { session_->recordSelection(); });
        panel_->addGroupGap();
        drawer(Drawer::Markup, QStringLiteral("draw-freehand"), tr("Markup"), toolKeys);
        panel_->addGroupGap();
        drawer(Drawer::More, QStringLiteral("overflow-menu"), tr("More"), QStringLiteral("F10"));
        close();
        return;
    }
    // The targets in one order (#95): this monitor, the monitors, windows, areas. Opened to record
    // (#30), the same row records instead of capturing a picture.
    const bool record = session_->recordMode();
    add(QStringLiteral("monitor-symbolic"), tr("This Monitor"), QStringLiteral("1"), [this, record] {
        if (record) {
            session_->record(screenTargetId(screen_.connector));
        } else {
            session_->captureMonitor(screen_.connector);
        }
    });
    drawer(Drawer::Monitors, QStringLiteral("preferences-desktop-display-randr-symbolic"), tr("Monitors"),
           QStringLiteral("2"));
    drawer(Drawer::Window, QStringLiteral("window"), tr("Windows"), QStringLiteral("W"));
    drawer(Drawer::Areas, QStringLiteral("select-rectangular"), tr("Areas"), QStringLiteral("G"));
    panel_->addGroupGap();
    if (record) {
        delayButton_ = drawer(Drawer::Delay, QStringLiteral("chronometer"), tr("Delay"), QStringLiteral("D"));
        panel_->addGroupGap();
        close();
        return;
    }
    drawer(Drawer::Record, QStringLiteral("media-record"), tr("Record"), QStringLiteral("Ctrl+R"));
    panel_->addGroupGap();
    delayButton_ = drawer(Drawer::Delay, QStringLiteral("chronometer"), tr("Delay"), QStringLiteral("D"));
    drawer(Drawer::Markup, QStringLiteral("draw-freehand"), tr("Markup"), toolKeys);
    panel_->addGroupGap();
    drawer(Drawer::More, QStringLiteral("overflow-menu"), tr("More"), QStringLiteral("F10"));
    close();
}

QString CaptureOverlay::drawerSignature(CaptureSession::PanelDrawer drawer) const
{
    using Drawer = CaptureSession::PanelDrawer;
    QString signature = QString::number(int(drawer));
    switch (drawer) {
    case Drawer::Delay:
        signature += QStringLiteral(":%1:%2").arg(session_->captureDelaySeconds()).arg(session_->keepDelayActive());
        break;
    case Drawer::More:
        signature += QLatin1Char(':') + panelState() + QLatin1Char(':') + QString::number(session_->colorPickerActive())
                   + QLatin1Char(':') + QString::number(session_->frozen())
                   + QLatin1Char(':') + QString::number(session_->canSwitchToLive());
        break;
    case Drawer::Areas:
        signature += QLatin1Char(':') + panelState();
        break;
    default:
        break; // Markup and the area form keep their state; they are never rebuilt while open
    }
    return signature;
}

QWidget *CaptureOverlay::buildDrawer(CaptureSession::PanelDrawer drawer)
{
    using Drawer = CaptureSession::PanelDrawer;
    listEntries_.clear();
    switch (drawer) {
    case Drawer::None:
        return nullptr;
    case Drawer::Window:
        return buildWindowList();
    case Drawer::Areas:
        return buildAreaList();
    case Drawer::Delay:
        return buildDelayRow();
    case Drawer::Markup:
        return buildMarkupDrawer();
    case Drawer::More:
        return buildMoreRow();
    case Drawer::SaveArea:
        return buildAreaForm();
    case Drawer::Help:
        return buildHelpPanel(nullptr);
    case Drawer::Record:
        return buildRecordList();
    case Drawer::Monitors:
        return buildMonitorList();
    }
    return nullptr;
}

namespace
{

// A list drawer (windows, areas): grows with its rows up to a limit, then scrolls.
QWidget *listDrawer(QWidget *content)
{
    constexpr int maxHeight = 360;
    auto *scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setAutoFillBackground(false);
    scroll->viewport()->setAutoFillBackground(false);
    content->setAutoFillBackground(false);
    scroll->setWidget(content);
    const QSize hint = content->sizeHint();
    const bool scrolls = hint.height() > maxHeight;
    const int scrollbar = scrolls ? scroll->style()->pixelMetric(QStyle::PM_ScrollBarExtent) : 0;
    scroll->setFixedSize(std::min(hint.width() + scrollbar, 640), std::min(hint.height(), maxHeight));
    return scroll;
}

// The 1–9 key of a list drawer entry (Q24), in front of the row; later rows get an empty slot.
QLabel *entryNumber(int index, QWidget *parent)
{
    auto *label = new QLabel(index < 9 ? QString::number(index + 1) : QString(), parent);
    label->setAlignment(Qt::AlignCenter);
    label->setFixedWidth(20);
    QPalette pal = label->palette();
    QColor muted = pal.color(QPalette::WindowText);
    muted.setAlphaF(0.6);
    pal.setColor(QPalette::WindowText, muted);
    label->setPalette(pal);
    label->setAccessibleName(QString()); // the row's button carries the name
    return label;
}

QWidget *buttonRow(const QList<QWidget *> &buttons)
{
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    for (QWidget *button : buttons) {
        button->setParent(row);
        layout->addWidget(button);
    }
    layout->addStretch();
    return row;
}

} // namespace

// The Markup drawer (#71, Q22; named Markup since #100): ten tools flat as panel buttons, then the slim property row of
// the active tool with Undo/Redo. The drawer may be wider than the main row.
QWidget *CaptureOverlay::buildMarkupDrawer()
{
    struct Tool
    {
        ToolKind kind;
        QString icon;
        QString title;
        QString key;
    };
    const QList<Tool> tools{
        {ToolKind::Select, QStringLiteral("edit-select"), tr("Select"), QStringLiteral("V")},
        {ToolKind::Arrow, QStringLiteral("draw-arrow"), tr("Arrow"), QStringLiteral("A")},
        {ToolKind::Rectangle, QStringLiteral("draw-rectangle"), tr("Rectangle"), QStringLiteral("R")},
        {ToolKind::Ellipse, QStringLiteral("draw-ellipse"), tr("Ellipse"), QStringLiteral("E")},
        {ToolKind::Pen, QStringLiteral("draw-freehand"), tr("Pen"), QStringLiteral("B")},
        {ToolKind::Highlighter, QStringLiteral("draw-highlight"), tr("Marker"), QStringLiteral("M")},
        {ToolKind::Text, QStringLiteral("draw-text"), tr("Text"), QStringLiteral("T")},
        {ToolKind::Step, QStringLiteral("draw-number"), tr("Step"), QStringLiteral("N")},
        {ToolKind::Redact, QStringLiteral("view-hidden"), tr("Redact"), QStringLiteral("X")},
        {ToolKind::Spotlight, QStringLiteral("highlight-pointer-spot"), tr("Spotlight"), QStringLiteral("S")},
    };

    auto *box = new QWidget;
    auto *layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto *row = new QWidget(box);
    row->setObjectName(QStringLiteral("markupToolRow"));
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(0);
    QList<std::pair<ToolKind, QPointer<PanelButton>>> buttons;
    for (const Tool &tool : tools) {
        auto *button = new PanelButton(tool.icon, tool.title, tool.key, row);
        connect(button, &QAbstractButton::clicked, this, [this, kind = tool.kind] { session_->activateMarkupTool(kind); });
        rowLayout->addWidget(button);
        buttons.append({tool.kind, button});
    }
    AnnotationController *controller = &session_->annotationController();
    const auto syncActive = [buttons, controller] {
        for (const auto &[kind, button] : buttons) {
            if (button) {
                button->setActive(controller->tool() == kind);
            }
        }
    };
    syncActive();
    connect(controller, &AnnotationController::toolChanged, box, syncActive);
    layout->addWidget(row, 0, Qt::AlignLeft);

    auto *properties = new AnnotationToolbar(controller, box);
    properties->setPanelMode(true);
    properties->setSettings(&session_->store().annotations);
    connect(properties, &AnnotationToolbar::settingsModified, this, [this] { session_->persistAnnotationSettings(); });
    // One width for every tool: the tool row never shifts under the pointer while switching.
    properties->setMinimumWidth(std::max(properties->widestPropertyRowWidth(), row->sizeHint().width()));
    layout->addWidget(properties);
    return box;
}

// "Active Window" (#124): whichever window has the focus when the recording starts, so it can be
// the default of the recording shortcut.
static void addActiveWindowRow(EntryList &list, CaptureSession *session)
{
    const QString id = activeWindowRecordingTargetId();
    list.addRow(QStringLiteral("window-duplicate"), CaptureOverlay::tr("Active Window"), true, {},
                [session, id] { session->record(id); }, nullptr, id);
}

QWidget *CaptureOverlay::buildWindowList()
{
    const bool record = session_->recordMode();
    EntryList list(session_, &listEntries_);
    if (record) {
        addActiveWindowRow(list, session_);
    }
    for (const WindowInfo &window : session_->windows()) {
        if (record && window.minimized) {
            continue; // a minimized window has nothing to show
        }
        QString text = elided(window.caption.isEmpty() ? window.appName : window.caption, 70);
        if (window.minimized) {
            text = tr("%1 (minimized)").arg(text);
        }
        const QString icon = QIcon::hasThemeIcon(window.iconName) ? window.iconName : QStringLiteral("window");
        list.addRow(
            icon, text, true, {{window.geometry, window.caption}},
            [this, window, record] {
                if (record) {
                    session_->record(windowRecordingTargetId(window.handle));
                } else {
                    session_->captureWindow(window);
                }
            },
            nullptr, record ? windowRecordingTargetId(window.handle) : QString());
    }
    if (list.isEmpty() || (record && list.content()->findChildren<QToolButton *>().size() <= 1)) {
        list.column()->addWidget(new QLabel(tr("No windows found"), list.content()));
        return list.content();
    }
    return listDrawer(list.content());
}

namespace
{

QList<CaptureSession::Highlight> monitorOutline(CaptureSession *session, const ScreenInfo &screen)
{
    return {{screen.geometry, session->monitorName(screen.connector)}};
}

QList<CaptureSession::Highlight> allMonitorsOutline(CaptureSession *session)
{
    // Every monitor gets the frame a single monitor gets (#95).
    QList<CaptureSession::Highlight> outline;
    for (const ScreenInfo &screen : session->orderedScreens()) {
        outline += monitorOutline(session, screen);
    }
    return outline;
}

} // namespace

// The monitors (#95): each one by name, the panel's own first marked, then all of them. With a
// single monitor "All Monitors" would repeat it and is left out.
static void addMonitorRows(EntryList &list, CaptureSession *session, const QString &panelConnector,
                           const std::function<void(const QString &)> &pickMonitor, const std::function<void()> &pickAll,
                           bool pins = false)
{
    QList<ScreenInfo> screens = session->orderedScreens();
    std::stable_partition(screens.begin(), screens.end(),
                          [&](const ScreenInfo &screen) { return screen.connector == panelConnector; });
    for (const ScreenInfo &screen : std::as_const(screens)) {
        QString name = session->monitorName(screen.connector);
        if (screen.connector == panelConnector && screens.size() > 1) {
            name = CaptureOverlay::tr("%1 (this monitor)").arg(name);
        }
        const QString connector = screen.connector;
        list.addRow(QStringLiteral("monitor-symbolic"), elided(name, 70), true, monitorOutline(session, screen),
                    [pickMonitor, connector] { pickMonitor(connector); }, nullptr,
                    pins ? screenTargetId(connector) : QString());
    }
    if (screens.size() > 1) {
        list.addRow(QStringLiteral("view-fullscreen"), CaptureOverlay::tr("All Monitors"), true,
                    allMonitorsOutline(session), pickAll, nullptr, pins ? allMonitorsTargetId() : QString());
    }
}

QWidget *CaptureOverlay::buildMonitorList()
{
    const bool record = session_->recordMode();
    EntryList list(session_, &listEntries_);
    addMonitorRows(
        list, session_, session_->panelConnector(),
        [this, record](const QString &connector) {
            if (record) {
                session_->record(screenTargetId(connector));
            } else {
                session_->captureMonitor(connector);
            }
        },
        [this, record] {
            if (record) {
                session_->record(allMonitorsTargetId());
            } else {
                session_->captureAllMonitors();
            }
        },
        record);
    return listDrawer(list.content());
}

// The Record drawer (#30) mirrors the panel (#95): monitors, windows, areas, in that order.
QWidget *CaptureOverlay::buildRecordList()
{
    EntryList list(session_, &listEntries_);
    const TargetStore &store = session_->store();

    list.caption(tr("Monitors"));
    addMonitorRows(
        list, session_, session_->panelConnector(),
        [this](const QString &connector) { session_->record(screenTargetId(connector)); },
        [this] { session_->record(allMonitorsTargetId()); }, true);

    list.caption(tr("Windows"));
    addActiveWindowRow(list, session_);
    for (const WindowInfo &window : session_->windows()) {
        if (window.minimized) {
            continue; // a minimized window has nothing to show
        }
        const QString icon = QIcon::hasThemeIcon(window.iconName) ? window.iconName : QStringLiteral("window");
        const QString targetId = windowRecordingTargetId(window.handle);
        list.addRow(icon, elided(window.caption.isEmpty() ? window.appName : window.caption, 70), true,
                    {{window.geometry, window.caption}}, [this, targetId] { session_->record(targetId); }, nullptr,
                    targetId);
    }

    bool areaCaption = false;
    for (const Area &area : store.areas) {
        if (!areaCaption) {
            list.caption(tr("Areas"));
            areaCaption = true;
        }
        QList<CaptureSession::Highlight> outline;
        bool available = false;
        if (area.isCrossMonitor()) {
            outline.append({area.rect, area.name});
            available = std::any_of(area.monitors.cbegin(), area.monitors.cend(),
                                    [&](const AreaMonitorBinding &b) { return session_->isConnected(b.connector); });
        } else if (const QScreen *screen = findScreen(area.connector)) {
            outline.append({area.rect.translated(screen->geometry().topLeft()), area.name});
            available = session_->isConnected(area.connector);
        }
        const QString id = area.id;
        list.addRow(QStringLiteral("select-rectangular"), elided(area.name, 70), available, outline,
                    [this, id] { session_->record(id); }, nullptr, id);
    }
    return listDrawer(list.content());
}

QWidget *CaptureOverlay::buildAreaList()
{
    auto *content = new QWidget;
    auto *column = new QVBoxLayout(content);
    column->setContentsMargins(0, 2, 0, 2);
    column->setSpacing(0);
    const bool record = session_->recordMode(); // #95: the Areas drawer records in record mode
    const bool print = session_->purpose() == CaptureSession::Purpose::Print && !record;
    const TargetStore &store = session_->store();
    if (store.areas.isEmpty() && (record || store.groups.isEmpty())) {
        auto *empty = new QLabel(tr("No saved areas yet. Draw a rectangle, then choose Save as Area."), content);
        empty->setWordWrap(true);
        empty->setMaximumWidth(360);
        column->addWidget(empty);
        return content;
    }

    const auto addRow = [&](const QString &id, const QString &icon, const QString &name, bool available,
                            const QList<CaptureSession::Highlight> &outline) {
        auto *row = new QWidget(content);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);
        layout->addWidget(entryNumber(int(listEntries_.size()), row));
        QString label = name;
        if (const QKeySequence shortcut = session_->shortcut(id); !shortcut.isEmpty()) {
            label = tr("%1    %2").arg(name, shortcut.toString(QKeySequence::NativeText));
        }
        auto *main = barButton(row, icon, label, record ? QString() : print ? tr("Capture now") : tr("Add as a rectangle"));
        main->setEnabled(available);
        layout->addWidget(main);
        layout->addStretch();
        if (record) {
            layout->addWidget(new PinMark(session_, id, row, main)); // #124
        }
        const auto pick = [this, id, print, record] {
            if (record) {
                session_->record(id);
            } else if (print) {
                session_->captureTarget(id);
            } else {
                session_->setPanelDrawer(CaptureSession::PanelDrawer::None);
                session_->addTarget(id);
            }
        };
        connect(main, &QToolButton::clicked, this, pick);
        listEntries_.append(available ? std::function<void()>(pick) : std::function<void()>());
        if (print) {
            auto *add = barButton(row, QStringLiteral("list-add"), QString(), tr("Add as a rectangle to adjust it"));
            add->setToolButtonStyle(Qt::ToolButtonIconOnly);
            add->setAccessibleName(tr("Add %1 as a rectangle").arg(name));
            add->setEnabled(available);
            layout->addWidget(add);
            connect(add, &QToolButton::clicked, this, [this, id] {
                session_->setPanelDrawer(CaptureSession::PanelDrawer::None);
                session_->addTarget(id);
            });
        }
        // Hovering a row outlines the area on screen.
        row->setAttribute(Qt::WA_Hover);
        auto *hover = new HoverOutline(row, session_, outline);
        row->installEventFilter(hover);
        main->installEventFilter(hover);
        column->addWidget(row);
    };

    const auto outlineOf = [&](const QString &areaId) -> QList<CaptureSession::Highlight> {
        const Area *area = store.area(areaId);
        if (!area) {
            return {};
        }
        if (area->isCrossMonitor()) {
            return {{area->rect, area->name}};
        }
        const QScreen *screen = findScreen(area->connector);
        if (!screen) {
            return {};
        }
        return {{area->rect.translated(screen->geometry().topLeft()), area->name}};
    };

    const auto areaAvailable = [&](const Area &area) {
        if (area.isCrossMonitor()) {
            return std::any_of(area.monitors.cbegin(), area.monitors.cend(),
                               [&](const AreaMonitorBinding &b) { return session_->isConnected(b.connector); });
        }
        return session_->isConnected(area.connector);
    };

    for (const Group &group : store.groups) {
        if (record) {
            break; // a group is several captures; a recording has one target
        }
        QList<CaptureSession::Highlight> outline;
        bool available = false;
        for (const QString &member : group.targetIds) {
            outline += outlineOf(member);
            if (const Area *area = store.area(member); area && areaAvailable(*area)) {
                available = true;
            }
        }
        addRow(group.id, QStringLiteral("object-group"), group.name, available, outline);
    }
    if (!record && !store.groups.isEmpty() && !store.areas.isEmpty()) {
        auto *line = new QFrame(content);
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);
        column->addWidget(line);
    }
    for (const Area &area : store.areas) {
        addRow(area.id, QStringLiteral("select-rectangular"), area.name, areaAvailable(area), outlineOf(area.id));
    }
    return listDrawer(content);
}

QWidget *CaptureOverlay::buildDelayRow()
{
    const int currentDelay = session_->captureDelaySeconds();
    const bool keepActive = session_->keepDelayActive();
    const auto retake = [this, keepActive](int seconds) {
        session_->setPanelDrawer(CaptureSession::PanelDrawer::None);
        if (keepActive) {
            session_->setCaptureDelay(seconds, true);
        }
        session_->retakeWithDelay(seconds);
    };

    QList<QWidget *> buttons;
    for (const int preset : {3, 5, 10}) {
        auto *button = new PanelButton(QString(), tr("Retake in %1 s").arg(preset), QString(), nullptr);
        button->setGlyphText(tr("%1 s").arg(preset));
        button->setActive(keepActive && currentDelay == preset);
        connect(button, &QAbstractButton::clicked, this, [retake, preset] { retake(preset); });
        buttons.append(button);
    }
    auto *custom = new PanelButton(QStringLiteral("chronometer"), tr("Retake in custom…"), QString(), nullptr);
    connect(custom, &QAbstractButton::clicked, this, [this, currentDelay, retake] {
        bool ok = false;
        const int chosen = QInputDialog::getInt(this, tr("Retake Delay"), tr("Seconds:"),
                                              currentDelay > 0 ? currentDelay : 3, 1, 60, 1, &ok);
        if (ok) {
            retake(chosen);
        }
    });
    buttons.append(custom);
    auto *keep = new PanelButton(QStringLiteral("view-refresh"), tr("Keep delay active"), QString(), nullptr);
    keep->setActive(keepActive);
    connect(keep, &QAbstractButton::clicked, this, [this, currentDelay, keepActive] {
        session_->setCaptureDelay(!keepActive ? (currentDelay > 0 ? currentDelay : 3) : 0, !keepActive);
    });
    buttons.append(keep);
    return buttonRow(buttons);
}

QWidget *CaptureOverlay::buildMoreRow()
{
    using Drawer = CaptureSession::PanelDrawer;
    const auto button = [this](const QString &icon, const QString &title, const QString &shortcut,
                               const std::function<void()> &action) {
        auto *b = new PanelButton(icon, title, shortcut, nullptr);
        connect(b, &QAbstractButton::clicked, this, action);
        return static_cast<QWidget *>(b);
    };
    auto *pick = static_cast<PanelButton *>(
        button(QStringLiteral("color-picker"), tr("Pick Color"), QStringLiteral("I"), [this] {
            session_->setPickerCursorGlobal(lastPosition_);
            session_->setColorPickerActive(!session_->colorPickerActive());
        }));
    pick->setActive(session_->colorPickerActive());
    QWidget *edit = button(QStringLiteral("document-edit"), tr("Edit in Window"), QStringLiteral("Ctrl+E"),
                           [this] { session_->editInWindow(screen_.connector); });
    if (panelState() == QLatin1String("selection")) {
        return buttonRow({
            button(QStringLiteral("document-save-as"), tr("Save As…"), QStringLiteral("Ctrl+Shift+S"),
                   [this] { session_->saveAs(); }),
            button(QStringLiteral("bookmark-new"), tr("Save as Area"), QStringLiteral("Ctrl+B"),
                   [this] { session_->showAreaPanel(); }),
            button(QStringLiteral("list-add"), tr("Add Rectangle"), QString(),
                   [this] {
                       session_->setPanelDrawer(Drawer::None);
                       session_->setForceNew(true);
                   }),
            edit,
            pick,
        });
    }
    // Freeze/Live as an entry too (Q11); like the end cap it names the action (#94).
    const bool frozen = session_->frozen();
    auto *mode = static_cast<PanelButton *>(
        button(frozen ? QStringLiteral("media-playback-start") : QStringLiteral("media-playback-pause"),
               frozen ? tr("Live") : tr("Freeze"), QStringLiteral("Space"),
               [this] { session_->setFrozen(!session_->frozen()); }));
    mode->setEnabled(!frozen || session_->canSwitchToLive());
    return buttonRow({
        mode,
        pick,
        edit,
        button(QStringLiteral("configure"), tr("Settings"), QStringLiteral("Ctrl+,"), [this] { session_->openSettings(); }),
        button(QStringLiteral("help-contextual"), tr("Shortcut Overview"), QStringLiteral("F1"),
               [this] { session_->toggleHelp(); }),
    });
}

QWidget *CaptureOverlay::buildAreaForm()
{
    SelectionModel &model = session_->selections();
    const int count = model.count();
    const TargetStore &store = session_->store();

    auto *form = new QWidget;
    auto *layout = new QFormLayout(form);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto *title = new QLabel(form);
    QFont bold = title->font();
    bold.setBold(true);
    title->setFont(bold);
    layout->addRow(title);

    auto *combine = new QCheckBox(tr("Combine into a group"), form);
    combine->setChecked(true);
    combine->setVisible(count > 1);
    if (count > 1) {
        layout->addRow(combine);
    }

    auto *name = new QLineEdit(form);
    name->setMinimumWidth(260);
    if (count == 1) {
        const QList<ScreenInfo> hit = session_->intersectingScreens(model.inDrawingOrder().first().rect);
        const QString prefix = hit.size() == 1 ? store.monitorName(hit.first()) : QStringLiteral("area");
        name->setText(store.nextAreaName(prefix));
    } else {
        int n = 1;
        while (!store.isNameAvailable(tr("Group %1").arg(n))) {
            ++n;
        }
        name->setText(tr("Group %1").arg(n));
    }
    name->selectAll();
    layout->addRow(tr("Name:"), name);

    auto *shortcut = new KKeySequenceWidget(form);
    shortcut->setCheckForConflictsAgainst(KKeySequenceWidget::GlobalShortcuts | KKeySequenceWidget::StandardShortcuts);
    layout->addRow(tr("Shortcut:"), shortcut);

    auto *delayCombo = new QComboBox(form);
    delayCombo->setAccessibleName(tr("Area capture delay"));
    delayCombo->addItem(tr("None"), 0);
    delayCombo->addItem(tr("%n seconds", nullptr, 3), 3);
    delayCombo->addItem(tr("%n seconds", nullptr, 5), 5);
    delayCombo->addItem(tr("%n seconds", nullptr, 10), 10);
    delayCombo->addItem(tr("Custom…"), -1);
    layout->addRow(tr("Delay:"), delayCombo);
    auto lastDelayIdx = std::make_shared<int>(0);
    connect(delayCombo, &QComboBox::currentIndexChanged, form, [form, delayCombo, lastDelayIdx](int idx) {
        if (idx < 0) {
            return;
        }
        const int data = delayCombo->itemData(idx).toInt();
        if (data >= 0) {
            *lastDelayIdx = idx;
            return;
        }
        bool ok = false;
        const int prev = std::max(1, delayCombo->itemData(*lastDelayIdx).toInt());
        const int chosen = QInputDialog::getInt(form, tr("Area Delay"), tr("Seconds:"), prev, 1, 60, 1, &ok);
        if (!ok) {
            const QSignalBlocker block(delayCombo);
            delayCombo->setCurrentIndex(*lastDelayIdx);
            return;
        }
        const int existing = delayCombo->findData(chosen);
        if (existing >= 0) {
            const QSignalBlocker block(delayCombo);
            delayCombo->setCurrentIndex(existing);
            *lastDelayIdx = existing;
            return;
        }
        const QSignalBlocker block(delayCombo);
        const int insertAt = delayCombo->count() - 1;
        delayCombo->insertItem(insertAt, tr("%n seconds", nullptr, chosen), chosen);
        delayCombo->setCurrentIndex(insertAt);
        *lastDelayIdx = insertAt;
    });

    auto *explain = new QLabel(form);
    explain->setWordWrap(true);
    explain->setMaximumWidth(380);
    layout->addRow(QString(), explain);

    auto *error = new QLabel(form);
    error->setWordWrap(true);
    error->setMaximumWidth(380);
    error->hide();
    layout->addRow(QString(), error);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    auto *back = new QPushButton(QIcon::fromTheme(QStringLiteral("go-previous")), tr("Back"), form);
    auto *confirm = new QPushButton(QIcon::fromTheme(QStringLiteral("document-save")), QString(), form);
    confirm->setDefault(true);
    buttons->addWidget(back);
    buttons->addWidget(confirm);
    layout->addRow(buttons);

    const auto update = [=] {
        const bool group = count > 1 && combine->isChecked();
        const bool named = count == 1 || group;
        name->setEnabled(named);
        shortcut->setEnabled(named);
        if (count == 1) {
            title->setText(tr("Save as Area"));
            explain->setText(tr("The shortcut is optional and works in every application. You can also capture the "
                                "area from the Itsova widget."));
            confirm->setText(tr("Save Area"));
        } else if (group) {
            title->setText(tr("Save %n areas as a group", nullptr, count));
            explain->setText(tr("One shortcut captures all %n areas at the same moment, each into its own file.",
                                nullptr, count));
            confirm->setText(tr("Save Group"));
        } else {
            title->setText(tr("Save %n areas", nullptr, count));
            explain->setText(tr("Each rectangle becomes its own area with an automatic name. You can rename them and "
                                "give them shortcuts later in the widget or the settings."));
            confirm->setText(tr("Save %n Areas", nullptr, count));
        }
    };
    update();
    connect(combine, &QCheckBox::toggled, form, update);

    const auto submit = [this, name, shortcut, delayCombo, combine, error] {
        const int chosenDelay = std::max(0, delayCombo->currentData().toInt());
        const QString problem =
            session_->saveAsAreas(name->text(), shortcut->keySequence(), combine->isChecked(), chosenDelay);
        if (!problem.isEmpty()) {
            error->setText(problem);
            error->show();
            name->setFocus();
            panel_->adjustSize();
            refreshPanel();
        }
    };
    connect(confirm, &QPushButton::clicked, form, submit);
    connect(name, &QLineEdit::returnPressed, form, submit);
    connect(back, &QPushButton::clicked, this, [this] {
        session_->setPanelDrawer(CaptureSession::PanelDrawer::None);
        setFocus();
    });

    QTimer::singleShot(0, name, [this, name] {
        activateWindow();
        name->setFocus();
    });
    return form;
}

void CaptureOverlay::refreshPanel()
{
    using Drawer = CaptureSession::PanelDrawer;
    if (!panel_) {
        return;
    }
    if (session_->panelConnector() != screen_.connector) {
        if (panel_->drawerOpen()) {
            panel_->setDrawer(nullptr);
        }
        builtDrawer_ = Drawer::None;
        builtDrawerSignature_.clear();
        panel_->hide();
        hintPanel_->hide();
        return;
    }

    const QString state = panelState();
    if (state != builtState_) {
        rebuildMainRow(state);
        builtState_ = state;
    }
    const Drawer drawer = session_->panelDrawer();
    const QString signature = drawerSignature(drawer);
    if (drawer != builtDrawer_ || signature != builtDrawerSignature_) {
        panel_->setDrawer(buildDrawer(drawer));
        builtDrawer_ = drawer;
        builtDrawerSignature_ = signature;
        if (QWidget *content = panel_->drawer()) {
            for (QAbstractButton *entry : content->findChildren<QAbstractButton *>()) {
                entry->installEventFilter(this); // arrow-key navigation (#96)
            }
        }
    }

    PanelModeCap *cap = panel_->modeCap();
    cap->setFrozen(session_->frozen());
    cap->setEnabled(!session_->frozen() || session_->canSwitchToLive());
    for (auto it = drawerButtons_.cbegin(); it != drawerButtons_.cend(); ++it) {
        if (it.value()) {
            it.value()->setActive(it.key() == int(drawer));
        }
    }
    // F1 overview (#72, Q15): every button in its hover state at once, keys as keycaps below.
    const bool overview = drawer == Drawer::Help;
    const bool menuOpen = drawer != Drawer::None && !overview;
    for (const QPointer<PanelButton> &button : std::as_const(mainButtons_)) {
        if (button) {
            button->setForcedHover(overview);
            button->setKeycapVisible(overview); // an open menu ends the overview, so no keycaps then
            button->setToolTipsSuppressed(menuOpen);
        }
    }
    if (saveButton_ && state == QLatin1String("selection")) {
        const int count = session_->selections().count();
        saveButton_->setTitle(count > 1 ? tr("Save %n Areas", nullptr, count) : tr("Save"));
    }
    if (saveButton_ && state == QLatin1String("area")) {
        saveButton_->setEnabled(!session_->selections().isEmpty());
    }
    if (delayButton_) {
        const int delay = session_->captureDelaySeconds();
        delayButton_->setBadge(delay > 0 ? tr("%1 s").arg(delay) : QString());
    }

    panel_->adjustSize();
    placePanel();
    panel_->show();
    panel_->raise();
    placeHint();
}

void CaptureOverlay::placePanel()
{
    if (!panel_ || panelDragging_ || session_->isDragging() || drag_ != Drag::None) {
        return;
    }
    QRect covered;
    for (const Selection &selection : session_->selections().stack()) {
        covered = covered.united(toLocalRect(selection.rect).intersected(rect()));
    }
    panel_->move(
        placeOverlayPanel(panel_->size(), size(), covered, session_->panelPosition(screen_.connector), panelMargin));
}

bool CaptureOverlay::eventFilter(QObject *watched, QEvent *event)
{
    if (auto *button = qobject_cast<QAbstractButton *>(watched); button && panel_ && panel_->isAncestorOf(button)) {
        if (event->type() == QEvent::KeyPress && navigatePanel(static_cast<QKeyEvent *>(event))) {
            return true;
        }
        // A drawer entry with keyboard focus looks hovered (#96).
        if ((event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)
            && !qobject_cast<PanelButton *>(button) && panel_->drawer() && panel_->drawer()->isAncestorOf(button)) {
            const bool focused = event->type() == QEvent::FocusIn;
            button->setAttribute(Qt::WA_UnderMouse, focused || button->rect().contains(button->mapFromGlobal(QCursor::pos())));
            button->update();
        }
    }
    // The panel's layout may settle after refreshPanel() (a drawer row changing its controls):
    // place it again so it stays centred instead of growing from its old left edge.
    if (watched == panel_ && event->type() == QEvent::Enter) {
        session_->setPointerPick({}); // the pointer is on the panel, not on a target
    }
    if (watched == panel_ && event->type() == QEvent::Resize) {
        placePanel();
        placeHint();
    }
    return QWidget::eventFilter(watched, event);
}

void CaptureOverlay::placeHint()
{
    const QString text = session_->hint();
    if (text.isEmpty() || panel_->isHidden()) { // isVisible() is false until the overlay is shown
        hintPanel_->hide();
        return;
    }
    hint_->setText(text);
    // One line up to 720 px; a word-wrapped label would otherwise pick a narrow width by itself.
    hint_->setFixedWidth(std::min(hint_->fontMetrics().horizontalAdvance(text) + 2, 720));
    hintPanel_->adjustSize();
    const QSize size = hintPanel_->sizeHint();
    const QRect panel = panel_->geometry();
    const int x = std::clamp(panel.center().x() - size.width() / 2, 0, std::max(0, width() - size.width()));
    // Below the panel, or above it when the panel sits in the lower half.
    const bool above = panel.center().y() > height() / 2;
    const int y = above ? panel.top() - size.height() : panel.bottom() + 1;
    hintPanel_->setGeometry(x, std::clamp(y, 0, std::max(0, height() - size.height())), size.width(), size.height());
    hintPanel_->show();
    hintPanel_->raise();
}

} // namespace itsova
