#include "annotations/annotationdocument.h"
#include "annotations/annotationitem.h"
#include "annotations/overlayexport.h"
#include "areaselector.h"
#include "quickeditorwindow.h"
#include "targets.h"

#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>

using namespace glimply;

class OverlayMarkupTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void areaOverlayPhotoCraftModifiersAndSpaceDrag();
    void multiMonitorAnnotationMappingAndAllMonitors();
    void redactionPrintLeakGuards();
    void rtlCaptureGeometryAndQuickEditorLayout();
};

void OverlayMarkupTest::areaOverlayPhotoCraftModifiersAndSpaceDrag()
{
    QImage bg(600, 400, QImage::Format_RGB32);
    bg.fill(Qt::white);
    AreaOverlay overlay(QStringLiteral("DP-1"), QSize(600, 400), bg, QRect());
    overlay.resize(600, 400);

    // 1. Shift+Drag constrains to 1:1 square.
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(100, 100), QPointF(100, 100), Qt::LeftButton, Qt::LeftButton,
                      Qt::ShiftModifier);
    QMouseEvent moveShift(QEvent::MouseMove, QPointF(220, 140), QPointF(220, 140), Qt::NoButton, Qt::LeftButton,
                          Qt::ShiftModifier);
    QApplication::sendEvent(&overlay, &press);
    QApplication::sendEvent(&overlay, &moveShift);
    QCOMPARE(overlay.selection(), QRect(100, 100, 120, 120));

    // 2. Space held mid-drag translates in-progress square cleanly.
    QKeyEvent spaceDown(QEvent::KeyPress, Qt::Key_Space, Qt::ShiftModifier);
    QApplication::sendEvent(&overlay, &spaceDown);
    QMouseEvent moveSpace(QEvent::MouseMove, QPointF(260, 190), QPointF(260, 190), Qt::NoButton, Qt::LeftButton,
                          Qt::ShiftModifier);
    QApplication::sendEvent(&overlay, &moveSpace);
    QCOMPARE(overlay.selection(), QRect(140, 150, 120, 120));

    QKeyEvent spaceUp(QEvent::KeyRelease, Qt::Key_Space, Qt::ShiftModifier);
    QApplication::sendEvent(&overlay, &spaceUp);
    QMouseEvent rel(QEvent::MouseButtonRelease, QPointF(260, 190), QPointF(260, 190), Qt::LeftButton, Qt::NoButton,
                    Qt::NoModifier);
    QApplication::sendEvent(&overlay, &rel);
    QCOMPARE(overlay.selection(), QRect(140, 150, 120, 120));

    // 3. Alt+Drag draws symmetrically from centre.
    QMouseEvent pAlt(QEvent::MouseButtonPress, QPointF(300, 200), QPointF(300, 200), Qt::LeftButton, Qt::LeftButton,
                     Qt::AltModifier);
    QMouseEvent mAlt(QEvent::MouseMove, QPointF(350, 225), QPointF(350, 225), Qt::NoButton, Qt::LeftButton,
                     Qt::AltModifier);
    QMouseEvent rAlt(QEvent::MouseButtonRelease, QPointF(350, 225), QPointF(350, 225), Qt::LeftButton, Qt::NoButton,
                     Qt::AltModifier);
    QApplication::sendEvent(&overlay, &pAlt);
    QApplication::sendEvent(&overlay, &mAlt);
    QApplication::sendEvent(&overlay, &rAlt);
    QCOMPARE(overlay.selection(), QRect(250, 175, 100, 50));

    // 4. Unified arrow keys: Shift+Right moves +10, Alt+Right resizes +1 per side around centre, Ctrl+Up makes square.
    QKeyEvent shiftRight(QEvent::KeyPress, Qt::Key_Right, Qt::ShiftModifier);
    QApplication::sendEvent(&overlay, &shiftRight);
    QCOMPARE(overlay.selection(), QRect(260, 175, 100, 50));

    QKeyEvent altRight(QEvent::KeyPress, Qt::Key_Right, Qt::AltModifier);
    QApplication::sendEvent(&overlay, &altRight);
    QCOMPARE(overlay.selection(), QRect(259, 175, 102, 50));
}

