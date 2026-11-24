#pragma once

#include "imageoutput.h"

#include <QDialog>
#include <QImage>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;
class QWidget;

namespace glimply
{

// Compact KDE/Breeze Save As / Export dialog providing format selection, contextual quality or
// compression controls, native file picker Browse..., and debounced async size estimation.
class ExportDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ExportDialog(const QImage &previewImage,
                          const QString &suggestedPath,
                          const ExportOptions &initialOptions,
                          QWidget *parent = nullptr);

    [[nodiscard]] QString filePath() const;
    [[nodiscard]] ExportOptions exportOptions() const;
    [[nodiscard]] QString estimatedSizeText() const;
    [[nodiscard]] qint64 estimatedBytes() const;

    [[nodiscard]] QComboBox *formatCombo() const;
    [[nodiscard]] QLineEdit *pathEdit() const;
    [[nodiscard]] QSpinBox *valueSpin() const;
    [[nodiscard]] QCheckBox *losslessCheck() const;

private:
    void ensureFormatInCombo(ImageFormat format);
    void syncParameterControls();
    void scheduleEstimate(bool immediate = false);
    void runEstimate();
    void acceptIfValid();
    void browsePath();

    QImage previewImage_;
    ExportOptions options_;
    qint64 estimatedBytes_ = -1;
    quint64 estimateSeq_ = 0;
    bool updating_ = false;

    QLineEdit *pathEdit_ = nullptr;
    QPushButton *browseButton_ = nullptr;
    QComboBox *formatCombo_ = nullptr;
    QLabel *paramLabel_ = nullptr;
    QWidget *paramWidget_ = nullptr;
    QCheckBox *losslessCheck_ = nullptr;
    QSlider *valueSlider_ = nullptr;
    QSpinBox *valueSpin_ = nullptr;
    QLabel *sizeLabel_ = nullptr;
    QTimer *estimateTimer_ = nullptr;
};

} // namespace glimply
