#include "quickeditorwindow.h"

#include "annotations/annotationcontroller.h"
#include "annotations/annotationitem.h"
#include "imageoutput.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QImageReader>
#include <QImageWriter>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace glimply
{
namespace
{

constexpr qreal canvasScrollMargin = 24.0;

} // namespace

QuickEditorWindow::QuickEditorWindow(TargetStore &store, const QString &filePath, QWidget *parent)
    : QMainWindow(parent)
    , store_(store)
    , filePath_(QFileInfo(filePath).absoluteFilePath())
    , baseNameToken_(QFileInfo(filePath).completeBaseName())
    , ownedDoc_(std::make_unique<AnnotationDocument>())
{
    QImageReader reader(filePath_);
    reader.setAutoTransform(true);
    QImage loaded = reader.read();
    if (loaded.isNull()) {
        loadError_ = reader.errorString().isEmpty() ? tr("Could not open image file.") : reader.errorString();
        valid_ = false;
        buildUi();
        return;
    }
    loaded.setDevicePixelRatio(1.0);
    valid_ = true;
    isUnsavedCapture_ = false;

    buildUi();
    canvas_->setDocument(ownedDoc_.get());
    canvas_->setImage(loaded, 1.0);
    ownedDoc_->undoStack().setClean();
    wireSignals();
    resize(980, 680);
    updateWindowState();
    updateStatusBar();
    updateScrollBars();
}

QuickEditorWindow::QuickEditorWindow(TargetStore &store, const QImage &nativeImage, qreal scale,
                                     std::unique_ptr<AnnotationDocument> annotations, const QString &baseNameToken,
                                     QWidget *parent)
    : QMainWindow(parent)
    , store_(store)
    , baseNameToken_(baseNameToken.trimmed().isEmpty() ? QStringLiteral("capture") : baseNameToken.trimmed())
    , ownedDoc_(annotations ? std::move(annotations) : std::make_unique<AnnotationDocument>())
{
    if (nativeImage.isNull()) {
        loadError_ = tr("The captured image is empty.");
        valid_ = false;
        buildUi();
        return;
    }
    valid_ = true;
    isUnsavedCapture_ = true;

    buildUi();
    canvas_->setDocument(ownedDoc_.get());
    canvas_->setImage(nativeImage, scale > 0.0 ? scale : 1.0);
    ownedDoc_->undoStack().setClean();
    wireSignals();
    resize(980, 680);
    updateWindowState();
    updateStatusBar();
    updateScrollBars();
}

void QuickEditorWindow::buildUi()
{
    setWindowIcon(QIcon::fromTheme(QStringLiteral("io.github.m00sfett.glimply"),
                                   QIcon::fromTheme(QStringLiteral("document-edit"))));

    // Main file/clipboard toolbar.
    auto *fileBar = addToolBar(tr("Editor Actions"));
    fileBar->setMovable(false);
    fileBar->setFloatable(false);
    fileBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    saveAction_ = fileBar->addAction(QIcon::fromTheme(QStringLiteral("document-save")), tr("Save"));
    saveAction_->setShortcut(QKeySequence::Save);
    saveAction_->setToolTip(tr("Save changes cleanly to file without altering the clipboard (Ctrl+S)"));
    connect(saveAction_, &QAction::triggered, this, &QuickEditorWindow::save);

    saveCopyAction_ = fileBar->addAction(
        QIcon::fromTheme(QStringLiteral("document-save-all"), QIcon::fromTheme(QStringLiteral("document-save"))),
        tr("Save Copy"));
    saveCopyAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_S));
    saveCopyAction_->setToolTip(tr("Save a new edited copy alongside the original without overwriting (Ctrl+Alt+S)"));
    connect(saveCopyAction_, &QAction::triggered, this, &QuickEditorWindow::saveCopy);

    saveAsAction_ = fileBar->addAction(QIcon::fromTheme(QStringLiteral("document-save-as")), tr("Save As…"));
    saveAsAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    saveAsAction_->setToolTip(tr("Choose a new file path (Ctrl+Shift+S)"));
    connect(saveAsAction_, &QAction::triggered, this, &QuickEditorWindow::saveAs);

    fileBar->addSeparator();

    copyAction_ = fileBar->addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), tr("Copy"));
    // Bind Ctrl+Shift+C on the QAction so Ctrl+C continues working inside inline text fields;
    // keyPressEvent also triggers copyToClipboard on Ctrl+C when outside text editing.
    copyAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    copyAction_->setToolTip(tr("Copy flattened image to clipboard (Ctrl+C)"));
    connect(copyAction_, &QAction::triggered, this, &QuickEditorWindow::copyToClipboard);

    auto *closeAction = new QAction(this);
    closeAction->setShortcut(QKeySequence::Close);
    connect(closeAction, &QAction::triggered, this, &QWidget::close);
    addAction(closeAction);

    // Central area: markup toolbar + contextual crop strip + scrollable AnnotationCanvas.
    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    canvas_ = new AnnotationCanvas(central);
    canvas_->setEditorMode(true);

    auto *markupHeader = new QFrame(central);
    markupHeader->setFrameShape(QFrame::StyledPanel);
    markupHeader->setFrameShadow(QFrame::Plain);
    auto *headerLayout = new QVBoxLayout(markupHeader);
    headerLayout->setContentsMargins(6, 4, 6, 4);
    headerLayout->setSpacing(4);

    annotationToolbar_ = new AnnotationToolbar(canvas_->controller(), markupHeader);
    annotationToolbar_->setAvailableTools({
        ToolKind::Select,
        ToolKind::Crop,
        ToolKind::Arrow,
        ToolKind::Rectangle,
        ToolKind::Ellipse,
        ToolKind::Pen,
        ToolKind::Highlighter,
        ToolKind::Text,
        ToolKind::Step,
        ToolKind::Redact,
        ToolKind::Spotlight,
    });
    annotationToolbar_->setSettings(&store_.annotations);
    headerLayout->addWidget(annotationToolbar_);

    cropStrip_ = new QWidget(markupHeader);
    auto *cropLayout = new QHBoxLayout(cropStrip_);
    cropLayout->setContentsMargins(2, 2, 2, 0);
    cropLayout->setSpacing(6);
    cropInfoLabel_ = new QLabel(tr("Drag on canvas to select crop area"), cropStrip_);
    applyCropButton_ = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-ok-apply")), tr("Apply Crop"), cropStrip_);
    applyCropButton_->setToolTip(tr("Crop image cleanly to selection (Enter)"));
    applyCropButton_->setEnabled(false);
    applyCropButton_->setFocusPolicy(Qt::NoFocus);
    cancelCropButton_ = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-cancel")), tr("Cancel Crop"), cropStrip_);
    cancelCropButton_->setToolTip(tr("Exit crop mode (Escape)"));
    cancelCropButton_->setFocusPolicy(Qt::NoFocus);
    cropLayout->addWidget(cropInfoLabel_);
    cropLayout->addWidget(applyCropButton_);
    cropLayout->addWidget(cancelCropButton_);
    cropLayout->addStretch();
    cropStrip_->hide();
    headerLayout->addWidget(cropStrip_);

    root->addWidget(markupHeader);

    auto *canvasGrid = new QGridLayout;
    canvasGrid->setContentsMargins(0, 0, 0, 0);
    canvasGrid->setSpacing(0);
    hScroll_ = new QScrollBar(Qt::Horizontal, central);
    vScroll_ = new QScrollBar(Qt::Vertical, central);
    hScroll_->setFocusPolicy(Qt::NoFocus);
    vScroll_->setFocusPolicy(Qt::NoFocus);
    canvasGrid->addWidget(canvas_, 0, 0);
    canvasGrid->addWidget(vScroll_, 0, 1);
    canvasGrid->addWidget(hScroll_, 1, 0);
    canvasGrid->setRowStretch(0, 1);
    canvasGrid->setColumnStretch(0, 1);
    root->addLayout(canvasGrid, 1);

    setCentralWidget(central);

    // Status bar: pixel size, transient status message, zoom controls.
    dimensionsLabel_ = new QLabel(this);
    dimensionsLabel_->setContentsMargins(6, 0, 6, 0);
    statusBar()->addWidget(dimensionsLabel_);

    statusMessageLabel_ = new QLabel(this);
    statusMessageLabel_->setContentsMargins(8, 0, 8, 0);
    statusBar()->addWidget(statusMessageLabel_, 1);

    zoomOutButton_ = new QToolButton(this);
    zoomOutButton_->setIcon(QIcon::fromTheme(QStringLiteral("zoom-out")));
    zoomOutButton_->setToolTip(tr("Zoom out (Ctrl+-)"));
    zoomOutButton_->setAccessibleName(tr("Zoom out"));
    zoomOutButton_->setAutoRaise(true);
    zoomOutButton_->setFocusPolicy(Qt::NoFocus);
    connect(zoomOutButton_, &QToolButton::clicked, this, [this] { canvas_->zoomByStep(-1); });
    statusBar()->addPermanentWidget(zoomOutButton_);

    zoomButton_ = new QToolButton(this);
    zoomButton_->setPopupMode(QToolButton::InstantPopup);
    zoomButton_->setAutoRaise(true);
    zoomButton_->setFocusPolicy(Qt::NoFocus);
    zoomButton_->setAccessibleName(tr("Zoom preset"));
    zoomButton_->setToolTip(tr("Zoom level (Ctrl+0 Fit, Ctrl+1 100%, Ctrl/Alt+Wheel)"));
    auto *zoomMenu = new QMenu(zoomButton_);
    zoomMenu->addAction(tr("Fit in Window (Ctrl+0)"), this, [this] { canvas_->fitInView(); });
    zoomMenu->addSeparator();
    for (const qreal preset : {0.5, 1.0, 2.0, 4.0, 8.0}) {
        zoomMenu->addAction(tr("%1%").arg(int(std::lround(preset * 100.0))), this,
                            [this, preset] { canvas_->setZoomAnchored(preset, canvas_->rect().center()); });
    }
    zoomButton_->setMenu(zoomMenu);
    statusBar()->addPermanentWidget(zoomButton_);

    zoomInButton_ = new QToolButton(this);
    zoomInButton_->setIcon(QIcon::fromTheme(QStringLiteral("zoom-in")));
    zoomInButton_->setToolTip(tr("Zoom in (Ctrl++)"));
    zoomInButton_->setAccessibleName(tr("Zoom in"));
    zoomInButton_->setAutoRaise(true);
    zoomInButton_->setFocusPolicy(Qt::NoFocus);
    connect(zoomInButton_, &QToolButton::clicked, this, [this] { canvas_->zoomByStep(+1); });
    statusBar()->addPermanentWidget(zoomInButton_);

    statusClearTimer_ = new QTimer(this);
    statusClearTimer_->setSingleShot(true);
    statusClearTimer_->setInterval(4000);
    connect(statusClearTimer_, &QTimer::timeout, statusMessageLabel_, &QLabel::clear);
}

