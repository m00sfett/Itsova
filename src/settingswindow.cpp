#include "settingswindow.h"

#include "screenrecorder.h"

#include "app.h"
#include "areaselector.h"
#include "historystore.h"
#include "i18n.h"
#include "hookrunner.h"
#include "imageoutput.h"
#include "ocrengine.h"
#include "screens.h"
#include "selectiongeometry.h"
#include "shortcutmanager.h"
#include "triggerplan.h"

#include <KKeySequenceWidget>
#include <KMessageWidget>
#include <KPageWidgetItem>

#include <algorithm>

#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

namespace itsova
{
namespace
{

constexpr int idRole = Qt::UserRole;
constexpr int defaultFormatItemData = -1;
// The settings window must be gone before the frozen screen picture is taken.
constexpr int hideBeforeSelectDelayMs = 300;

QString websiteUrl()
{
    return QStringLiteral("https://roteson.de");
}

// The repository; private until the name is cleared, then public under the same address.
QString projectUrl()
{
    return QStringLiteral("https://github.com/m00sfett/Itsova");
}

QLabel *makeHint(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setEnabled(false); // disabled text colour is the KDE convention for secondary hints
    label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    return label;
}

QLabel *makeError(QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setWordWrap(true);
    QPalette pal = label->palette();
    pal.setColor(QPalette::WindowText, QColor(0xda, 0x44, 0x53)); // Breeze negative text
    label->setPalette(pal);
    label->hide();
    return label;
}

} // namespace

// Shared progressive-disclosure format/quality selector used by Storage (global default),
// Targets (per-target override), and Groups (per-group override).
class ExportOptionsWidget : public QWidget
{
public:
    ExportOptionsWidget(bool allowDefault, QWidget *parent = nullptr)
        : QWidget(parent)
        , allowDefault_(allowDefault)
    {
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);

        formatCombo_ = new QComboBox(this);
        formatCombo_->setAccessibleName(SettingsWindow::tr("Export image format"));
        layout->addWidget(formatCombo_);

        losslessCheck_ = new QCheckBox(SettingsWindow::tr("Lossless"), this);
        layout->addWidget(losslessCheck_);

        paramLabel_ = new QLabel(SettingsWindow::tr("Quality:"), this);
        layout->addWidget(paramLabel_);

        valueSpin_ = new QSpinBox(this);
        valueSpin_->setKeyboardTracking(false);
        valueSpin_->setAccessibleName(SettingsWindow::tr("Compression or quality level"));
        layout->addWidget(valueSpin_);

        if (!allowDefault_) {
            layout->addStretch(1);
        }

        rebuildFormats(ImageFormat::Png);

        connect(formatCombo_, &QComboBox::currentIndexChanged, this, [this] {
            if (updating_) {
                return;
            }
            const int data = formatCombo_->currentData().toInt();
            if (data == defaultFormatItemData) {
                overrideEnabled_ = false;
            } else {
                overrideEnabled_ = true;
                options_.format = static_cast<ImageFormat>(data);
            }
            syncParamWidgets();
            if (onChanged_) {
                onChanged_();
            }
        });
        connect(losslessCheck_, &QCheckBox::toggled, this, [this](bool on) {
            if (updating_) {
                return;
            }
            options_.lossless = on;
            valueSpin_->setEnabled(!on);
            if (onChanged_) {
                onChanged_();
            }
        });
        connect(valueSpin_, &QSpinBox::valueChanged, this, [this](int val) {
            if (updating_) {
                return;
            }
            if (options_.format == ImageFormat::Png) {
                options_.pngCompression = val;
            } else {
                options_.quality = val;
            }
            if (onChanged_) {
                onChanged_();
            }
        });
    }

    void setOnChanged(std::function<void()> callback)
    {
        onChanged_ = std::move(callback);
    }

    void setOptions(const ExportOptions &opts)
    {
        updating_ = true;
        overrideEnabled_ = true;
        options_ = opts;
        rebuildFormats(opts.format);
        formatCombo_->setCurrentIndex(formatCombo_->findData(int(opts.format)));
        syncParamWidgets();
        updating_ = false;
    }

    void setOverride(const ExportOverride &ov, ImageFormat globalFormat)
    {
        updating_ = true;
        overrideEnabled_ = ov.enabled;
        options_ = ov.options;
        rebuildFormats(globalFormat);
        const int targetData = ov.enabled ? int(ov.options.format) : defaultFormatItemData;
        const int idx = formatCombo_->findData(targetData);
        formatCombo_->setCurrentIndex(idx >= 0 ? idx : 0);
        syncParamWidgets();
        updating_ = false;
    }

    [[nodiscard]] ExportOptions options() const
    {
        return options_;
    }

    [[nodiscard]] ExportOverride overrideValue() const
    {
        return {overrideEnabled_, options_};
    }

private:
    void rebuildFormats(ImageFormat globalFormat)
    {
        const QSignalBlocker blocker(formatCombo_);
        formatCombo_->clear();
        if (allowDefault_) {
            formatCombo_->addItem(SettingsWindow::tr("Use default (%1)").arg(formatDisplayName(globalFormat)),
                                  defaultFormatItemData);
        }
        for (const ImageFormat fmt : availableCuratedFormats()) {
            formatCombo_->addItem(formatDisplayName(fmt), int(fmt));
        }
        if (overrideEnabled_ && formatCombo_->findData(int(options_.format)) < 0) {
            formatCombo_->addItem(formatDisplayName(options_.format), int(options_.format));
        }
    }

    void syncParamWidgets()
    {
        const bool showParams = !allowDefault_ || overrideEnabled_;
        if (!showParams) {
            losslessCheck_->hide();
            paramLabel_->hide();
            valueSpin_->hide();
            return;
        }
        const QSignalBlocker blockLossless(losslessCheck_);
        const QSignalBlocker blockSpin(valueSpin_);
        if (options_.format == ImageFormat::Png) {
            losslessCheck_->hide();
            paramLabel_->setText(SettingsWindow::tr("Compression:"));
            paramLabel_->show();
            valueSpin_->show();
            valueSpin_->setRange(0, 9);
            valueSpin_->setSuffix(QString());
            valueSpin_->setValue(options_.pngCompression >= 0 ? std::clamp(options_.pngCompression, 0, 9) : 6);
            valueSpin_->setEnabled(true);
        } else if (formatSupportsQuality(options_.format)) {
            const bool hasLossless = formatSupportsLosslessToggle(options_.format);
            losslessCheck_->setVisible(hasLossless);
            losslessCheck_->setChecked(hasLossless && options_.lossless);
            paramLabel_->setText(SettingsWindow::tr("Quality:"));
            paramLabel_->show();
            valueSpin_->show();
            const int maxQ = hasLossless ? 99 : 100;
            valueSpin_->setRange(1, maxQ);
            valueSpin_->setSuffix(SettingsWindow::tr(" %"));
            valueSpin_->setValue(std::clamp(options_.quality, 1, maxQ));
            valueSpin_->setEnabled(!(hasLossless && options_.lossless));
        } else {
            losslessCheck_->hide();
            paramLabel_->hide();
            valueSpin_->hide();
        }
    }

    bool allowDefault_ = false;
    bool overrideEnabled_ = false;
    bool updating_ = false;
    ExportOptions options_;
    QComboBox *formatCombo_ = nullptr;
    QCheckBox *losslessCheck_ = nullptr;
    QLabel *paramLabel_ = nullptr;
    QSpinBox *valueSpin_ = nullptr;
    std::function<void()> onChanged_;
};

class IntervalConfigWidget : public QWidget
{
public:
    explicit IntervalConfigWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);

        auto *topRow = new QHBoxLayout;
        topRow->setContentsMargins(0, 0, 0, 0);
        topRow->setSpacing(6);
        topRow->addWidget(new QLabel(SettingsWindow::tr("Every"), this));

        secondsSpin_ = new QSpinBox(this);
        secondsSpin_->setRange(1, 86400);
        secondsSpin_->setSuffix(SettingsWindow::tr(" s"));
        secondsSpin_->setKeyboardTracking(false);
        secondsSpin_->setAccessibleName(SettingsWindow::tr("Interval duration in seconds"));
        topRow->addWidget(secondsSpin_);

        onlyChangedCheck_ = new QCheckBox(SettingsWindow::tr("Only save when changed"), this);
        topRow->addWidget(onlyChangedCheck_);
        topRow->addStretch(1);
        layout->addLayout(topRow);

        auto *limitsRow = new QHBoxLayout;
        limitsRow->setContentsMargins(0, 0, 0, 0);
        limitsRow->setSpacing(6);
        limitsRow->addWidget(new QLabel(SettingsWindow::tr("Stop after:"), this));

        maxCapturesSpin_ = new QSpinBox(this);
        maxCapturesSpin_->setRange(0, 100000);
        maxCapturesSpin_->setSpecialValueText(SettingsWindow::tr("No capture limit"));
        maxCapturesSpin_->setSuffix(SettingsWindow::tr(" captures"));
        maxCapturesSpin_->setKeyboardTracking(false);
        maxCapturesSpin_->setAccessibleName(SettingsWindow::tr("Maximum captures per interval run"));
        limitsRow->addWidget(maxCapturesSpin_);

        maxMinutesSpin_ = new QSpinBox(this);
        maxMinutesSpin_->setRange(0, 10080);
        maxMinutesSpin_->setSpecialValueText(SettingsWindow::tr("No time limit"));
        maxMinutesSpin_->setSuffix(SettingsWindow::tr(" min"));
        maxMinutesSpin_->setKeyboardTracking(false);
        maxMinutesSpin_->setAccessibleName(SettingsWindow::tr("Maximum interval duration in minutes"));
        limitsRow->addWidget(maxMinutesSpin_);
        limitsRow->addStretch(1);
        layout->addLayout(limitsRow);

        const auto notifyChanged = [this] {
            if (!updating_ && onChanged_) {
                onChanged_();
            }
        };
        connect(secondsSpin_, &QSpinBox::valueChanged, this, notifyChanged);
        connect(maxCapturesSpin_, &QSpinBox::valueChanged, this, notifyChanged);
        connect(maxMinutesSpin_, &QSpinBox::valueChanged, this, notifyChanged);
        connect(onlyChangedCheck_, &QCheckBox::toggled, this, notifyChanged);
    }

    void setOnChanged(std::function<void()> callback)
    {
        onChanged_ = std::move(callback);
    }

    void setConfig(const IntervalConfig &cfg)
    {
        updating_ = true;
        secondsSpin_->setValue(std::clamp(cfg.seconds, 1, 86400));
        maxCapturesSpin_->setValue(std::clamp(cfg.maxCaptures, 0, 100000));
        maxMinutesSpin_->setValue(std::clamp(cfg.maxMinutes, 0, 10080));
        onlyChangedCheck_->setChecked(cfg.onlyWhenChanged);
        updating_ = false;
    }

    [[nodiscard]] IntervalConfig config() const
    {
        return {secondsSpin_->value(), maxCapturesSpin_->value(), maxMinutesSpin_->value(),
                onlyChangedCheck_->isChecked()};
    }

private:
    bool updating_ = false;
    QSpinBox *secondsSpin_ = nullptr;
    QSpinBox *maxCapturesSpin_ = nullptr;
    QSpinBox *maxMinutesSpin_ = nullptr;
    QCheckBox *onlyChangedCheck_ = nullptr;
    std::function<void()> onChanged_;
};