void OverlayMarkupTest::multiMonitorAnnotationMappingAndAllMonitors()
{
    ScreenInfo left{QStringLiteral("DP-1"), QRect(0, 0, 800, 600), {}, {}, {}};
    ScreenInfo right{QStringLiteral("HDMI-A-1"), QRect(800, 0, 640, 480), {}, {}, {}};
    const QList<ScreenInfo> screens{left, right};

    QImage leftImg(1000, 750, QImage::Format_RGB32); // scale 1.25
    leftImg.fill(Qt::white);
    QImage rightImg(640, 480, QImage::Format_RGB32); // scale 1.0
    rightImg.fill(QColor(240, 240, 240));
    QHash<QString, CaptureResult> frames{
        {left.connector, {leftImg, 1.25, {}}},
        {right.connector, {rightImg, 1.0, {}}},
    };

    AnnotationDocument doc;
    // Callout on DP-1 only (global 100,100 -> 260,100).
    AnnotationItem arrow;
    arrow.kind = ItemKind::Arrow;
    arrow.start = QPointF(100, 100);
    arrow.end = QPointF(260, 100);
    arrow.color = QColor(0xE5, 0x39, 0x35);
    arrow.strokeWidth = 6.0;
    doc.addItem(arrow);

    // Redaction on HDMI-A-1 (global x=900..980 -> local x=100..180).
    AnnotationItem redact;
    redact.kind = ItemKind::Redact;
    redact.redactionMode = RedactionMode::Solid;
    redact.color = Qt::black;
    redact.rect = QRectF(900, 120, 80, 50);
    doc.addItem(redact);

    QCOMPARE(annotatedConnectors(screens, doc), QStringList({QStringLiteral("DP-1"), QStringLiteral("HDMI-A-1")}));

    const QImage renderedLeft = renderAnnotatedMonitor(left, frames.value(left.connector), doc);
    QVERIFY(renderedLeft.pixelColor(int(160 * 1.25), int(100 * 1.25)).red() > 200);
    QCOMPARE(renderedLeft.pixelColor(200, 180), QColor(Qt::white));

    const QImage renderedRight = renderAnnotatedMonitor(right, frames.value(right.connector), doc);
    QCOMPARE(renderedRight.pixelColor(140, 145), QColor(0, 0, 0));
    QCOMPARE(renderedRight.pixelColor(50, 50), QColor(240, 240, 240));

    QString error;
    const QImage renderedAll = renderAnnotatedAllMonitors(screens, frames, doc, &error);
    QVERIFY(!renderedAll.isNull());
    QVERIFY(error.isEmpty());
    // Max scale across monitors is 1.25: arrow at (160*1.25, 100*1.25), redaction on HDMI at (940*1.25, 145*1.25).
    QVERIFY(renderedAll.pixelColor(int(160 * 1.25), int(100 * 1.25)).red() > 200);
    QCOMPARE(renderedAll.pixelColor(int(940 * 1.25), int(145 * 1.25)), QColor(0, 0, 0));
}

void OverlayMarkupTest::redactionPrintLeakGuards()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString printFile = dir.filePath(QStringLiteral("full_print.png"));
    QImage dummy(32, 32, QImage::Format_RGB32);
    dummy.fill(Qt::green);
    QVERIFY(dummy.save(printFile, "PNG"));
    QGuiApplication::clipboard()->setImage(dummy);

    AnnotationDocument emptyDoc;
    QVERIFY(shouldBypassRawPrintFastPath(true, printFile, emptyDoc));
    QVERIFY(!shouldDeletePrintFileOnCopy(emptyDoc, printFile));

    AnnotationDocument calloutDoc;
    AnnotationItem rect;
    rect.kind = ItemKind::Rectangle;
    rect.rect = QRectF(5, 5, 20, 20);
    calloutDoc.addItem(rect);
    QVERIFY(!shouldBypassRawPrintFastPath(true, printFile, calloutDoc));
    QVERIFY(!shouldDeletePrintFileOnCopy(calloutDoc, printFile));

    AnnotationDocument redactDoc;
    AnnotationItem redact;
    redact.kind = ItemKind::Redact;
    redact.rect = QRectF(5, 5, 20, 20);
    redactDoc.addItem(redact);
    QVERIFY(!shouldBypassRawPrintFastPath(true, printFile, redactDoc));
    QVERIFY(shouldDeletePrintFileOnCopy(redactDoc, printFile));

    QVERIFY(purgeUnredactedPrintFile(printFile, true));
    QVERIFY(!QFile::exists(printFile));
    QVERIFY(QGuiApplication::clipboard()->image().isNull());
}