void QuickEditorWindow::wireSignals()
{
    connect(&ownedDoc_->undoStack(), &QUndoStack::cleanChanged, this, [this] { updateWindowState(); });
    connect(annotationToolbar_, &AnnotationToolbar::settingsModified, this, [this] {
        canvas_->controller()->writeSettings(store_.annotations);
        Q_EMIT settingsModified();
    });

    connect(canvas_, &AnnotationCanvas::viewChanged, this, [this] {
        updateStatusBar();
        updateScrollBars();
    });
    connect(canvas_, &AnnotationCanvas::imageDimensionsChanged, this, [this] {
        updateStatusBar();
        updateScrollBars();
    });
    connect(canvas_, &AnnotationCanvas::cropRectChanged, this, &QuickEditorWindow::updateCropStrip);
    connect(canvas_->controller(), &AnnotationController::toolChanged, this, [this](ToolKind) {
        updateCropStrip(canvas_->cropRect());
    });
    connect(applyCropButton_, &QPushButton::clicked, canvas_, &AnnotationCanvas::applyActiveCrop);
    connect(cancelCropButton_, &QPushButton::clicked, canvas_, &AnnotationCanvas::cancelActiveCrop);

    connect(hScroll_, &QScrollBar::valueChanged, this, [this](int value) {
        if (syncingBars_) {
            return;
        }
        const qreal maxPanX = canvasScrollMargin;
        canvas_->setPanOffset(QPointF(maxPanX - value, canvas_->panOffset().y()));
    });
    connect(vScroll_, &QScrollBar::valueChanged, this, [this](int value) {
        if (syncingBars_) {
            return;
        }
        const qreal maxPanY = canvasScrollMargin;
        canvas_->setPanOffset(QPointF(canvas_->panOffset().x(), maxPanY - value));
    });
}

