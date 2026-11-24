#include "annotations/annotationcommands.h"
#include "annotations/annotationcontroller.h"
#include "annotations/annotationitem.h"
#include "quickeditorwindow.h"
#include "targets.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>
#include <QWheelEvent>

#include <memory>

using namespace glimply;

namespace
{

QImage makeSolidImage(const QSize &size, const QColor &color)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(color);
    return img;
}

} // namespace

class QuickEditorTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void fileSaveSaveCopyAndCopyIsolation();
    void captureHandoffPreservesVectorDocument();
    void undoableCropAndCoordinateShift();
    void photocraftZoomAndPan();
    void dirtyCloseGuard();
};

void QuickEditorTest::fileSaveSaveCopyAndCopyIsolation()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    TargetStore store;
    store.captureDirectory = tempDir.path();

    const QString samplePath = QDir(tempDir.path()).filePath(QStringLiteral("sample.png"));
    const QColor bgColor(40, 120, 200);
    QVERIFY(makeSolidImage(QSize(120, 90), bgColor).save(samplePath, "PNG"));

    const QString sentinel = QStringLiteral("keep-sentinel");
    QGuiApplication::clipboard()->setText(sentinel);
    QCOMPARE(QGuiApplication::clipboard()->text(), sentinel);

    QuickEditorWindow window(store, samplePath);
    QVERIFY2(window.isValid(), qPrintable(window.loadError()));
    QVERIFY(!window.isDirty());
    QCOMPARE(window.filePath(), QFileInfo(samplePath).absoluteFilePath());
    QCOMPARE(window.canvas()->image().size(), QSize(120, 90));

    AnnotationItem redaction;
    redaction.kind = ItemKind::Redact;
    redaction.redactionMode = RedactionMode::Solid;
    redaction.color = QColor(0, 0, 0);
    redaction.rect = QRectF(10.0, 10.0, 40.0, 40.0);
    window.canvas()->document()->undoStack().push(new AddItemCommand(window.canvas()->document(), redaction));

    QVERIFY(window.isDirty());

    // Save Copy must create _edited.png without overwriting original or touching clipboard.
    QVERIFY(window.saveCopy());
    const QString edited1 = QDir(tempDir.path()).filePath(QStringLiteral("sample_edited.png"));
    QVERIFY(QFileInfo::exists(edited1));
    QCOMPARE(QImage(edited1).pixelColor(25, 25), QColor(0, 0, 0));
    QCOMPARE(QImage(samplePath).pixelColor(25, 25), bgColor);
    QCOMPARE(window.filePath(), QFileInfo(samplePath).absoluteFilePath());
    QCOMPARE(QGuiApplication::clipboard()->text(), sentinel);
    QVERIFY(window.isDirty());

    // Second Save Copy must create sample_edited-2.png cleanly.
    QVERIFY(window.saveCopy());
    const QString edited2 = QDir(tempDir.path()).filePath(QStringLiteral("sample_edited-2.png"));
    QVERIFY(QFileInfo::exists(edited2));

    // Save must atomically update sample.png, clear dirty state, and still leave clipboard untouched.
    QVERIFY(window.save());
    QVERIFY(!window.isDirty());
    QCOMPARE(QImage(samplePath).pixelColor(25, 25), QColor(0, 0, 0));
    QCOMPARE(QGuiApplication::clipboard()->text(), sentinel);

    // Ctrl+C outside text editing copies the flattened export image to clipboard.
    QKeyEvent copyKey(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(&window, &copyKey);
    const QImage copied = QGuiApplication::clipboard()->image();
    QVERIFY(!copied.isNull());
    QCOMPARE(copied.size(), QSize(120, 90));
    QCOMPARE(copied.pixelColor(25, 25), QColor(0, 0, 0));
}

void QuickEditorTest::captureHandoffPreservesVectorDocument()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    TargetStore store;
    store.captureDirectory = tempDir.path();
    store.filenameTemplate = QStringLiteral("{date}_{time}_{area}.png");

    auto doc = std::make_unique<AnnotationDocument>();
    AnnotationItem rectItem;
    rectItem.kind = ItemKind::Rectangle;
    rectItem.rect = QRectF(12.0, 14.0, 30.0, 20.0);
    rectItem.color = QColor(220, 40, 40);
    doc->replaceItemsRaw({rectItem});

    const QImage rawCapture = makeSolidImage(QSize(160, 120), QColor(240, 240, 240));
    QuickEditorWindow window(store, rawCapture, 2.0, std::move(doc), QStringLiteral("selection"));

    QVERIFY2(window.isValid(), qPrintable(window.loadError()));
    QVERIFY(window.isDirty());
    QVERIFY(window.filePath().isEmpty());
    QCOMPARE(window.canvas()->logicalImageSize(), QSizeF(80.0, 60.0));
    QCOMPARE(window.canvas()->document()->count(), 1);
    QCOMPARE(window.canvas()->document()->items().first().rect, QRectF(12.0, 14.0, 30.0, 20.0));

    QSignalSpy savedSpy(&window, &QuickEditorWindow::savedToFile);
    QVERIFY(window.save());
    QCOMPARE(savedSpy.count(), 1);
    QVERIFY(!window.filePath().isEmpty());
    QVERIFY(QFileInfo::exists(window.filePath()));
    QCOMPARE(QFileInfo(window.filePath()).absolutePath(), QFileInfo(tempDir.path()).absoluteFilePath());
    QVERIFY(!window.isDirty());
}