class HookOverrideWidget : public QWidget
{
public:
    explicit HookOverrideWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);

        modeCombo_ = new QComboBox(this);
        modeCombo_->setAccessibleName(SettingsWindow::tr("Post-capture hook mode"));
        modeCombo_->addItem(SettingsWindow::tr("Use global hook setting"), int(HookOverrideMode::UseGlobal));
        modeCombo_->addItem(SettingsWindow::tr("Custom command"), int(HookOverrideMode::Custom));
        modeCombo_->addItem(SettingsWindow::tr("Disable hook"), int(HookOverrideMode::Disabled));
        layout->addWidget(modeCombo_);

        customRow_ = new QWidget(this);
        auto *customLayout = new QHBoxLayout(customRow_);
        customLayout->setContentsMargins(0, 0, 0, 0);
        customLayout->setSpacing(6);

        commandEdit_ = new QLineEdit(customRow_);
        commandEdit_->setPlaceholderText(SettingsWindow::tr("/path/to/script %f"));
        commandEdit_->setAccessibleName(SettingsWindow::tr("Custom hook command"));
        customLayout->addWidget(commandEdit_, 1);

        timeoutSpin_ = new QSpinBox(customRow_);
        timeoutSpin_->setRange(5, 300);
        timeoutSpin_->setSuffix(SettingsWindow::tr(" s"));
        timeoutSpin_->setKeyboardTracking(false);
        timeoutSpin_->setAccessibleName(SettingsWindow::tr("Custom hook timeout"));
        customLayout->addWidget(timeoutSpin_);

        customRow_->hide();
        layout->addWidget(customRow_);

        connect(modeCombo_, &QComboBox::currentIndexChanged, this, [this] {
            customRow_->setVisible(HookOverrideMode(modeCombo_->currentData().toInt()) == HookOverrideMode::Custom);
            if (!updating_ && onChanged_) {
                onChanged_();
            }
        });
        connect(commandEdit_, &QLineEdit::editingFinished, this, [this] {
            if (!updating_ && onChanged_) {
                onChanged_();
            }
        });
        connect(timeoutSpin_, &QSpinBox::valueChanged, this, [this] {
            if (!updating_ && onChanged_) {
                onChanged_();
            }
        });
    }

    void setOnChanged(std::function<void()> callback)
    {
        onChanged_ = std::move(callback);
    }

    void setOverride(const HookOverride &ov)
    {
        updating_ = true;
        const int idx = modeCombo_->findData(int(ov.mode));
        modeCombo_->setCurrentIndex(idx >= 0 ? idx : 0);
        if (!commandEdit_->hasFocus()) {
            commandEdit_->setText(ov.command);
        }
        timeoutSpin_->setValue(std::clamp(ov.timeoutSeconds, 5, 300));
        customRow_->setVisible(ov.mode == HookOverrideMode::Custom);
        updating_ = false;
    }

    [[nodiscard]] HookOverride overrideValue() const
    {
        return {HookOverrideMode(modeCombo_->currentData().toInt()), commandEdit_->text().trimmed(),
                timeoutSpin_->value()};
    }

private:
    bool updating_ = false;
    QComboBox *modeCombo_ = nullptr;
    QWidget *customRow_ = nullptr;
    QLineEdit *commandEdit_ = nullptr;
    QSpinBox *timeoutSpin_ = nullptr;
    std::function<void()> onChanged_;
};

SettingsWindow::SettingsWindow(App *app, QWidget *parent)
    : KPageDialog(parent)
    , app_(app)
{
    setWindowTitle(tr("Configure Itsova"));
    setWindowIcon(QIcon::fromTheme(QStringLiteral("io.github.m00sfett.itsova"),
                                   QIcon::fromTheme(QStringLiteral("camera-photo"))));
    setFaceType(KPageDialog::List);
    setStandardButtons(QDialogButtonBox::Close);
    setAttribute(Qt::WA_DeleteOnClose);

    targetsPage_ = addPage(createTargetsPage(), tr("Targets"));
    auto *targets = targetsPage_;
    targets->setHeader(tr("Areas and monitors"));
    targets->setIcon(QIcon::fromTheme(QStringLiteral("transform-crop")));
    groupsPage_ = addPage(createGroupsPage(), tr("Groups"));
    auto *groups = groupsPage_;
    groups->setHeader(tr("Capture several targets with one shortcut"));
    groups->setIcon(QIcon::fromTheme(QStringLiteral("view-group")));
    auto *storage = addPage(createStoragePage(), tr("Storage"));
    storage->setHeader(tr("Where captures are saved"));
    storage->setIcon(QIcon::fromTheme(QStringLiteral("folder-pictures")));
    auto *general = addPage(createGeneralPage(), tr("General"));
    general->setHeader(tr("General"));
    general->setIcon(QIcon::fromTheme(QStringLiteral("configure")));
    auto *replay = addPage(createReplayPage(), tr("Replay"));
    replay->setHeader(tr("Fullscreen video replay"));
    replay->setIcon(QIcon::fromTheme(QStringLiteral("media-playback-start")));
    auto *automation = addPage(createAutomationPage(), tr("Automation"));
    automation->setHeader(tr("Post-capture hooks and external automation"));
    automation->setIcon(QIcon::fromTheme(QStringLiteral("system-run")));

    connect(app_, &App::targetsChanged, this, &SettingsWindow::refresh);
    connect(app_->shortcuts(), &ShortcutManager::shortcutChanged, this, [this] {
        if (!updating_) {
            refresh();
        }
    });
    addFooter();
    resize(860, 600);
    refresh();
    // Probed once per window: the list must not be rebuilt while the user ticks a language.
    refreshTextRecognition();
}

void SettingsWindow::addFooter()
{
    auto *footer = new QWidget(this);
    auto *row = new QHBoxLayout(footer);
    row->setContentsMargins(10, 0, 6, 4);
    row->setSpacing(4);

    // Link text: plain, readable label with the links in the theme's link colour.
    auto *about = new QLabel(footer);
    about->setTextFormat(Qt::RichText);
    about->setOpenExternalLinks(true);
    about->setTextInteractionFlags(Qt::TextBrowserInteraction);
    about->setText(tr("Itsova v%1 · AGPL-3.0 · <a href=\"%2\">roteson.de</a> · <a href=\"%3\">GitHub</a>")
                       .arg(QCoreApplication::applicationVersion(), websiteUrl(), projectUrl()));
    QFont small = about->font();
    small.setPointSizeF(small.pointSizeF() * 0.9);
    about->setFont(small);
    row->addWidget(about);
    row->addStretch();

    const auto addMark = [&](const QString &resource, const QString &tooltip, const QString &url) {
        auto *mark = new QToolButton(footer);
        mark->setIcon(QIcon(resource));
        mark->setIconSize(QSize(28, 28));
        mark->setAutoRaise(true);
        mark->setToolTip(tooltip);
        mark->setAccessibleName(tooltip);
        mark->setCursor(Qt::PointingHandCursor);
        connect(mark, &QToolButton::clicked, this, [url] { QDesktopServices::openUrl(QUrl(url)); });
        row->addWidget(mark);
    };
    addMark(QStringLiteral(":/brand/brand-roteson.png"), tr("roteson.de"), websiteUrl());
    addMark(QStringLiteral(":/brand/brand-r2w2.png"), tr("R²W² Development · Itsova on GitHub"), projectUrl());

    // A full-width line under the pages and the Close button.
    if (auto *top = qobject_cast<QBoxLayout *>(layout())) {
        top->addWidget(footer);
    }
}

// ---------------------------------------------------------------- Targets

QWidget *SettingsWindow::createTargetsPage()
{
    auto *page = new QWidget;
    auto *layout = new QHBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *splitter = new QSplitter(page);
    layout->addWidget(splitter);

    auto *listSide = new QWidget(splitter);
    auto *listLayout = new QVBoxLayout(listSide);
    listLayout->setContentsMargins(0, 0, 0, 0);
    targetTree_ = new QTreeWidget(listSide);
    targetTree_->setAccessibleName(tr("Targets"));
    targetTree_->setHeaderLabels({tr("Target"), tr("Shortcut")});
    targetTree_->setRootIsDecorated(true);
    targetTree_->setUniformRowHeights(true);
    targetTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    targetTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    targetTree_->header()->setStretchLastSection(false);
    listLayout->addWidget(targetTree_);
    auto *buttons = new QHBoxLayout;
    auto *addButton = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), tr("Add Area…"), listSide);
    addButton->setToolTip(tr("Drag a rectangle on any monitor"));
    removeAreaButton_ = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), tr("Remove"), listSide);
    buttons->addWidget(addButton);
    buttons->addWidget(removeAreaButton_);
    buttons->addStretch();
    listLayout->addLayout(buttons);

    targetDetails_ = new QWidget(splitter);
    auto *form = new QFormLayout(targetDetails_);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setContentsMargins(12, 0, 0, 0);
    targetTitle_ = new QLabel(targetDetails_);
    QFont titleFont = targetTitle_->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.2);
    targetTitle_->setFont(titleFont);
    form->addRow(targetTitle_);

    areaName_ = new QLineEdit(targetDetails_);
    areaName_->setAccessibleName(tr("Name"));
    areaNameError_ = makeError(targetDetails_);
    auto *nameBox = new QWidget(targetDetails_);
    auto *nameLayout = new QVBoxLayout(nameBox);
    nameLayout->setContentsMargins(0, 0, 0, 0);
    nameLayout->addWidget(areaName_);
    nameLayout->addWidget(areaNameError_);
    nameHint_ = makeHint(tr("Also used in the file name."), nameBox);
    nameLayout->addWidget(nameHint_);
    form->addRow(tr("Name:"), nameBox);
    nameRow_ = {nameBox, form->labelForField(nameBox)};

    monitorLabel_ = new QLabel(targetDetails_);
    monitorLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Monitor:"), monitorLabel_);

    geometryRow_ = new QWidget(targetDetails_);
    auto *geometryLayout = new QVBoxLayout(geometryRow_);
    geometryLayout->setContentsMargins(0, 0, 0, 0);
    const auto makeSpin = [this](const QString &prefix, const QString &name) {
        auto *spin = new QSpinBox(geometryRow_);
        spin->setPrefix(prefix);
        spin->setAccessibleName(name);
        spin->setRange(0, 100000);
        spin->setAccelerated(true);
        spin->setKeyboardTracking(false);
        connect(spin, &QSpinBox::valueChanged, this, &SettingsWindow::applyAreaGeometry);
        return spin;
    };
    areaX_ = makeSpin(tr("X: "), tr("Left edge"));
    areaY_ = makeSpin(tr("Y: "), tr("Top edge"));
    areaWidth_ = makeSpin(tr("Width: "), tr("Width"));
    areaHeight_ = makeSpin(tr("Height: "), tr("Height"));
    areaWidth_->setMinimum(1);
    areaHeight_->setMinimum(1);
    auto *positionRow = new QHBoxLayout;
    positionRow->addWidget(areaX_);
    positionRow->addWidget(areaY_);
    auto *sizeRow = new QHBoxLayout;
    sizeRow->addWidget(areaWidth_);
    sizeRow->addWidget(areaHeight_);
    geometryLayout->addLayout(positionRow);
    geometryLayout->addLayout(sizeRow);
    auto *reselect = new QPushButton(QIcon::fromTheme(QStringLiteral("transform-crop")), tr("Select on Screen…"),
                                     geometryRow_);
    auto *reselectRow = new QHBoxLayout;
    reselectRow->addWidget(reselect);
    reselectRow->addStretch();
    geometryLayout->addLayout(reselectRow);
    geometryHint_ = makeHint(tr("Logical pixels relative to the monitor's top left corner. Captures use "
                                "the monitor's native resolution."),
                             geometryRow_);
    geometryLayout->addWidget(geometryHint_);
    form->addRow(tr("Position and size:"), geometryRow_);
    areaOnly_ << geometryRow_ << form->labelForField(geometryRow_);

    areaWarning_ = makeError(targetDetails_);
    form->addRow(QString(), areaWarning_);

    targetCursor_ = new QCheckBox(tr("Include the mouse pointer"), targetDetails_);
    form->addRow(tr("Pointer:"), targetCursor_);

    targetExport_ = new ExportOptionsWidget(true, targetDetails_);
    form->addRow(tr("Format:"), targetExport_);

    targetDelay_ = new QSpinBox(targetDetails_);
    targetDelay_->setRange(0, 60);
    targetDelay_->setSpecialValueText(tr("None"));
    targetDelay_->setSuffix(tr(" s"));
    targetDelay_->setKeyboardTracking(false);
    targetDelay_->setAccessibleName(tr("Capture delay"));
    form->addRow(tr("Delay:"), targetDelay_);

    targetInterval_ = new IntervalConfigWidget(targetDetails_);
    form->addRow(tr("Interval:"), targetInterval_);

    targetHook_ = new HookOverrideWidget(targetDetails_);
    form->addRow(tr("Hook:"), targetHook_);

    targetShortcut_ = new KKeySequenceWidget(targetDetails_);
    targetShortcut_->setAccessibleName(tr("Shortcut"));
    targetShortcut_->setCheckForConflictsAgainst(KKeySequenceWidget::GlobalShortcuts
                                                 | KKeySequenceWidget::StandardShortcuts);
    targetShortcut_->setMultiKeyShortcutsAllowed(false);
    form->addRow(tr("Shortcut:"), targetShortcut_);

    targetIntervalShortcut_ = new KKeySequenceWidget(targetDetails_);
    targetIntervalShortcut_->setAccessibleName(tr("Interval toggle shortcut"));
    targetIntervalShortcut_->setCheckForConflictsAgainst(KKeySequenceWidget::GlobalShortcuts
                                                         | KKeySequenceWidget::StandardShortcuts);
    targetIntervalShortcut_->setMultiKeyShortcutsAllowed(false);
    form->addRow(tr("Interval shortcut:"), targetIntervalShortcut_);
    form->addRow(QString(), makeHint(tr("Global: works in every application. Also editable in System Settings."),
                                     targetDetails_));

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);

    connect(targetTree_, &QTreeWidget::currentItemChanged, this, &SettingsWindow::refreshTargetDetails);
    connect(addButton, &QPushButton::clicked, this, &SettingsWindow::addArea);
    connect(removeAreaButton_, &QPushButton::clicked, this, &SettingsWindow::removeSelectedArea);
    connect(areaName_, &QLineEdit::editingFinished, this, &SettingsWindow::renameArea);
    connect(reselect, &QPushButton::clicked, this, [this] {
        if (const Area *area = app_->store().area(selectedTargetId())) {
            QRect initialGlobal;
            if (area->isCrossMonitor()) {
                initialGlobal = area->rect;
            } else if (const QScreen *screen = findScreen(area->connector)) {
                initialGlobal = area->rect.translated(screen->geometry().topLeft());
            }
            runSelector(initialGlobal, area->id);
        }
    });
    connect(targetCursor_, &QCheckBox::toggled, this, [this](bool on) {
        if (updating_) {
            return;
        }
        TargetStore &store = app_->store();
        const QString id = selectedTargetId();
        if (Area *area = store.area(id)) {
            area->includeCursor = on;
        } else if (id == allMonitorsTargetId()) {
            store.allMonitorsCursor = on;
        } else if (isScreenTargetId(id)) {
            const QString connector = connectorFromScreenTargetId(id);
            store.cursorScreens.removeAll(connector);
            if (on) {
                store.cursorScreens.append(connector);
            }
        }
        app_->storeChanged();
    });
    targetExport_->setOnChanged([this] {
        if (updating_) {
            return;
        }
        const QString id = selectedTargetId();
        if (!id.isEmpty()) {
            app_->store().setTargetExportOverride(id, targetExport_->overrideValue());
            app_->storeChanged();
        }
    });
    connect(targetDelay_, &QSpinBox::valueChanged, this, [this](int secs) {
        if (updating_) {
            return;
        }
        const QString id = selectedTargetId();
        if (!id.isEmpty()) {
            app_->store().setTargetDelaySeconds(id, secs);
            app_->storeChanged();
        }
    });
    targetInterval_->setOnChanged([this] {
        if (updating_) {
            return;
        }
        const QString id = selectedTargetId();
        if (!id.isEmpty()) {
            app_->store().setTargetIntervalConfig(id, targetInterval_->config());
            app_->storeChanged();
        }
    });
    targetHook_->setOnChanged([this] {
        if (updating_) {
            return;
        }
        const QString id = selectedTargetId();
        if (!id.isEmpty()) {
            app_->store().setTargetHookOverride(id, targetHook_->overrideValue());
            app_->storeChanged();
        }
    });
    connect(targetShortcut_, &KKeySequenceWidget::keySequenceChanged, this, [this](const QKeySequence &sequence) {
        if (!updating_) {
            assignShortcut(selectedTargetId(), sequence, targetShortcut_);
        }
    });
    connect(targetIntervalShortcut_, &KKeySequenceWidget::keySequenceChanged, this,
            [this](const QKeySequence &sequence) {
                if (!updating_) {
                    assignShortcut(intervalShortcutId(selectedTargetId()), sequence, targetIntervalShortcut_);
                }
            });
    return page;
}

