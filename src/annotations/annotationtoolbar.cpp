#include "annotationtoolbar.h"

#include <QButtonGroup>
#include <QColorDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <QSlider>
#include <QToolButton>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace glimply
{
namespace
{

QIcon makeVectorIcon(const QColor &ink, const std::function<void(QPainter &)> &draw)
{
    QIcon icon;
    for (const qreal dpr : {1.0, 2.0}) {
        const int px = int(std::lround(18.0 * dpr));
        QPixmap pm(px, px);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        {
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(QPen(ink, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            draw(p);
        }
        icon.addPixmap(pm);
    }
    return icon;
}

QIcon toolGlyphIcon(ToolKind tool, const QColor &ink)
{
    const auto fallback = makeVectorIcon(ink, [&](QPainter &p) {
        switch (tool) {
        case ToolKind::Select: {
            const QPolygonF cursor{{4.0, 3.0}, {4.0, 14.5}, {7.5, 11.5}, {10.5, 15.5}, {12.2, 14.5},
                                   {9.2, 10.5}, {13.5, 10.5}};
            p.setBrush(ink);
            p.setPen(QPen(ink, 1.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPolygon(cursor);
            break;
        }
        case ToolKind::Crop: {
            p.drawPolyline(QPolygonF{{5.0, 2.5}, {5.0, 13.0}, {15.5, 13.0}});
            p.drawPolyline(QPolygonF{{2.5, 5.5}, {12.5, 5.5}, {12.5, 15.5}});
            break;
        }
        case ToolKind::Arrow: {
            p.drawLine(QPointF(3.5, 14.5), QPointF(14.5, 3.5));
            p.drawPolyline(QPolygonF{{8.5, 3.5}, {14.5, 3.5}, {14.5, 9.5}});
            break;
        }
        case ToolKind::Rectangle:
            p.drawRoundedRect(QRectF(3.0, 4.0, 12.0, 10.0), 1.5, 1.5);
            break;
        case ToolKind::Ellipse:
            p.drawEllipse(QRectF(2.8, 4.0, 12.4, 10.0));
            break;
        case ToolKind::Pen: {
            QPainterPath path;
            path.moveTo(3.0, 13.5);
            path.cubicTo(6.5, 6.0, 9.5, 14.0, 15.0, 4.5);
            p.drawPath(path);
            break;
        }
        case ToolKind::Highlighter: {
            p.drawLine(QPointF(4.0, 14.0), QPointF(11.5, 4.0));
            p.drawLine(QPointF(7.0, 15.0), QPointF(14.5, 5.0));
            p.drawLine(QPointF(3.0, 14.5), QPointF(15.0, 14.5));
            break;
        }
        case ToolKind::Text: {
            p.drawLine(QPointF(4.0, 4.0), QPointF(14.0, 4.0));
            p.drawLine(QPointF(9.0, 4.0), QPointF(9.0, 14.5));
            p.drawLine(QPointF(6.5, 14.5), QPointF(11.5, 14.5));
            break;
        }
        case ToolKind::Step: {
            p.drawEllipse(QRectF(3.0, 3.0, 12.0, 12.0));
            QFont f = p.font();
            f.setBold(true);
            f.setPixelSize(9);
            p.setFont(f);
            p.drawText(QRectF(3.0, 3.0, 12.0, 12.0), Qt::AlignCenter, QStringLiteral("1"));
            break;
        }
        case ToolKind::Redact: {
            p.setBrush(ink);
            p.drawRect(QRectF(3.0, 4.5, 12.0, 9.0));
            break;
        }
        case ToolKind::Spotlight: {
            p.drawRoundedRect(QRectF(2.5, 3.5, 13.0, 11.0), 1.5, 1.5);
            p.drawEllipse(QRectF(5.5, 5.5, 7.0, 7.0));
            break;
        }
        }
    });

    switch (tool) {
    case ToolKind::Select:
        return QIcon::fromTheme(QStringLiteral("edit-select"), fallback);
    case ToolKind::Crop:
        return QIcon::fromTheme(QStringLiteral("transform-crop"), fallback);
    case ToolKind::Arrow:
        return QIcon::fromTheme(QStringLiteral("draw-arrow"), fallback);
    case ToolKind::Rectangle:
        return QIcon::fromTheme(QStringLiteral("draw-rectangle"), fallback);
    case ToolKind::Ellipse:
        return QIcon::fromTheme(QStringLiteral("draw-ellipse"), fallback);
    case ToolKind::Pen:
        return QIcon::fromTheme(QStringLiteral("draw-freehand"), fallback);
    case ToolKind::Highlighter:
        return QIcon::fromTheme(QStringLiteral("draw-highlight"), fallback);
    case ToolKind::Text:
        return QIcon::fromTheme(QStringLiteral("draw-text"), fallback);
    case ToolKind::Step:
    case ToolKind::Redact:
    case ToolKind::Spotlight:
        return fallback;
    }
    return fallback;
}

QIcon makeSwatchIcon(const QColor &color, bool selected, const QColor &highlight, const QColor &border)
{
    QIcon icon;
    for (const qreal dpr : {1.0, 2.0}) {
        const int px = int(std::lround(18.0 * dpr));
        QPixmap pm(px, px);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        {
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            if (selected) {
                p.setPen(QPen(highlight, 2.0));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(QRectF(1.5, 1.5, 15.0, 15.0));
            }
            p.setPen(QPen(border, 1.0));
            p.setBrush(color);
            p.drawEllipse(QRectF(3.5, 3.5, 11.0, 11.0));
            if (selected) {
                const QColor checkCol = contrastingTextColor(color);
                p.setPen(QPen(checkCol, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.drawPolyline(QPolygonF{{6.6, 9.2}, {8.4, 11.1}, {11.6, 7.2}});
            }
        }
        icon.addPixmap(pm);
    }
    return icon;
}

QIcon makeCustomWheelIcon(const QColor &ink)
{
    QIcon fallback = makeVectorIcon(ink, [](QPainter &p) {
        const QList<QColor> sectors{
            QColor(0xE5, 0x39, 0x35),
            QColor(0xFB, 0xC0, 0x2D),
            QColor(0x43, 0xA0, 0x47),
            QColor(0x1E, 0x88, 0xE5),
        };
        p.setPen(Qt::NoPen);
        for (int i = 0; i < 4; ++i) {
            p.setBrush(sectors[i]);
            p.drawPie(QRectF(3.0, 3.0, 12.0, 12.0), i * 90 * 16, 90 * 16);
        }
    });
    return QIcon::fromTheme(QStringLiteral("color-management"), fallback);
}

QFrame *makeVerticalRule(QWidget *parent)
{
    auto *line = new QFrame(parent);
    line->setFrameShape(QFrame::VLine);
    line->setFrameShadow(QFrame::Sunken);
    return line;
}

} // namespace

AnnotationToolbar::AnnotationToolbar(AnnotationController *controller, QWidget *parent)
    : QWidget(parent)
    , controller_(controller)
    , swatches_(defaultAnnotationSwatches())
{
    buildUi();
    setAvailableTools({
        ToolKind::Select,
        ToolKind::Arrow,
        ToolKind::Rectangle,
        ToolKind::Ellipse,
        ToolKind::Pen,
        ToolKind::Highlighter,
        ToolKind::Step,
        ToolKind::Spotlight,
    });

    if (controller_) {
        connect(controller_, &AnnotationController::toolChanged, this, &AnnotationToolbar::syncFromState);
        connect(controller_, &AnnotationController::styleChanged, this, &AnnotationToolbar::syncFromState);
        if (AnnotationDocument *doc = controller_->document()) {
            connect(doc, &AnnotationDocument::selectionChanged, this, &AnnotationToolbar::syncFromState);
            connect(doc, &AnnotationDocument::changed, this, &AnnotationToolbar::syncFromState);
            connect(&doc->undoStack(), &QUndoStack::canUndoChanged, this, &AnnotationToolbar::syncFromState);
            connect(&doc->undoStack(), &QUndoStack::canRedoChanged, this, &AnnotationToolbar::syncFromState);
        }
    }
    syncFromState();
}

void AnnotationToolbar::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(2, 2, 2, 2);
    root->setSpacing(4);

    // Row 1: Primary tool strip + Undo / Redo.
    toolRow_ = new QWidget(this);
    auto *row1 = new QHBoxLayout(toolRow_);
    row1->setContentsMargins(0, 0, 0, 0);
    row1->setSpacing(2);

    toolGroup_ = new QButtonGroup(this);
    toolGroup_->setExclusive(true);

    const QColor ink = palette().color(QPalette::WindowText);

    const auto addTool = [&](ToolKind kind, const QString &name, const QString &tip) {
        auto *btn = new QToolButton(toolRow_);
        btn->setIcon(toolGlyphIcon(kind, ink));
        btn->setIconSize(QSize(18, 18));
        btn->setToolTip(tip);
        btn->setAccessibleName(name);
        btn->setCheckable(true);
        btn->setAutoRaise(true);
        btn->setFocusPolicy(Qt::NoFocus);
        toolGroup_->addButton(btn, int(kind));
        toolButtons_.insert(int(kind), btn);
        row1->addWidget(btn);
        connect(btn, &QToolButton::clicked, this, [this, kind] {
            if (controller_) {
                controller_->setTool(kind);
            }
        });
    };

    addTool(ToolKind::Select, tr("Select / Move"), tr("Select, move, or resize annotations (V)"));
    addTool(ToolKind::Crop, tr("Crop"), tr("Crop image (C)"));

    QFrame *sep1 = makeVerticalRule(toolRow_);
    toolSeparators_.append(sep1);
    row1->addWidget(sep1);

    addTool(ToolKind::Arrow, tr("Arrow"), tr("Draw arrow; hold Shift to snap angle (A)"));
    addTool(ToolKind::Rectangle, tr("Rectangle"), tr("Draw rectangle; Shift for square, Alt from center (R)"));
    addTool(ToolKind::Ellipse, tr("Ellipse"), tr("Draw ellipse; Shift for circle, Alt from center (E)"));
    addTool(ToolKind::Pen, tr("Pen"), tr("Freehand pen (B)"));
    addTool(ToolKind::Highlighter, tr("Highlighter"), tr("Multiply highlighter (M)"));
    addTool(ToolKind::Text, tr("Text"), tr("Add text annotation (T)"));
    addTool(ToolKind::Step, tr("Step Counter"), tr("Place numbered badge; drag to add pointer (N)"));
    addTool(ToolKind::Redact, tr("Redact"), tr("Irreversible redaction (X)"));
    addTool(ToolKind::Spotlight, tr("Spotlight"), tr("Highlight region and dim surroundings (S)"));

    QFrame *sep2 = makeVerticalRule(toolRow_);
    toolSeparators_.append(sep2);
    row1->addWidget(sep2);

    const auto undoFallback = makeVectorIcon(ink, [](QPainter &p) {
        p.drawArc(QRectF(4.0, 5.5, 10.0, 9.0), 0, 180 * 16);
        p.drawPolyline(QPolygonF{{4.0, 6.0}, {4.0, 10.0}, {8.0, 10.0}});
    });
    undoButton_ = new QToolButton(toolRow_);
    undoButton_->setIcon(QIcon::fromTheme(QStringLiteral("edit-undo"), undoFallback));
    undoButton_->setIconSize(QSize(18, 18));
    undoButton_->setToolTip(tr("Undo (Ctrl+Z)"));
    undoButton_->setAccessibleName(tr("Undo"));
    undoButton_->setAutoRaise(true);
    undoButton_->setFocusPolicy(Qt::NoFocus);
    connect(undoButton_, &QToolButton::clicked, this, [this] {
        if (controller_ && controller_->document()) {
            controller_->document()->undoStack().undo();
        }
    });
    row1->addWidget(undoButton_);

    const auto redoFallback = makeVectorIcon(ink, [](QPainter &p) {
        p.drawArc(QRectF(4.0, 5.5, 10.0, 9.0), 0, 180 * 16);
        p.drawPolyline(QPolygonF{{14.0, 6.0}, {14.0, 10.0}, {10.0, 10.0}});
    });
    redoButton_ = new QToolButton(toolRow_);
    redoButton_->setIcon(QIcon::fromTheme(QStringLiteral("edit-redo"), redoFallback));
    redoButton_->setIconSize(QSize(18, 18));
    redoButton_->setToolTip(tr("Redo (Ctrl+Shift+Z)"));
    redoButton_->setAccessibleName(tr("Redo"));
    redoButton_->setAutoRaise(true);
    redoButton_->setFocusPolicy(Qt::NoFocus);
    connect(redoButton_, &QToolButton::clicked, this, [this] {
        if (controller_ && controller_->document()) {
            controller_->document()->undoStack().redo();
        }
    });
    row1->addWidget(redoButton_);
    row1->addStretch();

    root->addWidget(toolRow_);

    // Row 2: Contextual property strip (hidden when Select is active and nothing is selected).
    propertyBar_ = new QWidget(this);
    auto *row2 = new QHBoxLayout(propertyBar_);
    row2->setContentsMargins(2, 2, 2, 0);
    row2->setSpacing(6);

    // Color swatches + Custom picker.
    colorGroup_ = new QWidget(propertyBar_);
    auto *colorLayout = new QHBoxLayout(colorGroup_);
    colorLayout->setContentsMargins(0, 0, 0, 0);
    colorLayout->setSpacing(2);
    for (const QColor &swatch : std::as_const(swatches_)) {
        auto *btn = new QToolButton(colorGroup_);
        btn->setIconSize(QSize(18, 18));
        btn->setToolTip(tr("Color %1").arg(swatch.name(QColor::HexRgb)));
        btn->setAccessibleName(tr("Color %1").arg(swatch.name(QColor::HexRgb)));
        btn->setAutoRaise(true);
        btn->setFocusPolicy(Qt::NoFocus);
        swatchButtons_.append(btn);
        colorLayout->addWidget(btn);
        connect(btn, &QToolButton::clicked, this, [this, swatch] { applyColorChoice(swatch); });
    }
    customColorButton_ = new QToolButton(colorGroup_);
    customColorButton_->setIcon(makeCustomWheelIcon(ink));
    customColorButton_->setIconSize(QSize(18, 18));
    customColorButton_->setToolTip(tr("Choose custom color…"));
    customColorButton_->setAccessibleName(tr("Custom color"));
    customColorButton_->setAutoRaise(true);
    customColorButton_->setFocusPolicy(Qt::NoFocus);
    connect(customColorButton_, &QToolButton::clicked, this, [this] {
        const QColor chosen = QColorDialog::getColor(
            controller_ ? controller_->strokeColor() : Qt::red, this, tr("Select Annotation Color"));
        if (chosen.isValid()) {
            applyColorChoice(chosen);
        }
    });
    colorLayout->addWidget(customColorButton_);
    row2->addWidget(colorGroup_);

    // Stroke / marker width slider.
    sizeGroup_ = new QWidget(propertyBar_);
    auto *sizeLayout = new QHBoxLayout(sizeGroup_);
    sizeLayout->setContentsMargins(0, 0, 0, 0);
    sizeLayout->setSpacing(4);
    sizeTitleLabel_ = new QLabel(tr("Width:"), sizeGroup_);
    sizeSlider_ = new QSlider(Qt::Horizontal, sizeGroup_);
    sizeSlider_->setFixedWidth(84);
    sizeSlider_->setRange(2, 16);
    sizeSlider_->setAccessibleName(tr("Stroke width"));
    sizeSlider_->setFocusPolicy(Qt::NoFocus);
    sizeValueLabel_ = new QLabel(sizeGroup_);
    sizeValueLabel_->setMinimumWidth(fontMetrics().horizontalAdvance(QStringLiteral("48 px")) + 4);
    sizeLayout->addWidget(sizeTitleLabel_);
    sizeLayout->addWidget(sizeSlider_);
    sizeLayout->addWidget(sizeValueLabel_);
    connect(sizeSlider_, &QSlider::valueChanged, this, &AnnotationToolbar::applySizeChoice);
    row2->addWidget(sizeGroup_);

    // Shape fill toggle + opacity (Rectangle & Ellipse only).
    fillGroup_ = new QWidget(propertyBar_);
    auto *fillLayout = new QHBoxLayout(fillGroup_);
    fillLayout->setContentsMargins(0, 0, 0, 0);
    fillLayout->setSpacing(3);

    auto *fillModeGroup = new QButtonGroup(fillGroup_);
    fillModeGroup->setExclusive(true);

    outlineButton_ = new QToolButton(fillGroup_);
    outlineButton_->setIcon(makeVectorIcon(ink, [](QPainter &p) { p.drawRect(QRectF(4, 5, 10, 8)); }));
    outlineButton_->setText(tr("Outline"));
    outlineButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    outlineButton_->setCheckable(true);
    outlineButton_->setAutoRaise(true);
    outlineButton_->setFocusPolicy(Qt::NoFocus);
    fillModeGroup->addButton(outlineButton_);
    fillLayout->addWidget(outlineButton_);
    connect(outlineButton_, &QToolButton::clicked, this, [this] { applyFillModeChoice(ShapeFillMode::Outline); });

    filledButton_ = new QToolButton(fillGroup_);
    filledButton_->setIcon(makeVectorIcon(ink, [&](QPainter &p) {
        QColor tint = ink;
        tint.setAlpha(95);
        p.setBrush(tint);
        p.drawRect(QRectF(4, 5, 10, 8));
    }));
    filledButton_->setText(tr("Fill"));
    filledButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    filledButton_->setCheckable(true);
    filledButton_->setAutoRaise(true);
    filledButton_->setFocusPolicy(Qt::NoFocus);
    fillModeGroup->addButton(filledButton_);
    fillLayout->addWidget(filledButton_);
    connect(filledButton_, &QToolButton::clicked, this,
            [this] { applyFillModeChoice(ShapeFillMode::TranslucentFill); });

    fillOpacityBox_ = new QWidget(fillGroup_);
    auto *opLayout = new QHBoxLayout(fillOpacityBox_);
    opLayout->setContentsMargins(4, 0, 0, 0);
    opLayout->setSpacing(4);
    opLayout->addWidget(new QLabel(tr("Opacity:"), fillOpacityBox_));
    fillOpacitySlider_ = new QSlider(Qt::Horizontal, fillOpacityBox_);
    fillOpacitySlider_->setFixedWidth(72);
    fillOpacitySlider_->setRange(10, 100);
    fillOpacitySlider_->setAccessibleName(tr("Fill opacity"));
    fillOpacitySlider_->setFocusPolicy(Qt::NoFocus);
    fillOpacityLabel_ = new QLabel(fillOpacityBox_);
    fillOpacityLabel_->setMinimumWidth(fontMetrics().horizontalAdvance(QStringLiteral("100%")) + 4);
    opLayout->addWidget(fillOpacitySlider_);
    opLayout->addWidget(fillOpacityLabel_);
    connect(fillOpacitySlider_, &QSlider::valueChanged, this, &AnnotationToolbar::applyFillOpacityChoice);
    fillLayout->addWidget(fillOpacityBox_);
    row2->addWidget(fillGroup_);

    // Spotlight shape + dim opacity (Spotlight only).
    spotlightGroup_ = new QWidget(propertyBar_);
    auto *spotLayout = new QHBoxLayout(spotlightGroup_);
    spotLayout->setContentsMargins(0, 0, 0, 0);
    spotLayout->setSpacing(3);

    auto *spotShapeGroup = new QButtonGroup(spotlightGroup_);
    spotShapeGroup->setExclusive(true);

    spotlightRectButton_ = new QToolButton(spotlightGroup_);
    spotlightRectButton_->setIcon(toolGlyphIcon(ToolKind::Rectangle, ink));
    spotlightRectButton_->setText(tr("Rectangle"));
    spotlightRectButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    spotlightRectButton_->setCheckable(true);
    spotlightRectButton_->setAutoRaise(true);
    spotlightRectButton_->setFocusPolicy(Qt::NoFocus);
    spotShapeGroup->addButton(spotlightRectButton_);
    spotLayout->addWidget(spotlightRectButton_);
    connect(spotlightRectButton_, &QToolButton::clicked, this,
            [this] { applySpotlightShapeChoice(SpotlightShape::Rectangle); });

    spotlightEllipseButton_ = new QToolButton(spotlightGroup_);
    spotlightEllipseButton_->setIcon(toolGlyphIcon(ToolKind::Ellipse, ink));
    spotlightEllipseButton_->setText(tr("Ellipse"));
    spotlightEllipseButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    spotlightEllipseButton_->setCheckable(true);
    spotlightEllipseButton_->setAutoRaise(true);
    spotlightEllipseButton_->setFocusPolicy(Qt::NoFocus);
    spotShapeGroup->addButton(spotlightEllipseButton_);
    spotLayout->addWidget(spotlightEllipseButton_);
    connect(spotlightEllipseButton_, &QToolButton::clicked, this,
            [this] { applySpotlightShapeChoice(SpotlightShape::Ellipse); });

    spotLayout->addSpacing(4);
    spotLayout->addWidget(new QLabel(tr("Dim:"), spotlightGroup_));
    spotlightDimSlider_ = new QSlider(Qt::Horizontal, spotlightGroup_);
    spotlightDimSlider_->setFixedWidth(76);
    spotlightDimSlider_->setRange(35, 85);
    spotlightDimSlider_->setAccessibleName(tr("Spotlight dim opacity"));
    spotlightDimSlider_->setFocusPolicy(Qt::NoFocus);
    spotlightDimLabel_ = new QLabel(spotlightGroup_);
    spotlightDimLabel_->setMinimumWidth(fontMetrics().horizontalAdvance(QStringLiteral("100%")) + 4);
    spotLayout->addWidget(spotlightDimSlider_);
    spotLayout->addWidget(spotlightDimLabel_);
    connect(spotlightDimSlider_, &QSlider::valueChanged, this, &AnnotationToolbar::applySpotlightDimChoice);
    row2->addWidget(spotlightGroup_);

    row2->addStretch();
    root->addWidget(propertyBar_);
}

void AnnotationToolbar::setAvailableTools(const QList<ToolKind> &tools)
{
    for (auto it = toolButtons_.cbegin(); it != toolButtons_.cend(); ++it) {
        it.value()->setVisible(tools.contains(static_cast<ToolKind>(it.key())));
    }
}

void AnnotationToolbar::setSettings(AnnotationSettings *settings)
{
    settings_ = settings;
    if (settings_ && controller_) {
        controller_->applySettings(*settings_);
    }
}

void AnnotationToolbar::setCompactOverlayMode(bool compact)
{
    compact_ = compact;
    if (auto *l = qobject_cast<QVBoxLayout *>(layout())) {
        l->setSpacing(compact ? 3 : 5);
    }
}

QToolButton *AnnotationToolbar::toolButton(ToolKind tool) const { return toolButtons_.value(int(tool), nullptr); }
QList<QToolButton *> AnnotationToolbar::swatchButtons() const { return swatchButtons_; }
QToolButton *AnnotationToolbar::customColorButton() const { return customColorButton_; }
QToolButton *AnnotationToolbar::undoButton() const { return undoButton_; }
QToolButton *AnnotationToolbar::redoButton() const { return redoButton_; }

bool AnnotationToolbar::isPropertyBarVisible() const { return !propertyBar_->isHidden(); }
bool AnnotationToolbar::isColorGroupVisible() const { return !propertyBar_->isHidden() && !colorGroup_->isHidden(); }
bool AnnotationToolbar::isSizeGroupVisible() const { return !propertyBar_->isHidden() && !sizeGroup_->isHidden(); }
bool AnnotationToolbar::isFillGroupVisible() const { return !propertyBar_->isHidden() && !fillGroup_->isHidden(); }
bool AnnotationToolbar::isFillOpacityVisible() const { return isFillGroupVisible() && !fillOpacityBox_->isHidden(); }
bool AnnotationToolbar::isSpotlightGroupVisible() const { return !propertyBar_->isHidden() && !spotlightGroup_->isHidden(); }

bool AnnotationToolbar::activeTargetKind(ItemKind *outKind) const
{
    if (!controller_) {
        return false;
    }
    if (AnnotationDocument *doc = controller_->document()) {
        if (const AnnotationItem *sel = doc->selectedItem()) {
            *outKind = sel->kind;
            return true;
        }
    }
    switch (controller_->tool()) {
    case ToolKind::Arrow:
        *outKind = ItemKind::Arrow;
        return true;
    case ToolKind::Rectangle:
        *outKind = ItemKind::Rectangle;
        return true;
    case ToolKind::Ellipse:
        *outKind = ItemKind::Ellipse;
        return true;
    case ToolKind::Pen:
        *outKind = ItemKind::Pen;
        return true;
    case ToolKind::Highlighter:
        *outKind = ItemKind::Highlighter;
        return true;
    case ToolKind::Text:
        *outKind = ItemKind::Text;
        return true;
    case ToolKind::Step:
        *outKind = ItemKind::Step;
        return true;
    case ToolKind::Redact:
        *outKind = ItemKind::Redact;
        return true;
    case ToolKind::Spotlight:
        *outKind = ItemKind::Spotlight;
        return true;
    case ToolKind::Select:
    case ToolKind::Crop:
        return false;
    }
    return false;
}

void AnnotationToolbar::updateSwatchIcons(const QColor &activeColor)
{
    const QColor highlight = palette().color(QPalette::Highlight);
    QColor border = palette().color(QPalette::WindowText);
    border.setAlpha(90);
    for (qsizetype i = 0; i < swatches_.size() && i < swatchButtons_.size(); ++i) {
        const bool active = swatches_[i].rgb() == activeColor.rgb();
        swatchButtons_[i]->setIcon(makeSwatchIcon(swatches_[i], active, highlight, border));
    }
}

void AnnotationToolbar::syncFromState()
{
    if (!controller_ || updating_) {
        return;
    }
    updating_ = true;

    if (QToolButton *activeBtn = toolButtons_.value(int(controller_->tool()), nullptr)) {
        activeBtn->setChecked(true);
    }
    if (AnnotationDocument *doc = controller_->document()) {
        undoButton_->setEnabled(doc->undoStack().canUndo());
        redoButton_->setEnabled(doc->undoStack().canRedo());
    } else {
        undoButton_->setEnabled(false);
        redoButton_->setEnabled(false);
    }

    ItemKind kind = ItemKind::Rectangle;
    if (!activeTargetKind(&kind)) {
        propertyBar_->hide();
        updating_ = false;
        return;
    }
    propertyBar_->show();

    const AnnotationItem *sel = controller_->document() ? controller_->document()->selectedItem() : nullptr;

    const bool showColor = kind != ItemKind::Spotlight
                        && !(kind == ItemKind::Redact
                             && (sel ? sel->redactionMode : controller_->redactionMode()) != RedactionMode::Solid);
    const bool showSize = kind == ItemKind::Arrow || kind == ItemKind::Rectangle || kind == ItemKind::Ellipse
                       || kind == ItemKind::Pen || kind == ItemKind::Highlighter || kind == ItemKind::Step;
    const bool showFill = kind == ItemKind::Rectangle || kind == ItemKind::Ellipse;
    const bool showSpotlight = kind == ItemKind::Spotlight;

    colorGroup_->setVisible(showColor);
    sizeGroup_->setVisible(showSize);
    fillGroup_->setVisible(showFill);
    spotlightGroup_->setVisible(showSpotlight);

    if (showColor) {
        QColor activeColor = controller_->strokeColor();
        if (sel) {
            activeColor = sel->color;
        } else if (kind == ItemKind::Highlighter) {
            activeColor = controller_->highlighterColor();
        } else if (kind == ItemKind::Redact) {
            activeColor = controller_->redactionColor();
        }
        updateSwatchIcons(activeColor);
    }

    if (showSize) {
        const bool isHighlighter = kind == ItemKind::Highlighter;
        const int minW = isHighlighter ? 8 : 2;
        const int maxW = isHighlighter ? 48 : 16;
        const int curW = int(std::lround(sel ? sel->strokeWidth
                                             : (isHighlighter ? controller_->highlighterWidth()
                                                              : controller_->strokeWidth())));
        sizeSlider_->setRange(minW, maxW);
        sizeSlider_->setValue(std::clamp(curW, minW, maxW));
        sizeValueLabel_->setText(tr("%1 px").arg(sizeSlider_->value()));
    }

    if (showFill) {
        const ShapeFillMode mode = sel ? sel->fillMode : controller_->fillMode();
        const qreal opacity = sel ? sel->fillOpacity : controller_->fillOpacity();
        outlineButton_->setChecked(mode == ShapeFillMode::Outline);
        filledButton_->setChecked(mode == ShapeFillMode::TranslucentFill);
        const bool showOp = mode == ShapeFillMode::TranslucentFill;
        fillOpacityBox_->setVisible(showOp);
        const int pct = std::clamp(int(std::lround(opacity * 100.0)), 10, 100);
        fillOpacitySlider_->setValue(pct);
        fillOpacityLabel_->setText(tr("%1%").arg(pct));
    }

    if (showSpotlight) {
        const SpotlightShape shape = sel ? sel->spotlightShape : controller_->spotlightShape();
        const qreal dim = sel ? sel->dimOpacity : controller_->dimOpacity();
        spotlightRectButton_->setChecked(shape == SpotlightShape::Rectangle);
        spotlightEllipseButton_->setChecked(shape == SpotlightShape::Ellipse);
        const int pct = std::clamp(int(std::lround(dim * 100.0)), 35, 85);
        spotlightDimSlider_->setValue(pct);
        spotlightDimLabel_->setText(tr("%1%").arg(pct));
    }

    updating_ = false;
}

void AnnotationToolbar::applyColorChoice(const QColor &color)
{
    if (!controller_ || updating_) {
        return;
    }
    ItemKind kind = ItemKind::Rectangle;
    if (!activeTargetKind(&kind)) {
        return;
    }
    if (kind == ItemKind::Highlighter) {
        controller_->setHighlighterColor(color);
    } else if (kind == ItemKind::Redact) {
        controller_->setRedactionColor(color);
    } else {
        controller_->setStrokeColor(color);
    }
    if (AnnotationDocument *doc = controller_->document()) {
        if (const AnnotationItem *sel = doc->selectedItem()) {
            AnnotationItem after = *sel;
            after.color = color;
            after.color.setAlpha(255);
            doc->commitModification(*sel, after, /*mergeKey=*/3);
        }
    }
    if (settings_) {
        controller_->writeSettings(*settings_);
        Q_EMIT settingsModified();
    }
    syncFromState();
}

void AnnotationToolbar::applySizeChoice(int value)
{
    if (!controller_ || updating_) {
        return;
    }
    ItemKind kind = ItemKind::Rectangle;
    if (!activeTargetKind(&kind)) {
        return;
    }
    sizeValueLabel_->setText(tr("%1 px").arg(value));
    if (kind == ItemKind::Highlighter) {
        controller_->setHighlighterWidth(value);
    } else {
        controller_->setStrokeWidth(value);
    }
    if (AnnotationDocument *doc = controller_->document()) {
        if (const AnnotationItem *sel = doc->selectedItem()) {
            AnnotationItem after = *sel;
            after.strokeWidth = value;
            doc->commitModification(*sel, after, /*mergeKey=*/4);
        }
    }
    if (settings_) {
        controller_->writeSettings(*settings_);
        Q_EMIT settingsModified();
    }
}

void AnnotationToolbar::applyFillModeChoice(ShapeFillMode mode)
{
    if (!controller_ || updating_) {
        return;
    }
    controller_->setFillMode(mode);
    if (AnnotationDocument *doc = controller_->document()) {
        if (const AnnotationItem *sel = doc->selectedItem();
            sel && (sel->kind == ItemKind::Rectangle || sel->kind == ItemKind::Ellipse)) {
            AnnotationItem after = *sel;
            after.fillMode = mode;
            doc->commitModification(*sel, after, /*mergeKey=*/5);
        }
    }
    if (settings_) {
        controller_->writeSettings(*settings_);
        Q_EMIT settingsModified();
    }
    syncFromState();
}

void AnnotationToolbar::applyFillOpacityChoice(int percent)
{
    if (!controller_ || updating_) {
        return;
    }
    const qreal op = std::clamp(percent / 100.0, 0.10, 1.0);
    fillOpacityLabel_->setText(tr("%1%").arg(percent));
    controller_->setFillOpacity(op);
    if (AnnotationDocument *doc = controller_->document()) {
        if (const AnnotationItem *sel = doc->selectedItem();
            sel && (sel->kind == ItemKind::Rectangle || sel->kind == ItemKind::Ellipse)) {
            AnnotationItem after = *sel;
            after.fillOpacity = op;
            doc->commitModification(*sel, after, /*mergeKey=*/6);
        }
    }
    if (settings_) {
        controller_->writeSettings(*settings_);
        Q_EMIT settingsModified();
    }
}

void AnnotationToolbar::applySpotlightShapeChoice(SpotlightShape shape)
{
    if (!controller_ || updating_) {
        return;
    }
    controller_->setSpotlightShape(shape);
    if (AnnotationDocument *doc = controller_->document()) {
        if (const AnnotationItem *sel = doc->selectedItem(); sel && sel->kind == ItemKind::Spotlight) {
            AnnotationItem after = *sel;
            after.spotlightShape = shape;
            doc->commitModification(*sel, after, /*mergeKey=*/7);
        }
    }
    if (settings_) {
        controller_->writeSettings(*settings_);
        Q_EMIT settingsModified();
    }
    syncFromState();
}

void AnnotationToolbar::applySpotlightDimChoice(int percent)
{
    if (!controller_ || updating_) {
        return;
    }
    const qreal dim = std::clamp(percent / 100.0, 0.35, 0.85);
    spotlightDimLabel_->setText(tr("%1%").arg(percent));
    controller_->setDimOpacity(dim);
    if (AnnotationDocument *doc = controller_->document()) {
        if (const AnnotationItem *sel = doc->selectedItem(); sel && sel->kind == ItemKind::Spotlight) {
            AnnotationItem after = *sel;
            after.dimOpacity = dim;
            doc->commitModification(*sel, after, /*mergeKey=*/8);
        }
    }
    if (settings_) {
        controller_->writeSettings(*settings_);
        Q_EMIT settingsModified();
    }
}

} // namespace glimply
