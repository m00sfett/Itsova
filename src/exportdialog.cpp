#include "exportdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>

namespace glimply
{
namespace
{

constexpr int estimateDebounceMs = 120;

QString buildFileDialogFilter()
{
    QStringList allExtensions;
    QStringList sections;
    for (const ImageFormat fmt : availableCuratedFormats()) {
        if (fmt == ImageFormat::Jpeg) {
            allExtensions << QStringLiteral("*.jpg") << QStringLiteral("*.jpeg");
            sections << QStringLiteral("JPEG (*.jpg *.jpeg)");
        } else {
            const QString ext = canonicalExtension(fmt);
            allExtensions << QStringLiteral("*.%1").arg(ext);
            sections << QStringLiteral("%1 (*.%2)").arg(formatDisplayName(fmt), ext);
        }
    }
    sections.prepend(ExportDialog::tr("Images (%1)").arg(allExtensions.join(QLatin1Char(' '))));
    sections << ExportDialog::tr("All files (*)");
    return sections.join(QStringLiteral(";;"));
}

} // namespace

ExportDialog::ExportDialog(const QImage &previewImage,
                           const QString &suggestedPath,
                           const ExportOptions &initialOptions,
                           QWidget *parent)
    : QDialog(parent)
    , previewImage_(previewImage)
    , options_(initialOptions)
{
    if (!isImageFormatSupported(options_.format)) {
        options_.format = ImageFormat::Png;
    }

    setWindowTitle(tr("Save Screenshot As"));
    setMinimumWidth(480);

    auto *root = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    pathEdit_ = new QLineEdit(replaceImageExtension(suggestedPath, options_.format), this);
    pathEdit_->setAccessibleName(tr("Destination file path"));
    browseButton_ = new QPushButton(QIcon::fromTheme(QStringLiteral("document-open-folder")), tr("Browse…"), this);
    auto *pathRow = new QHBoxLayout;
    pathRow->addWidget(pathEdit_);
    pathRow->addWidget(browseButton_);
    form->addRow(tr("File:"), pathRow);

    formatCombo_ = new QComboBox(this);
    formatCombo_->setAccessibleName(tr("Image format"));
    for (const ImageFormat fmt : availableCuratedFormats()) {
        formatCombo_->addItem(formatDisplayName(fmt), int(fmt));
    }
    ensureFormatInCombo(options_.format);
    formatCombo_->setCurrentIndex(formatCombo_->findData(int(options_.format)));
    form->addRow(tr("Format:"), formatCombo_);

    paramLabel_ = new QLabel(tr("Quality:"), this);
    paramWidget_ = new QWidget(this);
    auto *paramRow = new QHBoxLayout(paramWidget_);
    paramRow->setContentsMargins(0, 0, 0, 0);
    losslessCheck_ = new QCheckBox(tr("Lossless"), paramWidget_);
    valueSlider_ = new QSlider(Qt::Horizontal, paramWidget_);
    valueSlider_->setAccessibleName(tr("Compression or quality level"));
    valueSpin_ = new QSpinBox(paramWidget_);
    valueSpin_->setAccessibleName(tr("Compression or quality value"));
    paramRow->addWidget(losslessCheck_);
    paramRow->addWidget(valueSlider_, 1);
    paramRow->addWidget(valueSpin_);
    form->addRow(paramLabel_, paramWidget_);

    sizeLabel_ = new QLabel(previewImage_.isNull() ? tr("Unavailable") : tr("Calculating…"), this);
    sizeLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Estimated size:"), sizeLabel_);

    root->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    if (QPushButton *saveBtn = buttons->button(QDialogButtonBox::Save)) {
        saveBtn->setDefault(true);
    }
    root->addWidget(buttons);

    estimateTimer_ = new QTimer(this);
    estimateTimer_->setSingleShot(true);
    estimateTimer_->setInterval(estimateDebounceMs);
    connect(estimateTimer_, &QTimer::timeout, this, &ExportDialog::runEstimate);

    connect(browseButton_, &QPushButton::clicked, this, &ExportDialog::browsePath);
    connect(pathEdit_, &QLineEdit::editingFinished, this, [this] {
        if (updating_) {
            return;
        }
        const std::optional<ImageFormat> fromPath = imageFormatFromPath(pathEdit_->text().trimmed());
        if (fromPath.has_value() && isImageFormatSupported(*fromPath) && *fromPath != options_.format) {
            options_.format = *fromPath;
            updating_ = true;
            ensureFormatInCombo(*fromPath);
            formatCombo_->setCurrentIndex(formatCombo_->findData(int(*fromPath)));
            updating_ = false;
            syncParameterControls();
            scheduleEstimate(false);
        }
    });
    connect(formatCombo_, &QComboBox::currentIndexChanged, this, [this] {
        if (updating_) {
            return;
        }
        const auto fmt = static_cast<ImageFormat>(formatCombo_->currentData().toInt());
        if (fmt == options_.format) {
            return;
        }
        options_.format = fmt;
        updating_ = true;
        pathEdit_->setText(replaceImageExtension(pathEdit_->text().trimmed(), fmt));
        updating_ = false;
        syncParameterControls();
        scheduleEstimate(false);
    });
    connect(losslessCheck_, &QCheckBox::toggled, this, [this](bool on) {
        if (updating_) {
            return;
        }
        options_.lossless = on;
        valueSlider_->setEnabled(!on);
        valueSpin_->setEnabled(!on);
        scheduleEstimate(false);
    });
    connect(valueSlider_, &QSlider::valueChanged, this, [this](int val) {
        if (updating_) {
            return;
        }
        {
            const QSignalBlocker block(valueSpin_);
            valueSpin_->setValue(val);
        }
        if (options_.format == ImageFormat::Png) {
            options_.pngCompression = val;
        } else {
            options_.quality = val;
        }
        scheduleEstimate(false);
    });
    connect(valueSpin_, &QSpinBox::valueChanged, this, [this](int val) {
        if (updating_) {
            return;
        }
        {
            const QSignalBlocker block(valueSlider_);
            valueSlider_->setValue(val);
        }
        if (options_.format == ImageFormat::Png) {
            options_.pngCompression = val;
        } else {
            options_.quality = val;
        }
        scheduleEstimate(false);
    });

    connect(buttons, &QDialogButtonBox::accepted, this, &ExportDialog::acceptIfValid);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    syncParameterControls();
    scheduleEstimate(true);
}

QString ExportDialog::filePath() const
{
    QString raw = pathEdit_->text().trimmed();
    if (raw.isEmpty()) {
        return {};
    }
    const std::optional<ImageFormat> typedFmt = imageFormatFromPath(raw);
    if (!typedFmt.has_value() || !isImageFormatSupported(*typedFmt)) {
        raw = replaceImageExtension(raw, options_.format);
    }
    return QFileInfo(raw).absoluteFilePath();
}

ExportOptions ExportDialog::exportOptions() const
{
    ExportOptions out = options_;
    if (const std::optional<ImageFormat> pathFmt = imageFormatFromPath(filePath());
        pathFmt.has_value() && isImageFormatSupported(*pathFmt)) {
        out.format = *pathFmt;
    }
    return out;
}

QString ExportDialog::estimatedSizeText() const
{
    return sizeLabel_->text();
}

qint64 ExportDialog::estimatedBytes() const
{
    return estimatedBytes_;
}

QComboBox *ExportDialog::formatCombo() const
{
    return formatCombo_;
}

QLineEdit *ExportDialog::pathEdit() const
{
    return pathEdit_;
}

QSpinBox *ExportDialog::valueSpin() const
{
    return valueSpin_;
}

QCheckBox *ExportDialog::losslessCheck() const
{
    return losslessCheck_;
}

void ExportDialog::ensureFormatInCombo(ImageFormat format)
{
    if (formatCombo_->findData(int(format)) >= 0) {
        return;
    }
    formatCombo_->addItem(formatDisplayName(format), int(format));
}

void ExportDialog::syncParameterControls()
{
    updating_ = true;
    if (options_.format == ImageFormat::Png) {
        paramLabel_->setText(tr("Compression:"));
        paramLabel_->show();
        paramWidget_->show();
        losslessCheck_->hide();
        valueSlider_->setRange(0, 9);
        valueSpin_->setRange(0, 9);
        valueSpin_->setSuffix(QString());
        const int comp = options_.pngCompression >= 0 ? std::clamp(options_.pngCompression, 0, 9) : 6;
        valueSlider_->setValue(comp);
        valueSpin_->setValue(comp);
        valueSlider_->setEnabled(true);
        valueSpin_->setEnabled(true);
    } else if (formatSupportsQuality(options_.format)) {
        paramLabel_->setText(tr("Quality:"));
        paramLabel_->show();
        paramWidget_->show();
        const bool hasLossless = formatSupportsLosslessToggle(options_.format);
        losslessCheck_->setVisible(hasLossless);
        losslessCheck_->setChecked(hasLossless && options_.lossless);
        const int maxQ = hasLossless ? 99 : 100;
        const int q = std::clamp(options_.quality, 1, maxQ);
        valueSlider_->setRange(1, maxQ);
        valueSpin_->setRange(1, maxQ);
        valueSpin_->setSuffix(tr(" %"));
        valueSlider_->setValue(q);
        valueSpin_->setValue(q);
        const bool enabled = !(hasLossless && options_.lossless);
        valueSlider_->setEnabled(enabled);
        valueSpin_->setEnabled(enabled);
    } else {
        paramLabel_->hide();
        paramWidget_->hide();
    }
    updating_ = false;
}

void ExportDialog::scheduleEstimate(bool immediate)
{
    if (previewImage_.isNull()) {
        estimatedBytes_ = -1;
        sizeLabel_->setText(tr("Unavailable"));
        return;
    }
    ++estimateSeq_;
    estimatedBytes_ = -1;
    sizeLabel_->setText(tr("Calculating…"));
    if (immediate) {
        estimateTimer_->stop();
        runEstimate();
    } else {
        estimateTimer_->start();
    }
}

void ExportDialog::runEstimate()
{
    if (previewImage_.isNull()) {
        return;
    }
    const quint64 seq = estimateSeq_;
    const QImage snapshot = previewImage_;
    const ExportOptions opts = exportOptions();

    auto *watcher = new QFutureWatcher<qint64>();
    QPointer<ExportDialog> self(this);
    connect(watcher, &QFutureWatcher<qint64>::finished, this, [self, watcher, seq] {
        const qint64 bytes = watcher->result();
        watcher->deleteLater();
        if (!self || seq != self->estimateSeq_) {
            return;
        }
        self->estimatedBytes_ = bytes;
        if (bytes > 0) {
            self->sizeLabel_->setText(tr("≈ %1").arg(QLocale().formattedDataSize(bytes)));
        } else {
            self->sizeLabel_->setText(tr("Unavailable"));
        }
    });
    // Ensure watcher still gets deleted if the dialog is closed before QtConcurrent finishes.
    connect(watcher, &QFutureWatcher<qint64>::finished, watcher, &QObject::deleteLater);
    watcher->setFuture(QtConcurrent::run([snapshot, opts] {
        return estimateEncodedSize(snapshot, opts);
    }));
}

void ExportDialog::browsePath()
{
    const QString initial = filePath().isEmpty() ? pathEdit_->text().trimmed() : filePath();
    const QString chosen = QFileDialog::getSaveFileName(this, tr("Choose Destination File"), initial,
                                                        buildFileDialogFilter());
    if (chosen.isEmpty()) {
        return;
    }
    updating_ = true;
    const std::optional<ImageFormat> fromChosen = imageFormatFromPath(chosen);
    if (fromChosen.has_value() && isImageFormatSupported(*fromChosen)) {
        options_.format = *fromChosen;
        ensureFormatInCombo(*fromChosen);
        formatCombo_->setCurrentIndex(formatCombo_->findData(int(*fromChosen)));
        pathEdit_->setText(chosen);
    } else {
        pathEdit_->setText(replaceImageExtension(chosen, options_.format));
    }
    updating_ = false;
    syncParameterControls();
    scheduleEstimate(false);
}

void ExportDialog::acceptIfValid()
{
    if (filePath().isEmpty()) {
        pathEdit_->setFocus();
        return;
    }
    accept();
}

} // namespace glimply
