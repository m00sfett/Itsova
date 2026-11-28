#pragma once

#include "annotations/annotationcontroller.h"
#include "annotations/annotationdocument.h"
#include "annotations/annotationitem.h"
#include "captureservice.h"
#include "selectionmodel.h"
#include "targets.h"
#include "windowlist.h"
#include "selectiongeometry.h"
#include "windowpick.h"

#include <QDateTime>
#include <QFuture>
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
#include <optional>

class QAbstractButton;
class QLabel;
class QTimer;

namespace itsova
{

class App;
class CapsulePanel;
class CaptureOverlay;
class PanelButton;
class InlineTextEditor;
struct RenderedOutput;

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
    // ITSOVA_LATENCY_BENCH set: closes a Print session once its latency trace is complete, as if
    // Escape kept the full screenshot. Used by scripts/measure-latency.sh in a nested KWin (#67).
    void finishLatencyBenchIfDone();
    void setForcedFrozen(std::optional<bool> frozen);
    void setForceShowOverlay(bool force);

    // State for the overlays.
    [[nodiscard]] Purpose purpose() const;
    [[nodiscard]] bool frozen() const;
    [[nodiscard]] bool canSwitchToLive() const;
    [[nodiscard]] QImage frame(const QString &connector) const;
    [[nodiscard]] qreal frameScale(const QString &connector) const;
    // The loupe's pixels (#101): the frozen frame, or the Print snapshot while live (snapshot set).
    [[nodiscard]] QImage loupeFrame(const QString &connector, bool *snapshot = nullptr) const;
    // Native pixels per logical pixel of a monitor: from its capture, else from Qt.
    [[nodiscard]] qreal monitorScale(const QString &connector) const;
    // The native pixels globalRect covers on connector's monitor, rounded like the saved crop.
    [[nodiscard]] QRect nativeSelectionOn(const QRect &globalRect, const QString &connector) const;
    // The pixel size the saved file of globalRect will have.
    [[nodiscard]] QSize nativeSize(const QRect &globalRect) const;
    // The same with the selection's native nudge (#102).
    [[nodiscard]] QRect nativeSelectionOn(const Selection &selection, const QString &connector) const;
    [[nodiscard]] QSize nativeSize(const Selection &selection) const;
    // The selection drawn on screen: its logical rectangle moved by its nudge (logical, fractional).
    [[nodiscard]] QRectF displayRect(const Selection &selection) const;
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
    [[nodiscard]] bool isDragging() const;
    [[nodiscard]] bool dragLoupeActive() const;
    [[nodiscard]] QPoint dragCursorGlobal() const;
    // edges: the corner or edge being dragged (the loupe centres on it); keyNudge: the arrow-key dead
    // zone holds the mouse (#102).
    void setDragLoupe(bool active, const QPoint &globalPos = {}, Qt::Edges edges = {}, bool keyNudge = false);
    [[nodiscard]] Qt::Edges dragEdges() const { return dragEdges_; }
    [[nodiscard]] bool dragKeyNudge() const { return dragKeyNudge_; }
    [[nodiscard]] bool colorPickerActive() const;
    void setColorPickerActive(bool active);
    [[nodiscard]] QPoint pickerCursorGlobal() const;
    void setPickerCursorGlobal(const QPoint &globalPos);
    void cycleColorFormat();
    void pickColorAt(const QPoint &globalPos, bool applyToTool);
    [[nodiscard]] int hoveredSelectionId() const;
    void setHoveredSelectionId(int id);
    [[nodiscard]] QRect desktopBounds() const;
    [[nodiscard]] QList<QRect> screenRects() const;
    [[nodiscard]] QList<ScreenInfo> intersectingScreens(const QRect &globalRect) const;
    [[nodiscard]] QStringList intersectingConnectors(const QRect &globalRect) const;
    [[nodiscard]] QString actionBarConnector(const Selection &selection) const;
    // Opens the "Save as Area" form in the panel for the active rectangle.
    void showAreaPanel();
    // Closes that form, if it is open.
    void dismissAreaPanels();
    [[nodiscard]] QString hint() const;
    // The one-time first-start hint is due (#72); gone after the first button hover or F1.
    [[nodiscard]] bool firstHintPending() const;
    void dismissFirstHint();