void OverlayMarkupTest::rtlCaptureGeometryAndQuickEditorLayout()
{
    struct DirectionGuard {
        Qt::LayoutDirection previous = QGuiApplication::layoutDirection();
        ~DirectionGuard() { QGuiApplication::setLayoutDirection(previous); }
    } restoreDirection;

    QImage bg(600, 400, QImage::Format_RGB32);
    bg.fill(Qt::white);
    QGuiApplication::setLayoutDirection(Qt::RightToLeft);

    AreaOverlay overlay(QStringLiteral("DP-1"), QSize(600, 400), bg, QRect());
    overlay.resize(600, 400);
    QCOMPARE(overlay.layoutDirection(), Qt::RightToLeft);

    QMouseEvent press(QEvent::MouseButtonPress, QPointF(100, 100), QPointF(100, 100), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, QPointF(250, 200), QPointF(250, 200), Qt::NoButton, Qt::LeftButton,
                     Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(250, 200), QPointF(250, 200), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(&overlay, &press);
    QApplication::sendEvent(&overlay, &move);
    QApplication::sendEvent(&overlay, &release);
    QCOMPARE(overlay.selection(), QRect(100, 100, 150, 100));

    // Arrow navigation keeps physical screen coordinates under RTL.
    QKeyEvent shiftRight(QEvent::KeyPress, Qt::Key_Right, Qt::ShiftModifier);
    QApplication::sendEvent(&overlay, &shiftRight);
    QCOMPARE(overlay.selection(), QRect(110, 100, 150, 100));

    auto checkEditorToolbarOrder = [&](Qt::LayoutDirection direction, int *selectX, int *textX) {
        QGuiApplication::setLayoutDirection(direction);
        TargetStore store;
        QuickEditorWindow editor(store, bg, 1.0, std::make_unique<AnnotationDocument>(), QStringLiteral("rtl"));
        QVERIFY(editor.isValid());
        editor.resize(700, 500);
        editor.show();
        QVERIFY(QTest::qWaitForWindowExposed(&editor));
        QApplication::processEvents();
        QCOMPARE(editor.layoutDirection(), direction);

        AnnotationToolbar *toolbar = editor.toolbar();
        QVERIFY(toolbar != nullptr);
        QToolButton *selectButton = toolbar->toolButton(ToolKind::Select);
        QToolButton *textButton = toolbar->toolButton(ToolKind::Text);
        QVERIFY(selectButton != nullptr);
        QVERIFY(textButton != nullptr);
        *selectX = selectButton->mapTo(toolbar, QPoint(0, 0)).x();
        *textX = textButton->mapTo(toolbar, QPoint(0, 0)).x();
        QVERIFY(!editor.grab().toImage().isNull());

        editor.setCloseDecisionOverride(QuickEditorWindow::CloseDecision::Discard);
        editor.close();
    };

    int ltrSelectX = 0, ltrTextX = 0;
    int rtlSelectX = 0, rtlTextX = 0;
    checkEditorToolbarOrder(Qt::LeftToRight, &ltrSelectX, &ltrTextX);
    checkEditorToolbarOrder(Qt::RightToLeft, &rtlSelectX, &rtlTextX);
    QVERIFY(ltrSelectX < ltrTextX);
    QVERIFY(rtlSelectX > rtlTextX);
}

int main(int argc, char *argv[])
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    OverlayMarkupTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "overlaymarkuptest.moc"