void SettingsWindow::refreshTargets()
{
    const QString current = selectedTargetId();
    const TargetStore &store = app_->store();
    const QList<ScreenInfo> screens = currentScreens();
    ShortcutManager *shortcuts = app_->shortcuts();

    QStringList connectors;
    for (const ScreenInfo &screen : screens) {
        connectors.append(screen.connector);
    }
    QStringList disconnected;
    for (const Area &area : store.areas) {
        if (!area.isCrossMonitor() && !connectors.contains(area.connector) && !disconnected.contains(area.connector)) {
            disconnected.append(area.connector);
        }
    }

    targetTree_->clear();
    const auto addMonitor = [&](const QString &connector, bool connected) {
        const QString id = screenTargetId(connector);
        auto *item = new QTreeWidgetItem(targetTree_);
        item->setText(0, connected ? monitorName(store, connector) : tr("%1 (not connected)").arg(connector));
        item->setIcon(0, QIcon::fromTheme(QStringLiteral("video-display")));
        item->setText(1, shortcuts->shortcut(id).toString(QKeySequence::NativeText));
        item->setData(0, idRole, id);
        for (const Area &area : store.areas) {
            if (area.isCrossMonitor() || area.connector != connector) {
                continue;
            }
            auto *child = new QTreeWidgetItem(item);
            child->setText(0, area.name);
            child->setIcon(0, QIcon::fromTheme(QStringLiteral("transform-crop")));
            child->setText(1, shortcuts->shortcut(area.id).toString(QKeySequence::NativeText));
            child->setData(0, idRole, area.id);
        }
        item->setExpanded(true);
    };
    for (const QString &connector : std::as_const(connectors)) {
        addMonitor(connector, true);
    }
    for (const QString &connector : std::as_const(disconnected)) {
        addMonitor(connector, false);
    }
    auto *all = new QTreeWidgetItem(targetTree_);
    all->setText(0, tr("All monitors"));
    all->setIcon(0, QIcon::fromTheme(QStringLiteral("video-display")));
    all->setText(1, shortcuts->shortcut(allMonitorsTargetId()).toString(QKeySequence::NativeText));
    all->setData(0, idRole, allMonitorsTargetId());
    for (const Area &area : store.areas) {
        if (!area.isCrossMonitor()) {
            continue;
        }
        auto *child = new QTreeWidgetItem(all);
        child->setText(0, area.name);
        child->setIcon(0, QIcon::fromTheme(QStringLiteral("transform-crop")));
        child->setText(1, shortcuts->shortcut(area.id).toString(QKeySequence::NativeText));
        child->setData(0, idRole, area.id);
    }
    all->setExpanded(true);

    selectTarget(current.isEmpty() && !store.areas.isEmpty() ? store.areas.first().id : current);
    if (!targetTree_->currentItem() && targetTree_->topLevelItemCount() > 0) {
        targetTree_->setCurrentItem(targetTree_->topLevelItem(0));
    }
    refreshTargetDetails();
}

void SettingsWindow::refreshTargetDetails()
{
    const QString id = selectedTargetId();
    const TargetStore &store = app_->store();
    const Area *area = store.area(id);
    const bool isArea = area != nullptr;
    removeAreaButton_->setEnabled(isArea);
    targetDetails_->setEnabled(!id.isEmpty());

    updating_ = true;
    for (QWidget *widget : std::as_const(areaOnly_)) {
        if (widget) {
            widget->setVisible(isArea);
        }
    }
    areaNameError_->hide();
    areaWarning_->hide();

    const bool crossArea = isArea && area->isCrossMonitor();
    const QString connector = isArea ? area->connector : connectorFromScreenTargetId(id);
    const QScreen *screen = connector.isEmpty() ? nullptr : findScreen(connector);
    const bool isMonitor = !isArea && screen && isScreenTargetId(id);
    for (QWidget *widget : std::as_const(nameRow_)) {
        if (widget) {
            widget->setVisible(isArea || isMonitor);
        }
    }
    areaName_->setPlaceholderText(isMonitor ? connector : QString());
    nameHint_->setText(isMonitor ? tr("Also used in file names and for new areas on this monitor. Leave empty "
                                      "for %1.")
                                       .arg(connector)
                                 : tr("Also used in the file name."));

    const QList<ScreenInfo> allConnectedScreens = currentScreens();
    bool targetAvailable = false;
    if (crossArea) {
        QStringList allNames;
        QStringList connectedNames;
        QStringList missingConnectors;
        QString mismatchConnector;
        QString mismatchIdentity;
        int connectedBindings = 0;
        for (const AreaMonitorBinding &binding : area->monitors) {
            const auto it = std::find_if(allConnectedScreens.cbegin(), allConnectedScreens.cend(),
                                         [&](const ScreenInfo &s) { return s.connector == binding.connector; });
            if (it != allConnectedScreens.cend()) {
                ++connectedBindings;
                const QString mName = store.monitorName(*it);
                allNames.append(mName);
                if (!area->rect.intersected(it->geometry).isEmpty()) {
                    connectedNames.append(mName);
                }
                if (mismatchConnector.isEmpty() && !binding.monitorIdentity.isEmpty() && !it->identity().isEmpty()
                    && binding.monitorIdentity != it->identity()) {
                    mismatchConnector = binding.connector;
                    mismatchIdentity = binding.monitorIdentity;
                }
            } else {
                allNames.append(binding.connector);
                missingConnectors.append(binding.connector);
            }
        }
        targetAvailable = !connectedNames.isEmpty();
        targetTitle_->setText(area->name);
        areaName_->setText(area->name);
        monitorLabel_->setWordWrap(true);
        monitorLabel_->setText(tr("Cross-monitor (%1)").arg(allNames.join(QStringLiteral(" + "))));
        geometryHint_->setText(
            tr("Global logical coordinates across monitors. Captures compose intersected monitors at the highest scale."));

        QRect desktopBounds;
        for (const ScreenInfo &s : allConnectedScreens) {
            desktopBounds = desktopBounds.united(s.geometry);
        }
        areaX_->setRange(desktopBounds.isEmpty() ? -100000 : desktopBounds.left(),
                         desktopBounds.isEmpty() ? 100000 : desktopBounds.left() + desktopBounds.width() - 1);
        areaY_->setRange(desktopBounds.isEmpty() ? -100000 : desktopBounds.top(),
                         desktopBounds.isEmpty() ? 100000 : desktopBounds.top() + desktopBounds.height() - 1);
        areaWidth_->setMaximum(desktopBounds.isEmpty() ? 100000 : std::max(1, desktopBounds.width()));
        areaHeight_->setMaximum(desktopBounds.isEmpty() ? 100000 : std::max(1, desktopBounds.height()));
        areaX_->setValue(area->rect.x());
        areaY_->setValue(area->rect.y());
        areaWidth_->setValue(area->rect.width());
        areaHeight_->setValue(area->rect.height());
        geometryRow_->setEnabled(!allConnectedScreens.isEmpty());

        if (connectedBindings == 0) {
            areaWarning_->setText(tr("None of this area's monitors are connected. Nothing is saved when triggered."));
            areaWarning_->show();
        } else if (connectedNames.isEmpty()) {
            areaWarning_->setText(
                tr("This area no longer overlaps any connected monitor. Adjust it or select it again."));
            areaWarning_->show();
        } else if (!missingConnectors.isEmpty()) {
            areaWarning_->setText(tr("%1 not connected. Triggering saves the visible part on %2.")
                                      .arg(missingConnectors.join(QStringLiteral(", ")),
                                           connectedNames.join(QStringLiteral(", "))));
            areaWarning_->show();
        } else if (!mismatchConnector.isEmpty()) {
            areaWarning_->setText(tr("A different monitor is connected to %1 than when this area was defined (%2).")
                                      .arg(mismatchConnector, mismatchIdentity));
            areaWarning_->show();
        }
    } else {
        targetAvailable = isArea || screen || id == allMonitorsTargetId();
        if (id == allMonitorsTargetId()) {
            targetTitle_->setText(tr("All monitors"));
            monitorLabel_->setText(tr("One image of all monitors in their current arrangement, at the highest scale. "
                                      "Gaps are transparent."));
            monitorLabel_->setWordWrap(true);
        } else if (screen) {
            monitorLabel_->setWordWrap(false);
            // Qt only reports the integer buffer scale on Wayland, so the native size is not shown here.
            monitorLabel_->setText(tr("%1 · %2 × %3 (logical)")
                                       .arg(connector)
                                       .arg(screen->geometry().width())
                                       .arg(screen->geometry().height()));
        } else {
            monitorLabel_->setWordWrap(false);
            monitorLabel_->setText(tr("%1 · not connected").arg(connector));
        }
        if (isArea) {
            targetTitle_->setText(area->name);
            areaName_->setText(area->name);
            geometryHint_->setText(tr("Logical pixels relative to the monitor's top left corner. Captures use "
                                      "the monitor's native resolution."));
            const QSize bounds = screen ? screen->geometry().size() : QSize(100000, 100000);
            areaX_->setRange(0, std::max(0, bounds.width() - 1));
            areaY_->setRange(0, std::max(0, bounds.height() - 1));
            areaWidth_->setMaximum(bounds.width());
            areaHeight_->setMaximum(bounds.height());
            areaX_->setValue(area->rect.x());
            areaY_->setValue(area->rect.y());
            areaWidth_->setValue(area->rect.width());
            areaHeight_->setValue(area->rect.height());
            geometryRow_->setEnabled(screen != nullptr);
            if (!screen) {
                areaWarning_->setText(tr("The monitor is not connected. This area is skipped when triggered."));
                areaWarning_->show();
            } else if (!QRect(QPoint(), screen->geometry().size()).contains(area->rect)) {
                areaWarning_->setText(tr("The area no longer fits on this monitor and is skipped when triggered. "
                                           "Adjust it or select it again."));
                areaWarning_->show();
            } else if (!area->monitorIdentity.isEmpty() && screenInfo(screen).identity() != area->monitorIdentity) {
                areaWarning_->setText(tr("A different monitor is connected to %1 than when this area was defined "
                                           "(%2).")
                                            .arg(connector, area->monitorIdentity));
                areaWarning_->show();
            }
        } else if (screen) {
            const QString name = store.monitorName(screenInfo(screen));
            targetTitle_->setText(tr("Monitor %1").arg(name));
            areaName_->setText(name == connector ? QString() : name);
        } else if (id != allMonitorsTargetId()) {
            targetTitle_->setText(tr("Monitor %1").arg(connector));
        }
    }
    Q_UNUSED(targetAvailable);

    targetCursor_->setChecked(store.targetIncludesCursor(id));
    targetCursor_->setEnabled(isArea || screen || id == allMonitorsTargetId());
    targetExport_->setOverride(store.targetExportOverride(id), store.exportOptions.format);
    targetExport_->setEnabled(isArea || screen || id == allMonitorsTargetId());
    targetDelay_->setValue(store.targetDelaySeconds(id));
    targetDelay_->setEnabled(isArea || screen || id == allMonitorsTargetId());
    targetInterval_->setConfig(store.targetIntervalConfig(id));
    targetInterval_->setEnabled(isArea || screen || id == allMonitorsTargetId());
    targetHook_->setOverride(store.targetHookOverride(id));
    targetHook_->setEnabled(isArea || screen || id == allMonitorsTargetId());
    targetShortcut_->setKeySequence(app_->shortcuts()->shortcut(id));
    targetShortcut_->setEnabled(isArea || screen || id == allMonitorsTargetId());
    targetIntervalShortcut_->setKeySequence(app_->shortcuts()->shortcut(intervalShortcutId(id)));
    targetIntervalShortcut_->setEnabled(isArea || screen || id == allMonitorsTargetId());
    updating_ = false;
}