bool QuickEditorWindow::isValid() const
{
    return valid_;
}

QString QuickEditorWindow::loadError() const
{
    return loadError_;
}

QString QuickEditorWindow::filePath() const
{
    return filePath_;
}

bool QuickEditorWindow::isDirty() const
{
    if (!valid_) {
        return false;
    }
    return isUnsavedCapture_ || !ownedDoc_->undoStack().isClean();
}

AnnotationCanvas *QuickEditorWindow::canvas() const
{
    return canvas_;
}

AnnotationToolbar *QuickEditorWindow::toolbar() const
{
    return annotationToolbar_;
}

bool QuickEditorWindow::isCropStripVisible() const
{
    return cropStrip_ && !cropStrip_->isHidden();
}

void QuickEditorWindow::setCloseDecisionOverride(CloseDecision decision)
{
    closeOverride_ = decision;
}

QString QuickEditorWindow::displayName() const
{
    if (!filePath_.isEmpty()) {
        return QFileInfo(filePath_).fileName();
    }
    return baseNameToken_.isEmpty() ? tr("Untitled capture") : baseNameToken_;
}

void QuickEditorWindow::updateWindowState()
{
    setWindowModified(isDirty());
    setWindowTitle(tr("%1[*] — glimply").arg(displayName()));
}

