#pragma once

#include "annotations/annotationcontroller.h"
#include "annotations/annotationdocument.h"
#include "annotations/annotationitem.h"
#include "captureservice.h"
#include "selectionmodel.h"
#include "targets.h"
#include "windowlist.h"

#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QStringList>
#include <QWidget>

#include <functional>

class QLabel;
class QPushButton;
class QToolButton;

namespace glimply
{

class AnnotationToolbar;
class App;
class CaptureOverlay;
class InlineTextEditor;

// One Print key press (or one "Add Area") from start to the saved files. Owns the overlays (one
// per monitor), the rectangles, vector annotations and the frames they are cut from.
// Frozen: the overlays show the frames taken when the session started.
// Live: the overlays are see-through and the frames are taken when the user confirms, with the overlays hidden.
class CaptureSession : public QObject
{
    Q_OBJECT

public:
    enum class Purpose {
        Print, // the full screenshot is saved first; the overlay narrows it down
        DefineArea, // only defines areas, nothing is captured
    };

    CaptureSession(App *app, Purpose purpose);
    ~CaptureSession() override;

    void start();
    // Print pressed again while the overlay is open: a fresh full screenshot, rectangles stay.
    void printPressed();

    // State for the overlays.
    [[nodiscard]] Purpose purpose() const;
    [[nodiscard]] bool frozen() const;
    [[nodiscard]] bool canSwitchToLive() const;
    [[nodiscard]] QImage frame(const QString &connector) const;
    [[nodiscard]] SelectionModel &selections();
    [[nodiscard]] TargetStore &store() const;
    [[nodiscard]] AnnotationDocument &annotations();
    [[nodiscard]] const AnnotationDocument &annotations() const;
    [[nodiscard]] AnnotationController &annotationController();
    [[nodiscard]] const AnnotationController &annotationController() const;
    [[nodiscard]] bool markupVisible() const;
    void setMarkupVisible(bool visible);
    void activateMarkupTool(ToolKind tool);
    void persistAnnotationSettings();
    [[nodiscard]] bool forceNew() const;
    void setForceNew(bool on);
    void setDragging(bool dragging);
    [[nodiscard]] QString hint() const;
    // Outlines shown while the user hovers an area or a window in a list (global logical rects).
    struct Highlight {
        QRect rect;
        QString label;
    };
    [[nodiscard]] const QList<Highlight> &highlights() const;
    void setHighlights(const QList<Highlight> &highlights);
    [[nodiscard]] QList<WindowInfo> windows() const;
    [[nodiscard]] QKeySequence shortcut(const QString &id) const;
    [[nodiscard]] bool isConnected(const QString &connector) const;
    // The user's name for the monitor, or its connector.
    [[nodiscard]] QString monitorName(const QString &connector) const;

    // Actions.
    void save(const QString &preferredConnector = QString());
    void copy(const QString &preferredConnector = QString());
    void saveAs();
    // Returns an error message, or an empty string when the areas were saved.
    QString saveAsAreas(const QString &name, const QKeySequence &shortcut, bool combine);
    void captureMonitor(const QString &connector);
    void captureAllMonitors();
    void captureTarget(const QString &id);
    void addTarget(const QString &id);
    void captureWindow(const WindowInfo &window);
    void setFrozen(bool frozen);
    [[nodiscard]] bool helpExpanded() const;
    void toggleHelp();
    void openSettings();
    // Escape, right click, Close: keeps the full screenshot (Print, prompting if redacted) or discards (Add Area).
    void close();