void QuickEditorTest::undoableCropAndCoordinateShift()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    TargetStore store;
    store.captureDirectory = tempDir.path();

    auto doc = std::make_unique<AnnotationDocument>();
    AnnotationItem box;
    box.kind = ItemKind::Rectangle;
    box.rect = QRectF(30.0, 30.0, 20.0, 20.0);
    doc->replaceItemsRaw({box});

    QuickEditorWindow window(store, makeSolidImage(QSize(120, 100), Qt::white), 1.0, std::move(doc),
                             QStringLiteral("crop"));
    QVERIFY(window.isValid());
    window.resize(700, 500);
    window.show();
    QTest::qWait(20);

    window.canvas()->controller()->setTool(ToolKind::Crop);
    QVERIFY(window.isCropStripVisible());

    window.canvas()->setCropRect(QRect(20, 10, 60, 50));
    QVERIFY(window.canvas()->hasCropRect());
    QVERIFY(window.canvas()->applyActiveCrop());

    QCOMPARE(window.canvas()->image().size(), QSize(60, 50));
    QCOMPARE(window.canvas()->document()->count(), 1);
    QCOMPARE(window.canvas()->document()->items().first().rect, QRectF(10.0, 20.0, 20.0, 20.0));

    window.canvas()->document()->undoStack().undo();
    QCOMPARE(window.canvas()->image().size(), QSize(120, 100));
    QCOMPARE(window.canvas()->document()->items().first().rect, QRectF(30.0, 30.0, 20.0, 20.0));

    window.canvas()->document()->undoStack().redo();
    QCOMPARE(window.canvas()->image().size(), QSize(60, 50));
    QCOMPARE(window.canvas()->document()->items().first().rect, QRectF(10.0, 20.0, 20.0, 20.0));
}

void QuickEditorTest::photocraftZoomAndPan()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    TargetStore store;
    store.captureDirectory = tempDir.path();

    const QString path = QDir(tempDir.path()).filePath(QStringLiteral("large.png"));
    QVERIFY(makeSolidImage(QSize(1200, 900), Qt::lightGray).save(path, "PNG"));

    QuickEditorWindow window(store, path);
    QVERIFY(window.isValid());
    window.resize(600, 400);
    window.show();
    QTest::qWait(20);

    QVERIFY(window.canvas()->isAutoFit());

    window.canvas()->resetZoom100();
    QVERIFY(!window.canvas()->isAutoFit());
    QVERIFY(qFuzzyCompare(window.canvas()->zoom(), 1.0));

    const QPointF anchor(window.canvas()->width() * 0.5, window.canvas()->height() * 0.5);
    const QPointF globalAnchor = window.canvas()->mapToGlobal(anchor.toPoint());
    QWheelEvent zoomWheel(anchor, globalAnchor, QPoint(), QPoint(0, 120), Qt::NoButton, Qt::ControlModifier,
                          Qt::NoScrollPhase, false);
    QApplication::sendEvent(window.canvas(), &zoomWheel);
    QVERIFY(window.canvas()->zoom() > 1.0);

    const qreal zoomBeforeScroll = window.canvas()->zoom();
    const QPointF panBeforeScroll = window.canvas()->panOffset();
    QWheelEvent panWheel(anchor, globalAnchor, QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                         Qt::NoScrollPhase, false);
    QApplication::sendEvent(window.canvas(), &panWheel);
    QVERIFY(qFuzzyCompare(window.canvas()->zoom(), zoomBeforeScroll));
    QVERIFY(!qFuzzyCompare(window.canvas()->panOffset().y(), panBeforeScroll.y()));

    // Space + Left-Drag pans without creating any annotation item.
    const QPointF panBeforeDrag = window.canvas()->panOffset();
    QKeyEvent spacePress(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(window.canvas(), &spacePress);
    const QPointF p0(120.0, 120.0);
    const QPointF p1(155.0, 145.0);
    QMouseEvent press(QEvent::MouseButtonPress, p0, window.canvas()->mapToGlobal(p0.toPoint()), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, p1, window.canvas()->mapToGlobal(p1.toPoint()), Qt::NoButton, Qt::LeftButton,
                     Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, p1, window.canvas()->mapToGlobal(p1.toPoint()), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(window.canvas(), &press);
    QApplication::sendEvent(window.canvas(), &move);
    QApplication::sendEvent(window.canvas(), &release);
    QKeyEvent spaceRelease(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(window.canvas(), &spaceRelease);

    QVERIFY(qFuzzyCompare(window.canvas()->panOffset().x(), panBeforeDrag.x() + 35.0));
    QVERIFY(qFuzzyCompare(window.canvas()->panOffset().y(), panBeforeDrag.y() + 25.0));
    QCOMPARE(window.canvas()->document()->count(), 0);

    window.canvas()->fitInView();
    QVERIFY(window.canvas()->isAutoFit());
}

void QuickEditorTest::dirtyCloseGuard()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    TargetStore store;
    store.captureDirectory = tempDir.path();

    const QString path = QDir(tempDir.path()).filePath(QStringLiteral("guard.png"));
    QVERIFY(makeSolidImage(QSize(80, 60), Qt::white).save(path, "PNG"));

    QuickEditorWindow window(store, path);
    QVERIFY(window.isValid());

    AnnotationItem rectItem;
    rectItem.kind = ItemKind::Rectangle;
    rectItem.rect = QRectF(5.0, 5.0, 30.0, 20.0);
    window.canvas()->document()->undoStack().push(new AddItemCommand(window.canvas()->document(), rectItem));
    QVERIFY(window.isDirty());

    window.setCloseDecisionOverride(QuickEditorWindow::CloseDecision::Cancel);
    QVERIFY(!window.close());
    QVERIFY(window.isDirty());

    window.setCloseDecisionOverride(QuickEditorWindow::CloseDecision::Discard);
    QVERIFY(window.close());
}

QTEST_MAIN(QuickEditorTest)
#include "quickeditortest.moc"