QString SettingsWindow::selectedTargetId() const
{
    const QTreeWidgetItem *item = targetTree_->currentItem();
    return item ? item->data(0, idRole).toString() : QString();
}

void SettingsWindow::selectTarget(const QString &id)
{
    for (QTreeWidgetItemIterator it(targetTree_); *it; ++it) {
        if ((*it)->data(0, idRole).toString() == id) {
            targetTree_->setCurrentItem(*it);
            return;
        }
    }
}

void SettingsWindow::addArea()
{
    setCurrentPage(targetsPage_);
    runSelector(QRect(), QString());
}

void SettingsWindow::showTarget(const QString &id)
{
    if (app_->store().group(id)) {
        setCurrentPage(groupsPage_);
        selectGroup(id);
    } else {
        setCurrentPage(targetsPage_);
        selectTarget(id);
    }
}

void SettingsWindow::editArea(const QString &areaId)
{
    if (const Area *area = app_->store().area(areaId)) {
        setCurrentPage(targetsPage_);
        selectTarget(areaId);
        QRect initialGlobal;
        if (area->isCrossMonitor()) {
            initialGlobal = area->rect;
        } else if (const QScreen *screen = findScreen(area->connector)) {
            initialGlobal = area->rect.translated(screen->geometry().topLeft());
        }
        runSelector(initialGlobal, area->id);
    }
}

void SettingsWindow::runSelector(const QRect &initialGlobalRect, const QString &areaId)
{
    if (selector_) {
        return;
    }
    selector_ = new AreaSelector(this);
    connect(selector_, &AreaSelector::selected, this,
            [this, areaId](const QRect &globalRect) {
                selector_->deleteLater();
                TargetStore &store = app_->store();
                const QList<ScreenInfo> screens = currentScreens();
                const QList<ScreenInfo> hit = intersectingScreens(screens, globalRect);
                QString id = areaId;
                if (Area *area = store.area(areaId)) {
                    if (hit.size() == 1 && hit.first().geometry.contains(globalRect)) {
                        area->connector = hit.first().connector;
                        area->monitorIdentity = hit.first().identity();
                        area->monitors.clear();
                        area->rect = globalRect.translated(-hit.first().geometry.topLeft());
                    } else if (!hit.isEmpty()) {
                        area->connector.clear();
                        area->monitorIdentity.clear();
                        area->monitors.clear();
                        for (const ScreenInfo &s : hit) {
                            area->monitors.append({s.connector, s.identity()});
                        }
                        area->rect = globalRect;
                    }
                } else if (!hit.isEmpty()) {
                    id = store.addAreaForGlobalRect(screens, globalRect).id;
                }
                app_->storeChanged();
                show();
                raise();
                activateWindow();
                setCurrentPage(targetsPage_);
                selectTarget(id);
                if (areaId.isEmpty() && !id.isEmpty()) {
                    // A fresh area is most useful with a shortcut: start recording right away.
                    targetShortcut_->captureKeySequence();
                }
            });
    connect(selector_, &AreaSelector::cancelled, this, [this] {
        selector_->deleteLater();
        show();
        raise();
        activateWindow();
    });
    hide();
    QTimer::singleShot(hideBeforeSelectDelayMs, selector_, [this, selector = selector_.data(), initialGlobalRect] {
        selector->start(initialGlobalRect, app_->store().showSelectionLoupe);
    });
}

void SettingsWindow::applyAreaGeometry()
{
    if (updating_) {
        return;
    }
    Area *area = app_->store().area(selectedTargetId());
    if (!area) {
        return;
    }
    if (area->isCrossMonitor()) {
        const QList<ScreenInfo> screens = currentScreens();
        QRect desktopBounds;
        for (const ScreenInfo &s : screens) {
            desktopBounds = desktopBounds.united(s.geometry);
        }
        if (desktopBounds.isEmpty()) {
            return;
        }
        QRect rect(areaX_->value(), areaY_->value(), areaWidth_->value(), areaHeight_->value());
        rect = clampToBounds(rect, desktopBounds);
        area->rect = rect;
        const QList<ScreenInfo> hit = intersectingScreens(screens, rect);
        if (hit.size() >= 2) {
            area->monitors.clear();
            for (const ScreenInfo &s : hit) {
                area->monitors.append({s.connector, s.identity()});
            }
        }
        app_->storeChanged();
        return;
    }
    const QScreen *screen = findScreen(area->connector);
    if (!screen) {
        return;
    }
    const QSize bounds = screen->geometry().size();
    QRect rect(areaX_->value(), areaY_->value(), areaWidth_->value(), areaHeight_->value());
    // Keep the area on its monitor: shrink the size if the position pushes it over the edge.
    rect.setWidth(std::min(rect.width(), bounds.width() - rect.x()));
    rect.setHeight(std::min(rect.height(), bounds.height() - rect.y()));
    area->rect = rect;
    app_->storeChanged();
}

void SettingsWindow::renameArea()
{
    if (updating_) {
        return;
    }
    const QString id = selectedTargetId();
    if (isScreenTargetId(id)) {
        const QString error = app_->renameTarget(id, areaName_->text());
        areaNameError_->setText(error);
        areaNameError_->setVisible(!error.isEmpty());
        return;
    }
    Area *area = app_->store().area(id);
    if (!area) {
        return;
    }
    const QString name = areaName_->text().trimmed();
    if (name == area->name) {
        areaNameError_->hide();
        return;
    }
    QStringList connectors;
    for (const ScreenInfo &screen : currentScreens()) {
        connectors.append(screen.connector);
    }
    if (sanitizeFilenameToken(name).isEmpty()) {
        areaNameError_->setText(tr("Enter a name that can be used in a file name."));
        areaNameError_->show();
        return;
    }
    if (!app_->store().isNameAvailable(name, area->id, connectors)) {
        areaNameError_->setText(tr("“%1” is already used by another target or group.").arg(name));
        areaNameError_->show();
        return;
    }
    area->name = name;
    areaNameError_->hide();
    app_->storeChanged();
}

void SettingsWindow::removeSelectedArea()
{
    const QString id = selectedTargetId();
    const Area *area = app_->store().area(id);
    if (!area) {
        return;
    }
    const auto answer = QMessageBox::question(
        this, tr("Remove Area"),
        tr("Remove area “%1”? Its shortcut is cleared and it is removed from all groups. Saved captures are "
           "kept.")
            .arg(area->name),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) {
        return;
    }
    app_->removeTarget(id);
}