void QuickEditorWindow::updateStatusBar()
{
    if (!valid_) {
        dimensionsLabel_->clear();
        zoomButton_->setText(tr("100%"));
        return;
    }
    const QSize sz = canvas_->image().size();
    dimensionsLabel_->setText(tr("%1 × %2 px").arg(sz.width()).arg(sz.height()));
    zoomButton_->setText(tr("%1%").arg(int(std::lround(canvas_->zoom() * 100.0))));
}

void QuickEditorWindow::updateScrollBars()
{
    if (!valid_ || canvas_->image().isNull()) {
        hScroll_->hide();
        vScroll_->hide();
        return;
    }
    const QSizeF logical = canvas_->logicalImageSize();
    const qreal scaledW = logical.width() * canvas_->zoom();
    const qreal scaledH = logical.height() * canvas_->zoom();
    const qreal minPanX = canvas_->width() - scaledW - canvasScrollMargin;
    const qreal minPanY = canvas_->height() - scaledH - canvasScrollMargin;
    const int rangeX = std::max(0, int(std::ceil(canvasScrollMargin - minPanX)));
    const int rangeY = std::max(0, int(std::ceil(canvasScrollMargin - minPanY)));

    syncingBars_ = true;
    hScroll_->setVisible(scaledW + 2.0 * canvasScrollMargin > canvas_->width());
    vScroll_->setVisible(scaledH + 2.0 * canvasScrollMargin > canvas_->height());
    hScroll_->setRange(0, rangeX);
    hScroll_->setPageStep(canvas_->width());
    hScroll_->setValue(std::clamp(int(std::lround(canvasScrollMargin - canvas_->panOffset().x())), 0, rangeX));
    vScroll_->setRange(0, rangeY);
    vScroll_->setPageStep(canvas_->height());
    vScroll_->setValue(std::clamp(int(std::lround(canvasScrollMargin - canvas_->panOffset().y())), 0, rangeY));
    syncingBars_ = false;
}

