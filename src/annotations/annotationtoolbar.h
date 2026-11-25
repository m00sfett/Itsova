#pragma once

#include "annotationcontroller.h"
#include "annotationitem.h"
#include "annotationsettings.h"

#include <QColor>
#include <QHash>
#include <QList>
#include <QWidget>

class QButtonGroup;
class QFontComboBox;
class QLabel;
class QSlider;
class QSpinBox;
class QToolButton;

namespace glimply
{

// Native KDE/Breeze two-row markup toolbar shared by CaptureOverlay (#16) and QuickEditorWindow
// (#17). Row 1 offers tools and Undo/Redo; Row 2 uses progressive disclosure to show only the
// property controls relevant to the active tool or currently selected item.
class AnnotationToolbar : public QWidget
{
    Q_OBJECT

public:
    explicit AnnotationToolbar(AnnotationController *controller, QWidget *parent = nullptr);

    void setAvailableTools(const QList<ToolKind> &tools);
    void setSettings(AnnotationSettings *settings);
    void setCompactOverlayMode(bool compact);
    // Overlay panel drawer (#71): the panel draws the tool row itself, so this toolbar hides its
    // own and keeps only the slim property row, with Undo/Redo at its trailing end. The property
    // row then stays visible even when no tool property applies, for Undo/Redo.
    void setPanelMode(bool panel);
    [[nodiscard]] bool isToolRowVisible() const;
    // Width of the property row in its widest state (text with Apply/Cancel, redaction with
    // strength, filled shape), so a drawer can keep one width while the tool changes.
    [[nodiscard]] int widestPropertyRowWidth();

    [[nodiscard]] QToolButton *toolButton(ToolKind tool) const;
    [[nodiscard]] QList<QToolButton *> swatchButtons() const;
    [[nodiscard]] QToolButton *customColorButton() const;
    [[nodiscard]] QToolButton *undoButton() const;
    [[nodiscard]] QToolButton *redoButton() const;
    [[nodiscard]] QToolButton *applyTextButton() const;
    [[nodiscard]] QToolButton *cancelTextButton() const;
    [[nodiscard]] QToolButton *legibilityButton(TextLegibility legibility) const;
    [[nodiscard]] QToolButton *redactionModeButton(RedactionMode mode) const;
    [[nodiscard]] QSlider *redactionStrengthSlider() const;

    [[nodiscard]] bool isPropertyBarVisible() const;
    [[nodiscard]] bool isColorGroupVisible() const;
    [[nodiscard]] bool isSizeGroupVisible() const;
    [[nodiscard]] bool isFillGroupVisible() const;
    [[nodiscard]] bool isFillOpacityVisible() const;
    [[nodiscard]] bool isSpotlightGroupVisible() const;
    [[nodiscard]] bool isTextGroupVisible() const;
    [[nodiscard]] bool isTextActionBoxVisible() const;
    [[nodiscard]] bool isRedactGroupVisible() const;
    [[nodiscard]] bool isRedactStrengthVisible() const;

Q_SIGNALS:
    void settingsModified();

private:
    void buildUi();
    void syncFromState();
    void updateSwatchIcons(const QColor &activeColor);
    void applyColorChoice(const QColor &color);
    void applySizeChoice(int value);
    void applyFillModeChoice(ShapeFillMode mode);
    void applyFillOpacityChoice(int percent);
    void applySpotlightShapeChoice(SpotlightShape shape);
    void applySpotlightDimChoice(int percent);
    void applyRedactionModeChoice(RedactionMode mode);
    void applyRedactionStrengthChoice(int value);
    void applyTextChange(const std::function<void(AnnotationItem &)> &mutateItem, int mergeKey);
    [[nodiscard]] bool activeTargetKind(ItemKind *outKind) const;

    AnnotationController *controller_ = nullptr;
    AnnotationSettings *settings_ = nullptr;
    bool updating_ = false;
    bool compact_ = false;
    bool panelMode_ = false;

    QWidget *toolRow_ = nullptr;
    QButtonGroup *toolGroup_ = nullptr;
    QHash<int, QToolButton *> toolButtons_;
    QList<QWidget *> toolSeparators_;
    QToolButton *undoButton_ = nullptr;
    QToolButton *redoButton_ = nullptr;

    QWidget *propertyBar_ = nullptr;
    QWidget *colorGroup_ = nullptr;
    QList<QColor> swatches_;
    QList<QToolButton *> swatchButtons_;
    QToolButton *customColorButton_ = nullptr;

    QWidget *sizeGroup_ = nullptr;
    QLabel *sizeTitleLabel_ = nullptr;
    QSlider *sizeSlider_ = nullptr;
    QLabel *sizeValueLabel_ = nullptr;

    QWidget *fillGroup_ = nullptr;
    QToolButton *outlineButton_ = nullptr;
    QToolButton *filledButton_ = nullptr;
    QWidget *fillOpacityBox_ = nullptr;
    QSlider *fillOpacitySlider_ = nullptr;
    QLabel *fillOpacityLabel_ = nullptr;

    QWidget *spotlightGroup_ = nullptr;
    QToolButton *spotlightRectButton_ = nullptr;
    QToolButton *spotlightEllipseButton_ = nullptr;
    QSlider *spotlightDimSlider_ = nullptr;
    QLabel *spotlightDimLabel_ = nullptr;

    QWidget *textGroup_ = nullptr;
    QFontComboBox *fontCombo_ = nullptr;
    QSpinBox *fontSizeSpin_ = nullptr;
    QToolButton *boldButton_ = nullptr;
    QToolButton *italicButton_ = nullptr;
    QToolButton *alignLeftButton_ = nullptr;
    QToolButton *alignCenterButton_ = nullptr;
    QToolButton *alignRightButton_ = nullptr;
    QToolButton *legNoneButton_ = nullptr;
    QToolButton *legShadowButton_ = nullptr;
    QToolButton *legOutlineButton_ = nullptr;
    QToolButton *legPillButton_ = nullptr;
    QWidget *textActionBox_ = nullptr;
    QToolButton *applyTextButton_ = nullptr;
    QToolButton *cancelTextButton_ = nullptr;

    QWidget *redactGroup_ = nullptr;
    QToolButton *redactSolidButton_ = nullptr;
    QToolButton *redactPixelateButton_ = nullptr;
    QToolButton *redactBlurButton_ = nullptr;
    QWidget *redactStrengthBox_ = nullptr;
    QLabel *redactStrengthTitleLabel_ = nullptr;
    QSlider *redactStrengthSlider_ = nullptr;
    QLabel *redactStrengthValueLabel_ = nullptr;
};

} // namespace glimply
