#pragma once

#include "annotations/annotationcanvas.h"
#include "annotations/annotationdocument.h"
#include "annotations/annotationtoolbar.h"
#include "targets.h"

#include <QImage>
#include <QMainWindow>
#include <QString>

#include <memory>

class QAction;
class QLabel;
class QPushButton;
class QScrollBar;
class QTimer;
class QToolButton;

namespace glimply
{

// Standalone native KDE/Breeze quick editor window opened from CaptureOverlay ("Edit in Window"),
// capture notifications ("Edit"), tray/widget ("Edit Last Capture"), or CLI/file-open arguments.
class QuickEditorWindow : public QMainWindow
{
    Q_OBJECT

public:
    enum class CloseDecision { AskUser, Save, Discard, Cancel };

    // Opens an existing image file from disk.
    QuickEditorWindow(TargetStore &store, const QString &filePath, QWidget *parent = nullptr);

    // Opens a raw native capture handed off from CaptureOverlay together with live editable annotations.
    QuickEditorWindow(TargetStore &store,
                      const QImage &nativeImage,
                      qreal scale,
                      std::unique_ptr<AnnotationDocument> annotations,
                      const QString &baseNameToken,
                      QWidget *parent = nullptr);

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] QString loadError() const;
    [[nodiscard]] QString filePath() const;
    [[nodiscard]] bool isDirty() const;

    [[nodiscard]] AnnotationCanvas *canvas() const;
    [[nodiscard]] AnnotationToolbar *toolbar() const;
    [[nodiscard]] bool isCropStripVisible() const;

    void setCloseDecisionOverride(CloseDecision decision);

public Q_SLOTS:
    bool save();
    bool saveCopy();
    bool saveAs();
    void copyToClipboard();

Q_SIGNALS:
    void savedToFile(const QString &filePath, const QString &displayName);
    void settingsModified();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void buildUi();
    void wireSignals();
    void updateWindowState();
    void updateStatusBar();
    void updateScrollBars();
    void updateCropStrip(const QRect &cropRect);
    void showTransientStatus(const QString &message);
    [[nodiscard]] QString displayName() const;
    [[nodiscard]] bool writeExistingOrChosenPath(const QString &targetPath, const ExportOptions &options,
                                                   QString *error);

    TargetStore &store_;
    ExportOptions exportOptions_;
    QString filePath_;
    QString baseNameToken_;
    QString loadError_;
    bool valid_ = false;
    bool isUnsavedCapture_ = false;
    bool syncingBars_ = false;
    CloseDecision closeOverride_ = CloseDecision::AskUser;

    std::unique_ptr<AnnotationDocument> ownedDoc_;
    AnnotationCanvas *canvas_ = nullptr;
    AnnotationToolbar *annotationToolbar_ = nullptr;

    QWidget *cropStrip_ = nullptr;
    QLabel *cropInfoLabel_ = nullptr;
    QPushButton *applyCropButton_ = nullptr;
    QPushButton *cancelCropButton_ = nullptr;

    QScrollBar *hScroll_ = nullptr;
    QScrollBar *vScroll_ = nullptr;

    QAction *saveAction_ = nullptr;
    QAction *saveCopyAction_ = nullptr;
    QAction *saveAsAction_ = nullptr;
    QAction *copyAction_ = nullptr;

    QLabel *dimensionsLabel_ = nullptr;
    QLabel *statusMessageLabel_ = nullptr;
    QTimer *statusClearTimer_ = nullptr;
    QToolButton *zoomOutButton_ = nullptr;
    QToolButton *zoomButton_ = nullptr;
    QToolButton *zoomInButton_ = nullptr;
};

} // namespace glimply