void QuickEditorWindow::updateCropStrip(const QRect &cropRect)
{
    const bool inCrop = canvas_->controller()->tool() == ToolKind::Crop;
    cropStrip_->setVisible(inCrop);
    if (!inCrop) {
        return;
    }
    const bool ready = canvas_->hasCropRect();
    applyCropButton_->setEnabled(ready);
    if (ready) {
        const QRect r = cropRect.normalized();
        cropInfoLabel_->setText(tr("Crop: %1 × %2 at %3, %4").arg(r.width()).arg(r.height()).arg(r.x()).arg(r.y()));
    } else {
        cropInfoLabel_->setText(tr("Drag on canvas to select crop area"));
    }
}

void QuickEditorWindow::showTransientStatus(const QString &message)
{
    statusMessageLabel_->setText(message);
    statusClearTimer_->start();
}

bool QuickEditorWindow::writeExistingOrChosenPath(const QString &targetPath, QString *error)
{
    const QImage exported = canvas_->renderExport();
    if (exported.isNull()) {
        if (error) {
            *error = tr("Nothing to save.");
        }
        return false;
    }
    const bool existed = QFile::exists(targetPath);
    QSaveFile file(targetPath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    if (!existed) {
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup
                            | QFileDevice::ReadOther);
    }
    QImageWriter writer(&file, "png");
    if (!writer.write(exported) || !file.commit()) {
        if (error) {
            *error = writer.errorString().isEmpty() ? file.errorString() : writer.errorString();
        }
        return false;
    }
    return true;
}

bool QuickEditorWindow::save()
{
    if (!valid_) {
        return false;
    }
    if (canvas_->controller()->isEditingText()) {
        canvas_->controller()->requestCommitActiveText(false);
    }
    QString error;
    if (!filePath_.isEmpty()) {
        if (!writeExistingOrChosenPath(filePath_, &error)) {
            QMessageBox::warning(this, tr("Save Failed"), tr("Could not save %1:\n%2").arg(filePath_, error));
            return false;
        }
    } else {
        const OutputWriter writer(store_.effectiveCaptureDirectory(), store_.effectiveFilenameTemplate());
        QString written;
        if (!writer.writeImage(canvas_->renderExport(), baseNameToken_, QDateTime::currentDateTime(), &written,
                               &error)) {
            QMessageBox::warning(this, tr("Save Failed"), error);
            return false;
        }
        filePath_ = written;
        isUnsavedCapture_ = false;
    }

    ownedDoc_->undoStack().setClean();
    updateWindowState();
    showTransientStatus(tr("Saved %1").arg(QFileInfo(filePath_).fileName()));
    Q_EMIT savedToFile(filePath_, displayName());
    return true;
}

bool QuickEditorWindow::saveCopy()
{
    if (!valid_) {
        return false;
    }
    if (canvas_->controller()->isEditingText()) {
        canvas_->controller()->requestCommitActiveText(false);
    }
    const QFileInfo info(filePath_);
    const QString directory = !filePath_.isEmpty() ? info.absolutePath() : store_.effectiveCaptureDirectory();
    const QString stem = !filePath_.isEmpty()
                             ? info.completeBaseName() + QStringLiteral("_edited")
                             : sanitizeFilenameToken(baseNameToken_.isEmpty() ? QStringLiteral("capture")
                                                                              : baseNameToken_)
                                   + QStringLiteral("_edited");

    QString written;
    QString error;
    if (!writeUniquePngFile(directory, stem + QStringLiteral(".png"), canvas_->renderExport(), &written, &error)) {
        QMessageBox::warning(this, tr("Save Copy Failed"), error);
        return false;
    }
    showTransientStatus(tr("Saved copy as %1").arg(QFileInfo(written).fileName()));
    return true;
}