void SettingsWindow::assignShortcut(const QString &id, const QKeySequence &sequence, KKeySequenceWidget *editor)
{
    if (id.isEmpty()) {
        return;
    }
    ShortcutManager *shortcuts = app_->shortcuts();
    if (!sequence.isEmpty()) {
        QStringList owners;
        for (const Group &group : app_->store().groups) {
            owners.append(group.id);
            owners.append(intervalShortcutId(group.id));
        }
        for (const Area &area : app_->store().areas) {
            owners.append(area.id);
            owners.append(intervalShortcutId(area.id));
        }
        for (const ScreenInfo &screen : currentScreens()) {
            const QString sid = screenTargetId(screen.connector);
            owners.append(sid);
            owners.append(intervalShortcutId(sid));
        }
        owners.append(allMonitorsTargetId());
        owners.append(intervalShortcutId(allMonitorsTargetId()));
        owners.append(colorPickerTargetId());
        owners.append(recordToggleActionId());
        owners.append(recordPauseActionId());
        for (const QString &owner : std::as_const(owners)) {
            if (owner == id || shortcuts->shortcut(owner) != sequence) {
                continue;
            }
            const auto answer = QMessageBox::question(
                this, tr("Shortcut Already in Use"),
                tr("%1 is already assigned to “%2”. Move it to “%3”?")
                    .arg(sequence.toString(QKeySequence::NativeText), App::targetLabel(app_->store(), owner),
                         App::targetLabel(app_->store(), id)),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
            if (answer != QMessageBox::Yes) {
                updating_ = true;
                editor->setKeySequence(shortcuts->shortcut(id));
                updating_ = false;
                return;
            }
            updating_ = true;
            shortcuts->setShortcut(owner, {});
            updating_ = false;
        }
    }
    updating_ = true;
    shortcuts->setShortcut(id, sequence);
    updating_ = false;
    refresh();
}

// ---------------------------------------------------------------- Groups

QWidget *SettingsWindow::createGroupsPage()
{
    auto *page = new QWidget;
    auto *layout = new QHBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *splitter = new QSplitter(page);
    layout->addWidget(splitter);

    auto *listSide = new QWidget(splitter);
    auto *listLayout = new QVBoxLayout(listSide);
    listLayout->setContentsMargins(0, 0, 0, 0);
    groupList_ = new QListWidget(listSide);
    groupList_->setAccessibleName(tr("Groups"));
    listLayout->addWidget(groupList_);
    auto *buttons = new QHBoxLayout;
    auto *addButton = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), tr("Add Group"), listSide);
    removeGroupButton_ = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), tr("Remove"), listSide);
    buttons->addWidget(addButton);
    buttons->addWidget(removeGroupButton_);
    buttons->addStretch();
    listLayout->addLayout(buttons);

    groupDetails_ = new QWidget(splitter);
    auto *form = new QFormLayout(groupDetails_);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setContentsMargins(12, 0, 0, 0);
    groupName_ = new QLineEdit(groupDetails_);
    groupName_->setAccessibleName(tr("Group name"));
    groupNameError_ = makeError(groupDetails_);
    auto *nameBox = new QWidget(groupDetails_);
    auto *nameLayout = new QVBoxLayout(nameBox);
    nameLayout->setContentsMargins(0, 0, 0, 0);
    nameLayout->addWidget(groupName_);
    nameLayout->addWidget(groupNameError_);
    form->addRow(tr("Name:"), nameBox);
    groupMembers_ = new QListWidget(groupDetails_);
    groupMembers_->setAccessibleName(tr("Targets in this group"));
    form->addRow(tr("Targets:"), groupMembers_);
    form->addRow(QString(), makeHint(tr("Each monitor is captured once per trigger, so all files show the same "
                                        "moment and share one timestamp."),
                                     groupDetails_));
    groupCursor_ = new QCheckBox(tr("Include the mouse pointer"), groupDetails_);
    form->addRow(tr("Pointer:"), groupCursor_);
    form->addRow(QString(), makeHint(tr("Applies to every target of this group, regardless of their own setting."),
                                     groupDetails_));

    groupExport_ = new ExportOptionsWidget(true, groupDetails_);
    form->addRow(tr("Format:"), groupExport_);
    form->addRow(QString(), makeHint(tr("Used when this group is triggered, unless a member target sets its own "
                                        "format."),
                                     groupDetails_));

    groupDelay_ = new QSpinBox(groupDetails_);
    groupDelay_->setRange(0, 60);
    groupDelay_->setSpecialValueText(tr("None"));
    groupDelay_->setSuffix(tr(" s"));
    groupDelay_->setKeyboardTracking(false);
    groupDelay_->setAccessibleName(tr("Group capture delay"));
    form->addRow(tr("Delay:"), groupDelay_);

    groupInterval_ = new IntervalConfigWidget(groupDetails_);
    form->addRow(tr("Interval:"), groupInterval_);

    groupHook_ = new HookOverrideWidget(groupDetails_);
    form->addRow(tr("Hook:"), groupHook_);

    groupShortcut_ = new KKeySequenceWidget(groupDetails_);
    groupShortcut_->setAccessibleName(tr("Group shortcut"));
    groupShortcut_->setCheckForConflictsAgainst(KKeySequenceWidget::GlobalShortcuts
                                                | KKeySequenceWidget::StandardShortcuts);
    groupShortcut_->setMultiKeyShortcutsAllowed(false);
    form->addRow(tr("Shortcut:"), groupShortcut_);

    groupIntervalShortcut_ = new KKeySequenceWidget(groupDetails_);
    groupIntervalShortcut_->setAccessibleName(tr("Group interval toggle shortcut"));
    groupIntervalShortcut_->setCheckForConflictsAgainst(KKeySequenceWidget::GlobalShortcuts
                                                        | KKeySequenceWidget::StandardShortcuts);
    groupIntervalShortcut_->setMultiKeyShortcutsAllowed(false);
    form->addRow(tr("Interval shortcut:"), groupIntervalShortcut_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);

    connect(groupList_, &QListWidget::currentItemChanged, this, &SettingsWindow::refreshGroupDetails);
    connect(addButton, &QPushButton::clicked, this, [this] {
        TargetStore &store = app_->store();
        QString name;
        for (int n = 1;; ++n) {
            name = tr("Group %1").arg(n);
            if (store.isNameAvailable(name)) {
                break;
            }
        }
        const QString id = store.addGroup(name).id;
        app_->storeChanged();
        selectGroup(id);
        groupName_->setFocus();
        groupName_->selectAll();
    });
    connect(removeGroupButton_, &QPushButton::clicked, this, [this] {
        const QString id = selectedGroupId();
        const Group *group = app_->store().group(id);
        if (!group) {
            return;
        }
        const auto answer = QMessageBox::question(
            this, tr("Remove Group"),
            tr("Remove group “%1”? Its targets stay; only the group and its shortcut are removed.").arg(group->name),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer == QMessageBox::Yes) {
            app_->removeTarget(id);
        }
    });
    connect(groupName_, &QLineEdit::editingFinished, this, &SettingsWindow::renameGroup);
    connect(groupMembers_, &QListWidget::itemChanged, this, [this] {
        if (updating_) {
            return;
        }
        Group *group = app_->store().group(selectedGroupId());
        if (!group) {
            return;
        }
        QStringList members;
        for (int row = 0; row < groupMembers_->count(); ++row) {
            const QListWidgetItem *item = groupMembers_->item(row);
            if (item->checkState() == Qt::Checked) {
                members.append(item->data(idRole).toString());
            }
        }
        group->targetIds = members;
        app_->storeChanged();
    });
    connect(groupCursor_, &QCheckBox::toggled, this, [this](bool on) {
        if (Group *group = app_->store().group(selectedGroupId()); group && !updating_) {
            group->includeCursor = on;
            app_->storeChanged();
        }
    });
    groupExport_->setOnChanged([this] {
        if (Group *group = app_->store().group(selectedGroupId()); group && !updating_) {
            group->exportOverride = groupExport_->overrideValue();
            app_->storeChanged();
        }
    });
    connect(groupDelay_, &QSpinBox::valueChanged, this, [this](int secs) {
        if (Group *group = app_->store().group(selectedGroupId()); group && !updating_) {
            group->delaySeconds = secs;
            app_->storeChanged();
        }
    });
    groupInterval_->setOnChanged([this] {
        if (Group *group = app_->store().group(selectedGroupId()); group && !updating_) {
            group->interval = groupInterval_->config();
            app_->storeChanged();
        }
    });
    groupHook_->setOnChanged([this] {
        if (Group *group = app_->store().group(selectedGroupId()); group && !updating_) {
            group->hookOverride = groupHook_->overrideValue();
            app_->storeChanged();
        }
    });
    connect(groupShortcut_, &KKeySequenceWidget::keySequenceChanged, this, [this](const QKeySequence &sequence) {
        if (!updating_) {
            assignShortcut(selectedGroupId(), sequence, groupShortcut_);
        }
    });
    connect(groupIntervalShortcut_, &KKeySequenceWidget::keySequenceChanged, this,
            [this](const QKeySequence &sequence) {
                if (!updating_) {
                    assignShortcut(intervalShortcutId(selectedGroupId()), sequence, groupIntervalShortcut_);
                }
            });
    return page;
}

void SettingsWindow::refreshGroups()
{
    const QString current = selectedGroupId();
    updating_ = true;
    groupList_->clear();
    for (const Group &group : app_->store().groups) {
        auto *item = new QListWidgetItem(QIcon::fromTheme(QStringLiteral("view-group")), group.name, groupList_);
        item->setData(idRole, group.id);
        const QKeySequence shortcut = app_->shortcuts()->shortcut(group.id);
        if (!shortcut.isEmpty()) {
            item->setToolTip(shortcut.toString(QKeySequence::NativeText));
        }
    }
    updating_ = false;
    selectGroup(current);
    if (!groupList_->currentItem() && groupList_->count() > 0) {
        groupList_->setCurrentRow(0);
    }
    refreshGroupDetails();
}

void SettingsWindow::refreshGroupDetails()
{
    const Group *group = app_->store().group(selectedGroupId());
    removeGroupButton_->setEnabled(group);
    groupDetails_->setEnabled(group);
    updating_ = true;
    groupNameError_->hide();
    groupMembers_->clear();
    groupName_->setText(group ? group->name : QString());
    if (group) {
        const TargetStore &store = app_->store();
        const auto addMember = [&](const QString &id, const QString &label, const QString &icon) {
            auto *item = new QListWidgetItem(QIcon::fromTheme(icon), label, groupMembers_);
            item->setData(idRole, id);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(group->targetIds.contains(id) ? Qt::Checked : Qt::Unchecked);
        };
        for (const Area &area : store.areas) {
            if (area.isCrossMonitor()) {
                QStringList names;
                names.reserve(area.monitors.size());
                for (const AreaMonitorBinding &b : area.monitors) {
                    names.append(monitorName(store, b.connector));
                }
                addMember(area.id, tr("%1 (cross-monitor: %2)").arg(area.name, names.join(QStringLiteral(" + "))),
                          QStringLiteral("transform-crop"));
            } else {
                addMember(area.id, tr("%1 (area on %2)").arg(area.name, monitorName(store, area.connector)),
                          QStringLiteral("transform-crop"));
            }
        }
        QStringList connectors;
        for (const ScreenInfo &screen : currentScreens()) {
            connectors.append(screen.connector);
        }
        for (const QString &id : group->targetIds) {
            if (isScreenTargetId(id) && !connectors.contains(connectorFromScreenTargetId(id))) {
                connectors.append(connectorFromScreenTargetId(id));
            }
        }
        for (const QString &connector : std::as_const(connectors)) {
            addMember(screenTargetId(connector), tr("Monitor %1").arg(monitorName(store, connector)),
                      QStringLiteral("video-display"));
        }
        addMember(allMonitorsTargetId(), tr("All monitors"), QStringLiteral("video-display"));
    }
    groupCursor_->setChecked(group && group->includeCursor);
    groupExport_->setOverride(group ? group->exportOverride : ExportOverride{}, app_->store().exportOptions.format);
    groupDelay_->setValue(group ? group->delaySeconds : 0);
    groupInterval_->setConfig(group ? group->interval : IntervalConfig{});
    groupHook_->setOverride(group ? group->hookOverride : HookOverride{});
    groupShortcut_->setKeySequence(group ? app_->shortcuts()->shortcut(group->id) : QKeySequence());
    groupIntervalShortcut_->setKeySequence(
        group ? app_->shortcuts()->shortcut(intervalShortcutId(group->id)) : QKeySequence());
    updating_ = false;
}

QString SettingsWindow::selectedGroupId() const
{
    const QListWidgetItem *item = groupList_->currentItem();
    return item ? item->data(idRole).toString() : QString();
}

void SettingsWindow::selectGroup(const QString &id)
{
    for (int row = 0; row < groupList_->count(); ++row) {
        if (groupList_->item(row)->data(idRole).toString() == id) {
            groupList_->setCurrentRow(row);
            return;
        }
    }
}

void SettingsWindow::renameGroup()
{
    if (updating_) {
        return;
    }
    Group *group = app_->store().group(selectedGroupId());
    if (!group) {
        return;
    }
    const QString name = groupName_->text().trimmed();
    if (name == group->name) {
        groupNameError_->hide();
        return;
    }
    if (!app_->store().isNameAvailable(name, group->id)) {
        groupNameError_->setText(name.isEmpty() ? tr("Enter a name.")
                                                : tr("“%1” is already used by another target or group.").arg(name));
        groupNameError_->show();
        return;
    }
    group->name = name;
    groupNameError_->hide();
    app_->storeChanged();
}

// ---------------------------------------------------------------- Storage and general