    // The one panel of the session (#69). It shows on one monitor: where the pointer is while
    // nothing is selected, then where the selection is; a monitor the user dragged it to wins.
    enum class PanelDrawer { None, Window, Areas, Delay, Markup, More, SaveArea, Help, Record, Monitors, Audio };
    [[nodiscard]] PanelDrawer panelDrawer() const;
    void setPanelDrawer(PanelDrawer drawer);
    // A second click on the button of the open drawer closes it.
    void togglePanelDrawer(PanelDrawer drawer);
    [[nodiscard]] QString panelConnector() const;
    void notePointerOn(const QString &connector);
    // Runs entry index (0-based) of the list drawer open on the panel (keys 1–9, Q24).
    void pickPanelEntry(int index);
    // Hover-and-click picking (#97): what a click without a drag would take at globalPos, or None
    // while picking does not apply (a selection, annotations, markup, the colour picker).
    [[nodiscard]] PickTarget pickAt(const QPoint &globalPos) const;
    [[nodiscard]] bool pickingActive() const;
    // The target framed under the pointer; repaints every overlay when it changes.
    void setPointerPick(const PickTarget &pick);
    // The pointer position the overlays last saw (QCursor::pos() is stale on Wayland).
    void notePointerPosition(const QPoint &globalPos);
    [[nodiscard]] const PickTarget &pointerPick() const;
    // Takes a picked target: captures it, or records it in record mode.
    void takePick(const PickTarget &pick);
    // The overlay that shows the panel (keys typed on another monitor's overlay go there, #96).
    [[nodiscard]] CaptureOverlay *panelOverlay() const;
    // Where the user put the panel on this monitor (top-left, monitor coordinates), if anywhere.
    [[nodiscard]] std::optional<QPoint> panelPosition(const QString &connector) const;
    void setPanelPosition(const QString &connector, const std::optional<QPoint> &position);
    // The panel was dropped at globalTopLeft with the pointer at globalPointer, on another monitor.
    void movePanelToMonitor(const QPoint &globalPointer, const QPoint &globalTopLeft);
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
    // The monitors of the session, left to right, top to bottom (#95).
    [[nodiscard]] QList<ScreenInfo> orderedScreens() const;

    // Actions.
    void save(const QString &preferredConnector = QString());
    void copy(const QString &preferredConnector = QString());
    void saveAs();
    void editInWindow(const QString &preferredConnector = QString());
    // Pins every rectangle (with annotations) to the screen and closes the overlay.
    void pin();
    // Recognizes the text of every rectangle (without annotations; the whole monitor when there
    // is none) and closes the overlay (#27).
    void copyText(const QString &preferredConnector = QString());
    // Text mode: no Print snapshot is saved and Enter copies text instead of saving.
    void setTextMode(bool on);
    // Screen recording (#30): the session only chooses what to record. It starts live, saves no
    // Print file and opens the Record drawer.
    void setRecordMode(bool on);
    [[nodiscard]] bool recordMode() const { return recordMode_; }
    [[nodiscard]] bool recordingAudioAvailable() const;
    [[nodiscard]] RecordingAudioSelection recordingAudioSelection() const { return recordingAudioSelection_; }
    void toggleRecordingAudio(RecordingAudioSource source);
    // Closes the overlay and records targetId (see App::startRecording) once it is gone, after the
    // delay chosen in the panel.
    void record(const QString &targetId);
    // Records the selection (all rectangles: their bounding box).
    void recordSelection();
    // The default recording target (#124): pinned in record mode, recorded by the shortcut.
    [[nodiscard]] QString defaultRecordingTarget() const;
    void toggleDefaultRecordingTarget(const QString &targetId);
    // The recording target of the current selection ("region:…"), empty without one.
    [[nodiscard]] QString selectionRecordingTargetId() const;
    // "Default for <shortcut>", or without a bound shortcut "Default for the recording shortcut".
    [[nodiscard]] QString defaultRecordingTip() const;
    [[nodiscard]] bool textMode() const;
    // Returns an error message, or an empty string when the areas were saved.
    QString saveAsAreas(const QString &name, const QKeySequence &shortcut, bool combine, int delaySeconds = 0);
    void captureMonitor(const QString &connector);
    void captureAllMonitors();
    void captureTarget(const QString &id);
    void addTarget(const QString &id);
    void captureWindow(const WindowInfo &window);
    void setFrozen(bool frozen);
    void retakeWithDelay(int seconds);
    [[nodiscard]] int captureDelaySeconds() const;
    [[nodiscard]] bool keepDelayActive() const;
    void setCaptureDelay(int seconds, bool keepActive);
    [[nodiscard]] bool helpExpanded() const;
    void toggleHelp();
    void openSettings();
    // Escape, right click, Close: keeps the full screenshot (Print, prompting if redacted) or discards (Add Area).
    void close();