bool QuickEditorWindow::saveAs()
{
    if (!valid_) {
        return false;
    }
    if (canvas_->controller()->isEditingText()) {
        canvas_->controller()->requestCommitActiveText(false);
    }
    const QString suggestion = !filePath_.isEmpty()
                                   ? filePath_
                                   : QDir(store_.effectiveCaptureDirectory())
                                         .filePath(filenameFor(store_.effectiveFilenameTemplate(), baseNameToken_,
                                                               QDateTime::currentDateTime()));
    QString chosen = QFileDialog::getSaveFileName(this, tr("Save Image As"), suggestion, tr("PNG images (*.png)"));
    if (chosen.isEmpty()) {
        return false;
    }
    if (!chosen.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) {
        chosen.append(QStringLiteral(".png"));
    }
    QString error;
    if (!writeExistingOrChosenPath(chosen, &error)) {
        QMessageBox::warning(this, tr("Save Failed"), tr("Could not save %1:\n%2").arg(chosen, error));
        return false;
    }
    filePath_ = QFileInfo(chosen).absoluteFilePath();
    baseNameToken_ = QFileInfo(filePath_).completeBaseName();
    isUnsavedCapture_ = false;
    ownedDoc_->undoStack().setClean();
    updateWindowState();
    showTransientStatus(tr("Saved %1").arg(QFileInfo(filePath_).fileName()));
    Q_EMIT savedToFile(filePath_, displayName());
    return true;
}

void QuickEditorWindow::copyToClipboard()
{
    if (!valid_) {
        return;
    }
    if (canvas_->controller()->isEditingText()) {
        canvas_->controller()->requestCommitActiveText(false);
    }
    const QImage exported = canvas_->renderExport();
    if (exported.isNull()) {
        return;
    }
    QGuiApplication::clipboard()->setImage(exported);
    showTransientStatus(tr("Copied to clipboard"));
}

void QuickEditorWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_C && event->modifiers() == Qt::ControlModifier
        && !canvas_->controller()->isEditingText()) {
        if (!qobject_cast<QLineEdit *>(focusWidget()) && !qobject_cast<QPlainTextEdit *>(focusWidget())) {
            copyToClipboard();
            event->accept();
            return;
        }
    }
    QMainWindow::keyPressEvent(event);
}

void QuickEditorWindow::closeEvent(QCloseEvent *event)
{
    if (canvas_->controller()->isEditingText()) {
        canvas_->controller()->requestCommitActiveText(false);
    }
    if (!isDirty()) {
        event->accept();
        return;
    }

    CloseDecision decision = closeOverride_;
    if (decision == CloseDecision::AskUser) {
        QMessageBox box(QMessageBox::Warning, tr("Unsaved Changes"),
                        tr("Save changes to “%1” before closing?").arg(displayName()), QMessageBox::NoButton, this);
        QPushButton *saveBtn = box.addButton(tr("Save"), QMessageBox::AcceptRole);
        QPushButton *discardBtn = box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
        QPushButton *cancelBtn = box.addButton(tr("Cancel"), QMessageBox::RejectRole);
        box.setDefaultButton(saveBtn);
        box.setEscapeButton(cancelBtn);
        box.exec();
        if (box.clickedButton() == saveBtn) {
            decision = CloseDecision::Save;
        } else if (box.clickedButton() == discardBtn) {
            decision = CloseDecision::Discard;
        } else {
            decision = CloseDecision::Cancel;
        }
    }

    if (decision == CloseDecision::Save) {
        if (save()) {
            event->accept();
        } else {
            event->ignore();
        }
    } else if (decision == CloseDecision::Discard) {
        event->accept();
    } else {
        event->ignore();
    }
}

} // namespace glimply