QWidget *SettingsWindow::createStoragePage()
{
    auto *page = new QWidget;
    auto *outer = new QHBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *content = new QWidget(page);
    content->setMaximumWidth(680);
    outer->addWidget(content);
    auto *form = new QFormLayout(content);
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    directory_ = new QLineEdit(page);
    directory_->setPlaceholderText(TargetStore::defaultCaptureDirectory());
    directory_->setMinimumWidth(360);
    directory_->setAccessibleName(tr("Capture folder"));
    auto *browse = new QPushButton(QIcon::fromTheme(QStringLiteral("document-open-folder")), tr("Browse…"), page);
    auto *directoryRow = new QHBoxLayout;
    directoryRow->addWidget(directory_);
    directoryRow->addWidget(browse);
    form->addRow(tr("Folder:"), directoryRow);
    form->addRow(QString(), makeHint(tr("All captures go into this folder, without subfolders. Leave empty for the "
                                        "default."),
                                     page));

    template_ = new QLineEdit(page);
    template_->setPlaceholderText(TargetStore::defaultFilenameTemplate());
    template_->setAccessibleName(tr("File name template"));
    form->addRow(tr("File name:"), template_);
    storageExport_ = new ExportOptionsWidget(false, page);
    form->addRow(tr("Format:"), storageExport_);
    templatePreview_ = new QLabel(page);
    templatePreview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Example:"), templatePreview_);
    form->addRow(QString(), makeHint(tr("Placeholders: {date} (YYYY-MM-DD), {time} (HH-MM-SS), {area} (target "
                                        "name). The file extension matches the chosen format. Existing files are "
                                        "never overwritten: a number is appended instead."),
                                     page));

    historyLimit_ = new QSpinBox(page);
    historyLimit_->setRange(HistoryStore::MinLimit, HistoryStore::MaxLimit);
    historyLimit_->setSuffix(tr(" captures"));
    historyLimit_->setAccessibleName(tr("Number of recent captures to keep"));
    auto *clearHistory = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-clear-history")), tr("Clear History"),
                                         page);
    historyFeedback_ = new QLabel(page);
    historyFeedback_->setVisible(false);
    auto *historyRow = new QHBoxLayout;
    historyRow->addWidget(historyLimit_);
    historyRow->addWidget(clearHistory);
    historyRow->addWidget(historyFeedback_);
    historyRow->addStretch(1);
    form->addRow(tr("Recent captures:"), historyRow);
    form->addRow(QString(), makeHint(tr("Shown in the widget and the tray menu. Interval captures are not listed. "
                                        "Clearing the list never deletes image files."),
                                     page));
    connect(historyLimit_, &QSpinBox::valueChanged, this, [this](int value) {
        if (updating_) {
            return;
        }
        app_->store().historyLimit = value;
        app_->storeChanged();
    });
    connect(clearHistory, &QPushButton::clicked, this, [this] {
        app_->clearHistory();
        historyFeedback_->setText(tr("History cleared"));
        historyFeedback_->setVisible(true);
        QTimer::singleShot(3000, historyFeedback_, [label = historyFeedback_] { label->setVisible(false); });
    });

    // Screen recording (#30): same folder and file names as captures, so only the video settings.
    recordingFormat_ = new QComboBox(page);
    recordingFormat_->setAccessibleName(tr("Recording format"));
    recordingFormat_->addItem(tr("MP4 (H.264): plays everywhere"), int(RecordingFormat::Mp4));
    recordingFormat_->addItem(tr("GIF: animated image, no sound, larger files"), int(RecordingFormat::Gif));
    recordingFormat_->addItem(tr("WebM (VP9): small files for the web"), int(RecordingFormat::WebM));
    auto *formatRow = new QHBoxLayout;
    formatRow->addWidget(recordingFormat_);
    formatRow->addStretch(1);
    form->addRow(tr("Recordings:"), formatRow);
    recordingFrameRate_ = new QComboBox(page);
    recordingFrameRate_->setAccessibleName(tr("Recording frame rate"));
    for (const int rate : recordingFrameRates()) {
        recordingFrameRate_->addItem(tr("%1 fps").arg(rate), rate);
    }
    recordingCursor_ = new QCheckBox(tr("Show the mouse pointer"), page);
    auto *recordingRow = new QHBoxLayout;
    recordingRow->addWidget(recordingFrameRate_);
    recordingRow->addSpacing(12);
    recordingRow->addWidget(recordingCursor_);
    recordingRow->addStretch(1);
    form->addRow(tr("Frame rate:"), recordingRow);
    // #125: short hints for start, stop, pause and errors, placed where the recording does not see them.
    recordingHints_ = new QCheckBox(tr("Show a short hint when a recording starts, pauses or stops"), page);
    form->addRow(tr("Hints:"), recordingHints_);
    connect(recordingHints_, &QCheckBox::toggled, this, [this](bool on) {
        if (!updating_) {
            app_->store().recordingHints = on;
            app_->storeChanged();
        }
    });
    recordToggleShortcut_ = new KKeySequenceWidget(page);
    recordToggleShortcut_->setAccessibleName(tr("Start or stop recording shortcut"));
    recordPauseShortcut_ = new KKeySequenceWidget(page);
    recordPauseShortcut_->setAccessibleName(tr("Pause or resume recording shortcut"));
    for (KKeySequenceWidget *editor : {recordToggleShortcut_, recordPauseShortcut_}) {
        editor->setCheckForConflictsAgainst(KKeySequenceWidget::GlobalShortcuts | KKeySequenceWidget::StandardShortcuts);
        editor->setMultiKeyShortcutsAllowed(false);
    }
    auto *shortcutRow = new QHBoxLayout;
    shortcutRow->addWidget(new QLabel(tr("Start/stop:"), page));
    shortcutRow->addWidget(recordToggleShortcut_);
    if (ScreenRecorder::canPause()) {
        shortcutRow->addSpacing(12);
        shortcutRow->addWidget(new QLabel(tr("Pause:"), page));
        shortcutRow->addWidget(recordPauseShortcut_);
    } else {
        recordPauseShortcut_->hide(); // this KPipeWire cannot pause
    }
    shortcutRow->addStretch(1);
    form->addRow(tr("Shortcuts:"), shortcutRow);
    // #124: what the start/stop shortcut records, pinned in the overlay.
    recordingDefault_ = new QLabel(page);
    recordingDefault_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    recordingDefaultRemove_ = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete-remove")), tr("Remove"), page);
    recordingDefaultRemove_->setAccessibleName(tr("Remove the default recording target"));
    auto *defaultRow = new QHBoxLayout;
    defaultRow->addWidget(recordingDefault_);
    defaultRow->addSpacing(12);
    defaultRow->addWidget(recordingDefaultRemove_);
    defaultRow->addStretch(1);
    form->addRow(tr("Default target:"), defaultRow);
    connect(recordingDefaultRemove_, &QPushButton::clicked, this, [this] { app_->setDefaultRecordingTarget(QString()); });
    connect(app_, &App::defaultRecordingTargetChanged, this, &SettingsWindow::refreshRecordingDefault);
    form->addRow(QString(), makeHint(tr("Record from the overlay (Ctrl+R) or with the start/stop shortcut, which "
                                        "records the default target: pin it with the pin next to an entry in the "
                                        "overlay's record menus (or P). Without one, the shortcut opens the overlay "
                                        "to choose. Hints show beside what is recorded; when everything is recorded, the recording "
                                        "starts once the start hint has gone. GIF recordings use at most %1 fps. "
                                        "Recordings have no sound.")
                                         .arg(maxGifFrameRate),
                                     page));
    connect(recordingFormat_, &QComboBox::currentIndexChanged, this, [this] {
        if (updating_) {
            return;
        }
        app_->store().recordingFormat = RecordingFormat(recordingFormat_->currentData().toInt());
        app_->storeChanged();
    });
    connect(recordingFrameRate_, &QComboBox::currentIndexChanged, this, [this] {
        if (updating_) {
            return;
        }
        app_->store().recordingFrameRate = recordingFrameRate_->currentData().toInt();
        app_->storeChanged();
    });
    connect(recordingCursor_, &QCheckBox::toggled, this, [this](bool on) {
        if (updating_) {
            return;
        }
        app_->store().recordingCursor = on;
        app_->storeChanged();
    });
    connect(recordToggleShortcut_, &KKeySequenceWidget::keySequenceChanged, this, [this](const QKeySequence &seq) {
        if (!updating_) {
            assignShortcut(recordToggleActionId(), seq, recordToggleShortcut_);
        }
    });
    connect(recordPauseShortcut_, &KKeySequenceWidget::keySequenceChanged, this, [this](const QKeySequence &seq) {
        if (!updating_) {
            assignShortcut(recordPauseActionId(), seq, recordPauseShortcut_);
        }
    });

    const auto applyDirectory = [this] {
        if (updating_) {
            return;
        }
        app_->store().captureDirectory = QDir::cleanPath(directory_->text().trimmed());
        if (directory_->text().trimmed().isEmpty()) {
            app_->store().captureDirectory.clear();
        }
        app_->storeChanged();
    };
    connect(directory_, &QLineEdit::editingFinished, this, applyDirectory);
    connect(browse, &QPushButton::clicked, this, [this, applyDirectory] {
        const QString chosen = QFileDialog::getExistingDirectory(this, tr("Capture Folder"),
                                                                 app_->store().effectiveCaptureDirectory());
        if (!chosen.isEmpty()) {
            directory_->setText(chosen);
            applyDirectory();
        }
    });
    connect(template_, &QLineEdit::textChanged, this, [this] {
        if (updating_) {
            return;
        }
        const QString text = template_->text().trimmed();
        templatePreview_->setText(filenameFor(text.isEmpty() ? TargetStore::defaultFilenameTemplate() : text,
                                              QStringLiteral("DP-1-1"), QDateTime::currentDateTime(),
                                              app_->store().exportOptions.format));
    });
    connect(template_, &QLineEdit::editingFinished, this, [this] {
        if (!updating_) {
            app_->store().filenameTemplate = template_->text().trimmed();
            app_->storeChanged();
        }
    });
    storageExport_->setOnChanged([this] {
        if (updating_) {
            return;
        }
        app_->store().exportOptions = storageExport_->options();
        app_->storeChanged();
    });
    return page;
}

QWidget *SettingsWindow::createGeneralPage()
{
    auto *scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *page = new QWidget(scroll);
    scroll->setWidget(page);
    auto *outer = new QHBoxLayout(page);
    outer->setContentsMargins(4, 0, 4, 0);
    auto *content = new QWidget(page);
    content->setMaximumWidth(680);
    outer->addWidget(content);
    outer->addStretch(1);
    auto *form = new QFormLayout(content);
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    // UI language (#53). Native names in their own script, so everyone finds their language
    // whatever the current one is; sorted by English name, so the order never moves.
    language_ = new QComboBox(page);
    language_->addItem(tr("System default (%1)")
                           .arg(uiLanguageNativeName(resolveUiLanguage(QString(), systemUiLanguages()))),
                       QString());
    for (const UiLanguageChoice &choice : uiLanguageChoices()) {
        language_->addItem(choice.nativeName, choice.code);
    }
    form->addRow(tr("Language:"), language_);
    languageRestart_ = new KMessageWidget(tr("Itsova needs to restart to switch the language."), page);
    languageRestart_->setMessageType(KMessageWidget::Information);
    languageRestart_->setWordWrap(true);
    languageRestart_->setCloseButtonVisible(false);
    auto *restartNow = new QAction(QIcon::fromTheme(QStringLiteral("view-refresh")), tr("Restart now"), languageRestart_);
    languageRestart_->addAction(restartNow);
    languageRestart_->hide();
    form->addRow(QString(), languageRestart_);
    form->addRow(QString(), makeHint(tr("The Plasma widget follows the Plasma language."), page));
    connect(language_, &QComboBox::currentIndexChanged, this, [this] {
        const QString code = language_->currentData().toString();
        const bool differs = resolveUiLanguage(code, systemUiLanguages()) != activeUiLanguage();
        if (differs) {
            languageRestart_->animatedShow();
        } else if (languageRestart_->isVisible()) {
            languageRestart_->animatedHide();
        }
        if (!updating_) {
            app_->store().uiLanguage = code;
            app_->storeChanged();
        }
    });
    connect(restartNow, &QAction::triggered, this, [this] {
        if (!app_->restart()) {
            languageRestart_->setMessageType(KMessageWidget::Warning);
            languageRestart_->setText(tr("Itsova could not restart itself. Quit and start it again to switch the "
                                         "language."));
        }
    });

    autostart_ = new QCheckBox(tr("Start Itsova when I log in"), page);
    form->addRow(tr("Startup:"), autostart_);
    form->addRow(QString(), makeHint(tr("Shortcuts only work while Itsova is running."), page));
    connect(autostart_, &QCheckBox::toggled, this, [this](bool on) {
        if (!updating_) {
            app_->store().autostart = on;
            app_->storeChanged();
        }
    });

    // Tray icon (#123): hidden, it still comes back while something runs, so it can be stopped.
    trayVisibility_ = new QComboBox(page);
    trayVisibility_->addItem(tr("Always"), int(TargetStore::TrayVisibility::Always));
    trayVisibility_->addItem(tr("Only while something runs"), int(TargetStore::TrayVisibility::WhileRunning));
    trayVisibility_->addItem(tr("Only while recording"), int(TargetStore::TrayVisibility::WhileRecording));
    form->addRow(tr("Tray icon:"), trayVisibility_);
    trayHint_ = makeHint(tr("Shows while a recording runs (or interval captures, with Only while something runs), "
                            "so you can stop it. To open these settings again, start Itsova from the "
                            "application launcher or run itsova."),
                         page);
    form->addRow(QString(), trayHint_);
    connect(trayVisibility_, &QComboBox::currentIndexChanged, this, [this] {
        const auto mode = TargetStore::TrayVisibility(trayVisibility_->currentData().toInt());
        trayHint_->setVisible(mode != TargetStore::TrayVisibility::Always);
        if (!updating_) {
            app_->store().trayVisibility = mode;
            app_->storeChanged();
        }
    });

    usePrintKey_ = new QCheckBox(tr("Use the Print key for Itsova"), page);
    form->addRow(tr("Print key:"), usePrintKey_);
    form->addRow(QString(),
                 makeHint(tr("Turning this off gives the Print key back to the application that had it before "
                             "(usually Spectacle). Before uninstalling Itsova, turn this off or run "
                             "\"itsova --release-print-key\"."),
                          page));
    connect(usePrintKey_, &QCheckBox::toggled, this, [this](bool on) {
        if (!updating_) {
            app_->setUsePrintKey(on);
        }
    });

    printOpensOverlay_ = new QCheckBox(tr("Open the selection overlay"), page);
    form->addRow(tr("After Print:"), printOpensOverlay_);
    form->addRow(QString(), makeHint(tr("In the overlay you can cut the screenshot down to rectangles, a monitor or a "
                                        "window. Without it, Print saves the full screenshot directly."),
                                     page));
    connect(printOpensOverlay_, &QCheckBox::toggled, this, [this](bool on) {
        if (!updating_) {
            app_->store().printOpensOverlay = on;
            app_->storeChanged();
        }
        if (saveImmediatelyOnPrint_) {
            saveImmediatelyOnPrint_->setEnabled(on);
        }
    });

    saveImmediatelyOnPrint_ = new QCheckBox(tr("Save full screenshot immediately on Print"), page);
    form->addRow(QString(), saveImmediatelyOnPrint_);
    form->addRow(QString(),
                 makeHint(tr("Off by default: Print keeps the capture in memory until you save, copy, pin, or edit "
                             "it. Turn this on to write the full screenshot to disk and copy it to the clipboard "
                             "immediately when Print is pressed."),
                          page));
    connect(saveImmediatelyOnPrint_, &QCheckBox::toggled, this, [this](bool on) {
        if (!updating_) {
            app_->store().saveImmediatelyOnPrint = on;
            app_->storeChanged();
        }
    });

    overlayStart_ = new QComboBox(page);
    overlayStart_->addItem(tr("Last used"), int(TargetStore::OverlayStart::LastUsed));
    overlayStart_->addItem(tr("Frozen"), int(TargetStore::OverlayStart::Frozen));
    overlayStart_->addItem(tr("Live"), int(TargetStore::OverlayStart::Live));
    form->addRow(tr("Overlay starts:"), overlayStart_);
    form->addRow(QString(), makeHint(tr("Frozen shows the moment you pressed Print. Live lets videos keep playing and "
                                        "captures when you save. Space switches in the overlay at any time."),
                                     page));
    connect(overlayStart_, &QComboBox::currentIndexChanged, this, [this] {
        if (!updating_) {
            app_->store().overlayStart = TargetStore::OverlayStart(overlayStart_->currentData().toInt());
            app_->storeChanged();
        }
    });

    // Overlay panel (#72, Q33): a dragged panel remembers its place per monitor; the first-start
    // hint shows once.
    auto *panelRow = new QWidget(page);
    auto *panelLayout = new QHBoxLayout(panelRow);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    resetPanelPosition_ = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-reset")), tr("Reset Panel Position"),
                                          panelRow);
    showFirstHintAgain_ = new QPushButton(QIcon::fromTheme(QStringLiteral("help-hint")),
                                          tr("Show First-Start Hint Again"), panelRow);
    panelLayout->addWidget(resetPanelPosition_);
    panelLayout->addWidget(showFirstHintAgain_);
    panelLayout->addStretch(1);
    form->addRow(tr("Overlay panel:"), panelRow);
    form->addRow(QString(), makeHint(tr("Reset puts the panel back at the top centre of every monitor."), page));
    connect(resetPanelPosition_, &QPushButton::clicked, this, [this] {
        app_->store().overlayPanelPositions.clear();
        app_->storeChanged();
        resetPanelPosition_->setEnabled(false);
    });
    connect(showFirstHintAgain_, &QPushButton::clicked, this, [this] {
        app_->store().overlayFirstHintShown = false;
        app_->storeChanged();
        showFirstHintAgain_->setEnabled(false);
    });

    captureOnRelease_ = new QCheckBox(tr("Save as soon as the mouse is released"), page);
    form->addRow(tr("Selection:"), captureOnRelease_);
    form->addRow(QString(), makeHint(tr("Skips the buttons: drawing one rectangle saves it right away."), page));
    connect(captureOnRelease_, &QCheckBox::toggled, this, [this](bool on) {
        if (!updating_) {
            app_->store().captureOnRelease = on;
            app_->storeChanged();
        }
    });

    showSelectionLoupe_ = new QCheckBox(tr("Show magnifier loupe while selecting"), page);
    form->addRow(tr("Loupe:"), showSelectionLoupe_);
    form->addRow(QString(), makeHint(tr("Shows a magnified 11 × 11 native pixel preview while dragging or resizing "
                                        "a selection. While live, it shows the picture taken when you pressed Print."),
                                     page));
    connect(showSelectionLoupe_, &QCheckBox::toggled, this, [this](bool on) {
        if (!updating_) {
            app_->store().showSelectionLoupe = on;
            app_->storeChanged();
        }
    });

    colorPickerShortcut_ = new KKeySequenceWidget(page);
    colorPickerShortcut_->setAccessibleName(tr("Pick color shortcut"));
    colorPickerShortcut_->setCheckForConflictsAgainst(KKeySequenceWidget::GlobalShortcuts
                                                      | KKeySequenceWidget::StandardShortcuts);
    colorPickerShortcut_->setMultiKeyShortcutsAllowed(false);
    form->addRow(tr("Pick color shortcut:"), colorPickerShortcut_);
    form->addRow(QString(), makeHint(tr("Global shortcut to open the pixel color picker anywhere."), page));
    connect(colorPickerShortcut_, &KKeySequenceWidget::keySequenceChanged, this, [this](const QKeySequence &seq) {
        if (!updating_) {
            assignShortcut(colorPickerTargetId(), seq, colorPickerShortcut_);
        }
    });

    // Logging (#92): starts, stops and errors always reach the journal; this adds the details.
    verboseLogging_ = new QCheckBox(tr("Write detailed logs"), page);
    form->addRow(tr("Logging:"), verboseLogging_);
    form->addRow(QString(), makeHint(tr("Itsova always writes starts, stops and errors to the system journal. Detailed "
                                        "logs help with bug reports. Show them with itsova logs."),
                                     page));
    connect(verboseLogging_, &QCheckBox::toggled, this, [this](bool on) {
        if (!updating_) {
            app_->store().verboseLogging = on;
            app_->storeChanged();
        }
    });

    // Text recognition (#27): optional, detected at runtime.
    ocrStatus_ = new QLabel(page);
    ocrStatus_->setWordWrap(true);
    ocrStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Text recognition:"), ocrStatus_);
    ocrLanguages_ = new QListWidget(page);
    ocrLanguages_->setAccessibleName(tr("Text recognition languages"));
    ocrLanguages_->setSelectionMode(QAbstractItemView::NoSelection);
    ocrLanguages_->setMaximumHeight(ocrLanguages_->fontMetrics().height() * 7);
    form->addRow(tr("Languages:"), ocrLanguages_);
    ocrHint_ = makeHint(QString(), page);
    form->addRow(QString(), ocrHint_);
    connect(ocrLanguages_, &QListWidget::itemChanged, this, [this] {
        if (updating_) {
            return;
        }
        QStringList chosen;
        for (int i = 0; i < ocrLanguages_->count(); ++i) {
            const QListWidgetItem *item = ocrLanguages_->item(i);
            if (item->checkState() == Qt::Checked) {
                chosen.append(item->data(Qt::UserRole).toString());
            }
        }
        app_->store().ocrLanguages = chosen;
        app_->storeChanged();
    });
    return scroll;
}