    // Repaints every overlay and moves the bars.
    void changed();

Q_SIGNALS:
    void finished();
    void defaultRecordingTargetChanged();

private:
    void captureFrames(const QStringList &connectors);
    // Runs next with up-to-date frames: right away when frozen, otherwise after hiding the
    // overlays long enough for the compositor to drop them from the screen.
    void withFrames(const QStringList &connectors, const std::function<void()> &next);
    void showOverlays();
    void hideOverlays();
    void savePrintImage();
    // The background write of the full Print screenshot (#67).
    struct PrintWrite
    {
        QImage image;
        QStringList paths;
        QStringList errors;
    };
    void waitForPrintWrite();
    void applyPrintWrite();
    // The full screenshot's path; waits for a pending background write first.
    QString &printPath();
    [[nodiscard]] const ScreenInfo *screenFor(const QString &connector) const;
    void handleInlineTextRequested(const AnnotationItem &item, bool isExisting);
    void removePrintSnapshotIfRedacted();
    // Writes images that share one timestamp. Returns the written paths.
    QStringList write(const QList<QPair<QString, QImage>> &images, QStringList *errors);
    QStringList write(const QList<QPair<QString, QImage>> &images, QStringList *errors, const ExportOptions &options);
    QStringList writeOutputs(const QList<RenderedOutput> &outputs, QStringList *errors);
    // Ends the session with these files; they replace the full screenshot.
    void finishWith(const QStringList &paths, const QImage &clipboardImage, const QStringList &errors,
                    const QString &displayName, const QString &targetId = QString());
    void finish(const QString &title, const QStringList &paths, const QStringList &errors = {});
    [[nodiscard]] QImage cropSelectionImage(const Selection &selection, bool includeAnnotations, qreal *outScale,
                                          QString *error) const;
    [[nodiscard]] QImage selectionImage(const Selection &selection, QString *error) const;

    App *app_;
    Purpose purpose_;
    std::optional<bool> forcedFrozen_;
    bool forceShowOverlay_ = false;
    bool textMode_ = false;
    bool recordMode_ = false;
    RecordingAudioSelection recordingAudioSelection_;
    bool frozen_ = true;
    bool forceNew_ = false;
    bool dragging_ = false;
    bool dragLoupeActive_ = false;
    QPoint dragCursorGlobal_;
    Qt::Edges dragEdges_;
    bool dragKeyNudge_ = false;
    bool colorPickerActive_ = false;
    QPoint pickerCursorGlobal_;
    QString transientHint_;
    int hoveredSelectionId_ = 0;
    bool finished_ = false;
    bool switching_ = false; // Live to Frozen: overlays hidden while the frames are taken
    QRect desktopBounds_;
    QList<ScreenInfo> screens_;
    QHash<QString, CaptureResult> frames_;
    QHash<QString, CaptureResult> printFrames_; // the Print snapshot, kept for the live loupe (#101)
    SelectionModel selections_;
    AnnotationDocument annotations_;
    AnnotationController annotationController_;
    QTimer *transientHintTimer_ = nullptr;
    QList<Highlight> highlights_;
    QList<WindowInfo> pickWindows_; // loaded in the background after the overlay shows (#97)
    PickTarget pointerPick_;
    QPoint pointerGlobal_;
    QList<QPointer<CaptureOverlay>> overlays_;
    PanelDrawer panelDrawer_ = PanelDrawer::None;
    QString pointerConnector_;
    QString manualPanelConnector_;
    QString printPath_;
    QFuture<PrintWrite> printWrite_;
    bool printWritePending_ = false;
};

// The overlay on one monitor: shows the picture (or the live screen), annotations, rectangles, and
// the bars. All state lives in the session; the overlay paints it and turns input into actions.
class CaptureOverlay : public QWidget
{
    Q_OBJECT

public:
    CaptureOverlay(CaptureSession *session, const ScreenInfo &screen);

    [[nodiscard]] QString connector() const;
    void pickListEntry(int index);
    [[nodiscard]] const ScreenInfo &screen() const;
    [[nodiscard]] InlineTextEditor *textEditor() const;
    void startInlineTextEdit(const AnnotationItem &item, bool isExisting);
    void stopInlineTextEdit();
    void refresh();

protected:
    bool focusNextPrevChild(bool next) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class Drag { None, Pending, Create, Move, Resize };