    // Repaints every overlay and moves the bars.
    void changed();

Q_SIGNALS:
    void finished();

private:
    void captureFrames(const QStringList &connectors);
    // Runs next with up-to-date frames: right away when frozen, otherwise after hiding the
    // overlays long enough for the compositor to drop them from the screen.
    void withFrames(const QStringList &connectors, const std::function<void()> &next);
    void showOverlays();
    void hideOverlays();
    void savePrintImage();
    [[nodiscard]] const ScreenInfo *screenFor(const QString &connector) const;
    void handleInlineTextRequested(const AnnotationItem &item, bool isExisting);
    void removePrintSnapshotIfRedacted();
    // Writes images that share one timestamp. Returns the written paths.
    QStringList write(const QList<QPair<QString, QImage>> &images, QStringList *errors);
    // Ends the session with these files; they replace the full screenshot.
    void finishWith(const QStringList &paths, const QImage &clipboardImage, const QStringList &errors,
                    const QString &displayName);
    void finish(const QString &title, const QStringList &paths, const QStringList &errors = {});
    [[nodiscard]] QImage selectionImage(const Selection &selection, QString *error) const;

    App *app_;
    Purpose purpose_;
    bool frozen_ = true;
    bool markupVisible_ = false;
    bool forceNew_ = false;
    bool dragging_ = false;
    bool finished_ = false;
    bool switching_ = false; // Live to Frozen: overlays hidden while the frames are taken
    QList<ScreenInfo> screens_;
    QHash<QString, CaptureResult> frames_;
    SelectionModel selections_;
    AnnotationDocument annotations_;
    AnnotationController annotationController_;
    QList<Highlight> highlights_;
    QList<QPointer<CaptureOverlay>> overlays_;
    QString printPath_;
};

// The overlay on one monitor: shows the picture (or the live screen), annotations, rectangles, and
// the bars. All state lives in the session; the overlay paints it and turns input into actions.
class CaptureOverlay : public QWidget
{
    Q_OBJECT

public:
    CaptureOverlay(CaptureSession *session, const ScreenInfo &screen);

    [[nodiscard]] QString connector() const;
    [[nodiscard]] const ScreenInfo &screen() const;
    [[nodiscard]] InlineTextEditor *textEditor() const;
    void startInlineTextEdit(const AnnotationItem &item, bool isExisting);
    void stopInlineTextEdit();
    void refresh();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    enum class Drag { None, Pending, Create, Move, Resize };

    void buildTopBar();
    void buildMarkupBar();
    [[nodiscard]] QWidget *buildHelpPanel(QWidget *parent) const;
    void buildActionBar();
    void showAreaPanel();
    void showActionButtons();
    void placeActionBar();
    void fillAreasMenu();
    void fillWindowsMenu();
    void applyDrag(const QPoint &position, Qt::KeyboardModifiers modifiers);
    bool reapplyModifiers(QKeyEvent *event);
    void updateCursor(const QPoint &pos);
    void saveArea();
    [[nodiscard]] QPointF toGlobalLogical(const QPointF &localPos) const;
    [[nodiscard]] const Selection *activeHere() const;
    [[nodiscard]] Qt::Edges edgesAt(const QRect &rect, const QPoint &pos) const;
    [[nodiscard]] QRect closeButton(const QRect &rect) const;
    // Rectangle whose delete button is under pos (active one, or the hovered one), or 0.
    [[nodiscard]] int closeButtonAt(const QPoint &pos) const;

    CaptureSession *session_;
    ScreenInfo screen_;

    Drag drag_ = Drag::None;
    bool spaceHeld_ = false;
    int dragId_ = 0;
    int pendingId_ = 0;
    int hoverId_ = 0;
    QPoint anchor_;
    QPoint lastPosition_;
    QRect dragStartRect_;
    Qt::Edges dragEdges_;

    QWidget *topBar_ = nullptr;
    QWidget *markupPanel_ = nullptr;
    AnnotationToolbar *annotationToolbar_ = nullptr;
    InlineTextEditor *textEditor_ = nullptr;
    QLabel *hint_ = nullptr;
    QToolButton *annotateButton_ = nullptr;
    QToolButton *helpButton_ = nullptr;
    QWidget *helpPanel_ = nullptr;
    QToolButton *frozenButton_ = nullptr;
    QToolButton *liveButton_ = nullptr;
    QWidget *actionBar_ = nullptr;
    QWidget *actionButtons_ = nullptr;
    QWidget *areaPanel_ = nullptr;
    QLabel *sizeLabel_ = nullptr;
    QPushButton *saveButton_ = nullptr;
};

} // namespace glimply