void SettingsWindow::refreshTextRecognition()
{
    const QScopedValueRollback<bool> guard(updating_, true);
    TesseractOcrEngine &engine = app_->ocrEngine();
    engine.refresh();
    ocrLanguages_->clear();
    if (!engine.available()) {
        ocrStatus_->setText(tr("Not installed. Install the tesseract package and a language package, for example "
                               "tesseract-data-eng, to copy text from captures."));
        ocrLanguages_->setEnabled(false);
        ocrHint_->setText(tr("Itsova works the same without it; Copy Text then explains what is missing."));
        return;
    }
    ocrStatus_->setText(tr("%1 found").arg(engine.description()));
    const QStringList installed = engine.languages();
    const QStringList active = effectiveOcrLanguages(app_->store().ocrLanguages, installed);
    QList<QPair<QString, QString>> rows;
    for (const QString &code : installed) {
        if (code == QLatin1String("osd") || code == QLatin1String("equ")) {
            continue; // orientation/equation data, not reading languages
        }
        // tesseract uses ISO 639-2 codes such as "deu"; show a readable name where Qt knows one.
        const QLocale locale(QLocale::codeToLanguage(code, QLocale::ISO639Part2));
        const QString name = locale.language() == QLocale::C ? code
                                                             : tr("%1 (%2)").arg(QLocale::languageToString(locale.language()), code);
        rows.append({name, code});
    }
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.first.localeAwareCompare(b.first) < 0; });
    for (const auto &[name, code] : std::as_const(rows)) {
        auto *item = new QListWidgetItem(name, ocrLanguages_);
        item->setData(Qt::UserRole, code);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(active.contains(code) ? Qt::Checked : Qt::Unchecked);
    }
    ocrLanguages_->setEnabled(!rows.isEmpty());
    QString hint = tr("Copy Text uses the checked languages; with none checked it uses all of them.");
    if (!installed.contains(QStringLiteral("eng"))) {
        hint += QLatin1Char(' ') + tr("English is not installed (package tesseract-data-eng).");
    }
    ocrHint_->setText(hint);
}

QWidget *SettingsWindow::createReplayPage()
{
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    replayLoop_ = new QCheckBox(tr("Loop video playback"), page);
    form->addRow(tr("Playback:"), replayLoop_);
    connect(replayLoop_, &QCheckBox::toggled, this, [this](bool on) {
        if (!updating_) {
            app_->store().replayLoop = on;
            app_->storeChanged();
        }
    });

    replayShowCursor_ = new QCheckBox(tr("Show mouse pointer during replay"), page);
    form->addRow(tr("Mouse pointer:"), replayShowCursor_);
    form->addRow(QString(), makeHint(tr("During playback: M shows/hides the pointer, Esc ends."), page));
    connect(replayShowCursor_, &QCheckBox::toggled, this, [this](bool on) {
        if (!updating_) {
            app_->store().replayShowCursor = on;
            app_->storeChanged();
        }
    });

    return page;
}

void SettingsWindow::refreshReplay()
{
    if (!replayLoop_ || !replayShowCursor_) {
        return;
    }
    updating_ = true;
    const TargetStore &store = app_->store();
    replayLoop_->setChecked(store.replayLoop);
    replayShowCursor_->setChecked(store.replayShowCursor);
    updating_ = false;
}