    void buildPanel();
    // "start", "selection" or "area": which main row the panel shows.
    [[nodiscard]] QString panelState() const;
    void rebuildMainRow(const QString &state);
    // What a drawer shows depends on more than its kind; it is rebuilt when this changes.
    [[nodiscard]] QString drawerSignature(CaptureSession::PanelDrawer drawer) const;
    [[nodiscard]] QWidget *buildDrawer(CaptureSession::PanelDrawer drawer);
    [[nodiscard]] QWidget *buildHelpPanel(QWidget *parent) const;
    [[nodiscard]] QWidget *buildWindowList();
    [[nodiscard]] QWidget *buildRecordList();
    [[nodiscard]] QWidget *buildMonitorList();
    [[nodiscard]] QWidget *buildAreaList();
    [[nodiscard]] QWidget *buildDelayRow();
    [[nodiscard]] QWidget *buildAudioRow();
    [[nodiscard]] QWidget *buildMarkupDrawer();
    [[nodiscard]] QWidget *buildMoreRow();
    [[nodiscard]] QWidget *buildAreaForm();
    void refreshPanel();
    void placePanel();
    void placeHint();
    void applyDrag(const QPoint &globalPos, Qt::KeyboardModifiers modifiers);
    bool reapplyModifiers(QKeyEvent *event);
    bool handlePanelKey(int key, Qt::KeyboardModifiers modifiers);
    // Arrow keys, Enter and Escape through the panel and its drawers (#96); true when handled.
    bool navigatePanel(QKeyEvent *event);
    [[nodiscard]] QList<QAbstractButton *> drawerEntries() const;
    void updateCursor(const QPoint &globalPos);
    [[nodiscard]] QPoint toGlobalPoint(const QPoint &localPos) const;
    [[nodiscard]] QRect toLocalRect(const QRect &globalRect) const;
    [[nodiscard]] QPointF toGlobalLogical(const QPointF &localPos) const;
    [[nodiscard]] Qt::Edges edgesAt(const QRect &globalRect, const QPoint &globalPos) const;
    [[nodiscard]] QRect closeButtonGlobal(const QRect &globalRect) const;
    // Rectangle whose delete button is under globalPos (active one, or the hovered one), or 0.
    [[nodiscard]] int closeButtonAt(const QPoint &globalPos) const;

    CaptureSession *session_;
    ScreenInfo screen_;

    Drag drag_ = Drag::None;
    bool spaceHeld_ = false;
    int dragId_ = 0;
    int pendingId_ = 0;
    QPoint anchor_;
    QPoint lastPosition_;
    QRect dragStartRect_;
    Qt::Edges dragEdges_;
    // Arrow-key nudging while a corner or edge is dragged (#102).
    NudgeDeadZone nudgeZone_;
    QMargins dragStartNudge_;
    QPoint nudgeTotal_;   // native pixels added by arrow keys in this drag
    QPoint lastDragPos_;  // the position the drag last used (pointer plus the dead zone's offset)
    [[nodiscard]] Qt::Edges currentDragEdges(const QPoint &dragPos) const;
    // Once a drag was nudged, Shift means "10 pixels" for the arrows, not "square".
    [[nodiscard]] Qt::KeyboardModifiers dragModifiers(Qt::KeyboardModifiers modifiers) const;

    InlineTextEditor *textEditor_ = nullptr;
    CapsulePanel *panel_ = nullptr;
    QWidget *hintPanel_ = nullptr;
    QLabel *hint_ = nullptr;
    QString builtState_;
    CaptureSession::PanelDrawer builtDrawer_ = CaptureSession::PanelDrawer::None;
    QString builtDrawerSignature_;
    // Actions of the open list drawer, in row order, for the keys 1–9 (Q24); empty = unavailable.
    QList<std::function<void()>> listEntries_;
    QHash<int, QPointer<PanelButton>> drawerButtons_;
    QList<QPointer<PanelButton>> mainButtons_;
    QPointer<PanelButton> saveButton_;
    QPointer<PanelButton> delayButton_;
    QPointer<PanelButton> audioButton_;
    QList<QPointer<PanelButton>> audioOptions_;
    bool panelDragging_ = false;
    QPoint panelGrabOffset_;
};

} // namespace itsova