QWidget *SettingsWindow::createAutomationPage()
{
    auto *scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *page = new QWidget(scroll);
    scroll->setWidget(page);
    auto *outer = new QHBoxLayout(page);
    outer->setContentsMargins(4, 0, 4, 0);
    auto *content = new QWidget(page);
    content->setMaximumWidth(680);
    outer->addWidget(content);
    outer->addStretch(1);
    auto *rootLayout = new QVBoxLayout(content);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(10);

    auto *hookHeading = new QLabel(tr("Post-Capture Command Hook"), content);
    QFont sectionFont = hookHeading->font();
    sectionFont.setBold(true);
    sectionFont.setPointSizeF(sectionFont.pointSizeF() * 1.08);
    hookHeading->setFont(sectionFont);
    rootLayout->addWidget(hookHeading);

    hookEnabled_ = new QCheckBox(tr("Run a command after saving captures"), content);
    rootLayout->addWidget(hookEnabled_);

    hookDetails_ = new QWidget(content);
    auto *hookForm = new QFormLayout(hookDetails_);
    hookForm->setContentsMargins(20, 4, 0, 0);
    hookForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    hookCommand_ = new QLineEdit(hookDetails_);
    hookCommand_->setPlaceholderText(QStringLiteral("/usr/local/bin/post-capture.sh %f"));
    hookCommand_->setAccessibleName(tr("Post-capture command"));
    hookForm->addRow(tr("Command:"), hookCommand_);
    hookForm->addRow(QString(), makeHint(
        tr("Executed directly without a shell. Pass %f for the file path(s), or read ITSOVA_FILE, ITSOVA_FILES, "
           "ITSOVA_TARGET_NAME, ITSOVA_FORMAT, and ITSOVA_TIMESTAMP. Use a script file for pipes or shell "
           "features."),
        hookDetails_));

    hookTimeout_ = new QSpinBox(hookDetails_);
    hookTimeout_->setRange(5, 300);
    hookTimeout_->setSuffix(tr(" s"));
    hookTimeout_->setKeyboardTracking(false);
    hookTimeout_->setAccessibleName(tr("Hook timeout in seconds"));
    hookForm->addRow(tr("Timeout:"), hookTimeout_);

    hookOnInterval_ = new QCheckBox(tr("Run hooks on interval captures"), hookDetails_);
    hookForm->addRow(tr("Intervals:"), hookOnInterval_);

    auto *testRow = new QHBoxLayout;
    testRow->setContentsMargins(0, 0, 0, 0);
    testHookButton_ = new QPushButton(QIcon::fromTheme(QStringLiteral("system-run")), tr("Test Hook"), hookDetails_);
    testRow->addWidget(testHookButton_);
    testRow->addStretch(1);
    hookForm->addRow(QString(), testRow);

    hookTestStatus_ = new QLabel(hookDetails_);
    hookTestStatus_->setWordWrap(true);
    hookTestStatus_->hide();
    hookForm->addRow(QString(), hookTestStatus_);

    rootLayout->addWidget(hookDetails_);

    auto *divider = new QFrame(content);
    divider->setFrameShape(QFrame::HLine);
    divider->setFrameShadow(QFrame::Sunken);
    rootLayout->addSpacing(6);
    rootLayout->addWidget(divider);
    rootLayout->addSpacing(4);

    auto *mcpHeading = new QLabel(tr("Local MCP Server"), content);
    mcpHeading->setFont(sectionFont);
    rootLayout->addWidget(mcpHeading);

    mcpEnabled_ = new QCheckBox(tr("Enable local MCP server (stdio only)"), content);
    rootLayout->addWidget(mcpEnabled_);

    mcpDetails_ = new QWidget(content);
    auto *mcpForm = new QFormLayout(mcpDetails_);
    mcpForm->setContentsMargins(20, 4, 0, 0);
    mcpForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    mcpAccessMode_ = new QComboBox(mcpDetails_);
    mcpAccessMode_->setAccessibleName(tr("MCP access scope"));
    mcpAccessMode_->addItem(tr("Allowlist only (recommended)"), int(McpAccessMode::Allowlist));
    mcpAccessMode_->addItem(tr("Confirm every capture"), int(McpAccessMode::ConfirmPerCapture));
    mcpAccessMode_->addItem(tr("Full session access"), int(McpAccessMode::FullSession));
    mcpForm->addRow(tr("Access scope:"), mcpAccessMode_);

    mcpAllowlistBox_ = new QWidget(mcpDetails_);
    auto *allowLayout = new QVBoxLayout(mcpAllowlistBox_);
    allowLayout->setContentsMargins(0, 2, 0, 2);
    allowLayout->setSpacing(4);
    allowLayout->addWidget(
        makeHint(tr("Only checked areas and groups can be listed or captured over MCP:"), mcpAllowlistBox_));
    mcpAllowlistList_ = new QListWidget(mcpAllowlistBox_);
    mcpAllowlistList_->setMaximumHeight(160);
    mcpAllowlistList_->setSelectionMode(QAbstractItemView::NoSelection);
    mcpAllowlistList_->setAccessibleName(tr("MCP allowed targets"));
    allowLayout->addWidget(mcpAllowlistList_);
    mcpAllowlistEmpty_ =
        makeHint(tr("No areas or groups yet — add one to allow MCP captures."), mcpAllowlistBox_);
    mcpAllowlistEmpty_->hide();
    allowLayout->addWidget(mcpAllowlistEmpty_);
    mcpForm->addRow(tr("Allowed targets:"), mcpAllowlistBox_);

    mcpFullWarning_ = new QLabel(
        tr("Warning: Connected local MCP clients can capture any screen, region, or window without confirmation."),
        mcpDetails_);
    mcpFullWarning_->setWordWrap(true);
    {
        QPalette warnPal = mcpFullWarning_->palette();
        warnPal.setColor(QPalette::WindowText, QColor(0xda, 0x44, 0x53));
        mcpFullWarning_->setPalette(warnPal);
    }
    mcpFullWarning_->hide();
    mcpForm->addRow(QString(), mcpFullWarning_);

    mcpNotify_ = new QCheckBox(tr("Show a passive notification on MCP captures"), mcpDetails_);
    mcpForm->addRow(tr("Notifications:"), mcpNotify_);

    auto *cmdRow = new QHBoxLayout;
    cmdRow->setContentsMargins(0, 0, 0, 0);
    cmdRow->setSpacing(6);
    mcpCommandDisplay_ = new QLineEdit(mcpDetails_);
    mcpCommandDisplay_->setReadOnly(true);
    mcpCommandDisplay_->setText(QStringLiteral("itsova mcp"));
    mcpCommandDisplay_->setAccessibleName(tr("MCP stdio launch command"));
    mcpCopyButton_ = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-copy")), tr("Copy"), mcpDetails_);
    cmdRow->addWidget(mcpCommandDisplay_, 1);
    cmdRow->addWidget(mcpCopyButton_);
    mcpForm->addRow(tr("Command:"), cmdRow);
    mcpForm->addRow(QString(),
                    makeHint(tr("Add this command in a local MCP client (Claude Code, Codex, Cursor). "
                                "Runs strictly on stdin/stdout with no network port."),
                             mcpDetails_));

    rootLayout->addWidget(mcpDetails_);
    rootLayout->addStretch(1);

    connect(hookEnabled_, &QCheckBox::toggled, this, [this](bool on) {
        hookDetails_->setVisible(on);
        if (!updating_) {
            app_->store().hook.enabled = on;
            app_->storeChanged();
        }
    });
    connect(hookCommand_, &QLineEdit::textChanged, this, [this](const QString &text) {
        testHookButton_->setEnabled(!text.trimmed().isEmpty());
    });
    connect(hookCommand_, &QLineEdit::editingFinished, this, [this] {
        if (!updating_) {
            app_->store().hook.command = hookCommand_->text().trimmed();
            app_->storeChanged();
        }
    });
    connect(hookTimeout_, &QSpinBox::valueChanged, this, [this](int secs) {
        if (!updating_) {
            app_->store().hook.timeoutSeconds = secs;
            app_->storeChanged();
        }
    });
    connect(hookOnInterval_, &QCheckBox::toggled, this, [this](bool on) {
        if (!updating_) {
            app_->store().hook.runOnInterval = on;
            app_->storeChanged();
        }
    });
    connect(testHookButton_, &QPushButton::clicked, this, [this] {
        const QString cmd = hookCommand_->text().trimmed();
        if (cmd.isEmpty()) {
            return;
        }
        testHookButton_->setEnabled(false);
        hookTestStatus_->setPalette(palette());
        hookTestStatus_->setText(tr("Running test hook…"));
        hookTestStatus_->show();
        QPointer<SettingsWindow> safeThis(this);
        app_->testPostCaptureHook(cmd, hookTimeout_->value(), [safeThis](const HookResult &res) {
            if (!safeThis) {
                return;
            }
            safeThis->testHookButton_->setEnabled(!safeThis->hookCommand_->text().trimmed().isEmpty());
            const bool ok = res.started && !res.timedOut && res.exitCode == 0;
            QPalette pal = safeThis->palette();
            if (!ok) {
                pal.setColor(QPalette::WindowText, QColor(0xda, 0x44, 0x53));
            }
            safeThis->hookTestStatus_->setPalette(pal);
            QString summary = ok ? tr("Hook succeeded (exit 0, %1 ms).").arg(res.elapsedMs)
                                 : (res.timedOut ? tr("Hook timed out after %1 ms.").arg(res.elapsedMs)
                                                 : tr("Hook failed: %1").arg(res.errorString));
            const QString outSample = !res.stdErr.trimmed().isEmpty() ? res.stdErr.trimmed() : res.stdOut.trimmed();
            if (!outSample.isEmpty()) {
                summary += QStringLiteral("\n") + outSample.left(240);
            }
            safeThis->hookTestStatus_->setText(summary);
            safeThis->hookTestStatus_->show();
        });
    });

    connect(mcpEnabled_, &QCheckBox::toggled, this, [this](bool on) {
        mcpDetails_->setVisible(on);
        if (!updating_) {
            app_->store().mcp.enabled = on;
            app_->storeChanged();
        }
    });
    connect(mcpAccessMode_, &QComboBox::currentIndexChanged, this, [this] {
        const auto mode = McpAccessMode(mcpAccessMode_->currentData().toInt());
        mcpAllowlistBox_->setVisible(mode == McpAccessMode::Allowlist);
        mcpFullWarning_->setVisible(mode == McpAccessMode::FullSession);
        if (!updating_) {
            app_->store().mcp.accessMode = mode;
            app_->storeChanged();
        }
    });
    connect(mcpAllowlistList_, &QListWidget::itemChanged, this, [this] {
        if (updating_) {
            return;
        }
        QStringList allowed;
        for (int i = 0; i < mcpAllowlistList_->count(); ++i) {
            const QListWidgetItem *item = mcpAllowlistList_->item(i);
            if (item && item->checkState() == Qt::Checked) {
                allowed.append(item->data(idRole).toString());
            }
        }
        app_->store().mcp.allowedTargetIds = allowed;
        app_->storeChanged();
    });
    connect(mcpNotify_, &QCheckBox::toggled, this, [this](bool on) {
        if (!updating_) {
            app_->store().mcp.notifyOnCapture = on;
            app_->storeChanged();
        }
    });
    connect(mcpCopyButton_, &QPushButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(QStringLiteral("itsova mcp"));
        mcpCopyButton_->setText(tr("Copied"));
        QPointer<QPushButton> safeBtn(mcpCopyButton_);
        QTimer::singleShot(1500, this, [safeBtn] {
            if (safeBtn) {
                safeBtn->setText(tr("Copy"));
            }
        });
    });

    return scroll;
}

void SettingsWindow::refreshStorage()
{
    updating_ = true;
    const TargetStore &store = app_->store();
    directory_->setText(store.captureDirectory);
    template_->setText(store.filenameTemplate);
    storageExport_->setOptions(store.exportOptions);
    historyLimit_->setValue(store.historyLimit);
    recordingFormat_->setCurrentIndex(recordingFormat_->findData(int(store.recordingFormat)));
    recordingFrameRate_->setCurrentIndex(
        std::max(0, recordingFrameRate_->findData(effectiveFrameRate(RecordingFormat::Mp4, store.recordingFrameRate))));
    recordingCursor_->setChecked(store.recordingCursor);
    recordingHints_->setChecked(store.recordingHints);
    refreshRecordingDefault();
    recordToggleShortcut_->setKeySequence(app_->shortcuts()->shortcut(recordToggleActionId()));
    recordPauseShortcut_->setKeySequence(app_->shortcuts()->shortcut(recordPauseActionId()));
    templatePreview_->setText(
        filenameFor(store.effectiveFilenameTemplate(), QStringLiteral("DP-1-1"), QDateTime::currentDateTime(),
                    store.exportOptions.format));
    {
        const int index = language_->findData(matchUiLanguage(store.uiLanguage).isEmpty()
                                                  ? QString()
                                                  : matchUiLanguage(store.uiLanguage));
        language_->setCurrentIndex(index < 0 ? 0 : index);
    }
    autostart_->setChecked(store.autostart);
    trayVisibility_->setCurrentIndex(trayVisibility_->findData(int(store.trayVisibility)));
    trayHint_->setVisible(store.trayVisibility != TargetStore::TrayVisibility::Always);
    usePrintKey_->setChecked(store.usePrintKey);
    printOpensOverlay_->setChecked(store.printOpensOverlay);
    saveImmediatelyOnPrint_->setChecked(store.saveImmediatelyOnPrint);
    saveImmediatelyOnPrint_->setEnabled(store.printOpensOverlay);
    overlayStart_->setCurrentIndex(overlayStart_->findData(int(store.overlayStart)));
    resetPanelPosition_->setEnabled(!store.overlayPanelPositions.isEmpty());
    showFirstHintAgain_->setEnabled(store.overlayFirstHintShown);
    captureOnRelease_->setChecked(store.captureOnRelease);
    showSelectionLoupe_->setChecked(store.showSelectionLoupe);
    verboseLogging_->setChecked(store.verboseLogging);
    colorPickerShortcut_->setKeySequence(app_->shortcuts()->shortcut(colorPickerTargetId()));
    updating_ = false;
}

void SettingsWindow::refreshAutomation()
{
    updating_ = true;
    const TargetStore &store = app_->store();
    hookEnabled_->setChecked(store.hook.enabled);
    hookDetails_->setVisible(store.hook.enabled);
    if (!hookCommand_->hasFocus()) {
        hookCommand_->setText(store.hook.command);
    }
    hookTimeout_->setValue(std::clamp(store.hook.timeoutSeconds, 5, 300));
    hookOnInterval_->setChecked(store.hook.runOnInterval);
    testHookButton_->setEnabled(!hookCommand_->text().trimmed().isEmpty());

    mcpEnabled_->setChecked(store.mcp.enabled);
    mcpDetails_->setVisible(store.mcp.enabled);
    mcpAccessMode_->setCurrentIndex(std::max(0, mcpAccessMode_->findData(int(store.mcp.accessMode))));
    mcpNotify_->setChecked(store.mcp.notifyOnCapture);
    mcpAllowlistBox_->setVisible(store.mcp.accessMode == McpAccessMode::Allowlist);
    mcpFullWarning_->setVisible(store.mcp.accessMode == McpAccessMode::FullSession);

    mcpAllowlistList_->clear();
    for (const Area &area : store.areas) {
        auto *item = new QListWidgetItem(QIcon::fromTheme(QStringLiteral("transform-crop")),
                                         tr("%1 (area)").arg(area.name), mcpAllowlistList_);
        item->setData(idRole, area.id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(store.mcp.allowedTargetIds.contains(area.id) ? Qt::Checked : Qt::Unchecked);
    }
    for (const Group &group : store.groups) {
        auto *item = new QListWidgetItem(QIcon::fromTheme(QStringLiteral("view-group")),
                                         tr("%1 (group)").arg(group.name), mcpAllowlistList_);
        item->setData(idRole, group.id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(store.mcp.allowedTargetIds.contains(group.id) ? Qt::Checked : Qt::Unchecked);
    }
    const bool hasAnyAllowlistItems = (mcpAllowlistList_->count() > 0);
    mcpAllowlistList_->setVisible(hasAnyAllowlistItems);
    mcpAllowlistEmpty_->setVisible(!hasAnyAllowlistItems);

    updating_ = false;
}

void SettingsWindow::refresh()
{
    refreshTargets();
    refreshGroups();
    refreshStorage();
    refreshReplay();
    refreshAutomation();
}

void SettingsWindow::refreshRecordingDefault()
{
    const QString id = app_->store().defaultRecordingTarget;
    recordingDefault_->setText(id.isEmpty() ? tr("None: the shortcut opens the overlay") : app_->recordingTargetName(id));
    recordingDefaultRemove_->setVisible(!id.isEmpty());
}

} // namespace itsova
