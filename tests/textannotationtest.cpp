#include "annotations/annotationcanvas.h"
#include "annotations/annotationcontroller.h"
#include "annotations/annotationdocument.h"
#include "annotations/annotationitem.h"
#include "annotations/annotationrenderer.h"
#include "annotations/annotationsettings.h"
#include "annotations/annotationtextlayout.h"
#include "annotations/annotationtoolbar.h"
#include "annotations/inlinetexteditor.h"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QtTest>

using namespace glimply;

class TextAnnotationTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void pointTextAndEscCommit();
    void paragraphTextWrapAndExteriorMove();
    void doubleClickReEditCancelAndInputIsolation();
    void rtlZwnjAndLegibilityModesCrisp();
    void toolbarTextPropertiesAndDefaults();
};

void TextAnnotationTest::pointTextAndEscCommit()
{
    AnnotationCanvas canvas;
    canvas.resize(500, 350);
    QImage bg(500, 350, QImage::Format_RGB32);
    bg.fill(Qt::white);
    canvas.setImage(bg, 1.0);

    AnnotationController *ctrl = canvas.controller();
    AnnotationDocument *doc = canvas.document();
    ctrl->setTool(ToolKind::Text);

    // Plain click -> Point Text inline edit.
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(40, 50), QPointF(40, 50), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(41, 50), QPointF(41, 50), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(&canvas, &press);
    QApplication::sendEvent(&canvas, &release);
    QVERIFY(ctrl->isEditingText());
    QVERIFY(!canvas.textEditor()->isHidden());

    canvas.textEditor()->textEdit()->setPlainText(QStringLiteral("Line one\nLine two"));
    // Escape commits text (PhotoCraft / Photoshop useEscToCommit) and switches to Select.
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(canvas.textEditor()->textEdit(), &esc);
    QVERIFY(!ctrl->isEditingText());
    QCOMPARE(ctrl->tool(), ToolKind::Select);
    QCOMPARE(doc->count(), 1);
    const AnnotationItem *item = doc->selectedItem();
    QVERIFY(item != nullptr);
    QCOMPARE(item->text, QStringLiteral("Line one\nLine two"));
    QVERIFY(!item->paragraphMode);
    QCOMPARE(item->legibility, TextLegibility::None);

    // Empty Point Text is discarded cleanly on Escape.
    ctrl->setTool(ToolKind::Text);
    QMouseEvent press2(QEvent::MouseButtonPress, QPointF(200, 50), QPointF(200, 50), Qt::LeftButton, Qt::LeftButton,
                       Qt::NoModifier);
    QMouseEvent rel2(QEvent::MouseButtonRelease, QPointF(200, 50), QPointF(200, 50), Qt::LeftButton, Qt::NoButton,
                     Qt::NoModifier);
    QApplication::sendEvent(&canvas, &press2);
    QApplication::sendEvent(&canvas, &rel2);
    QVERIFY(ctrl->isEditingText());
    canvas.textEditor()->textEdit()->setPlainText(QStringLiteral("   "));
    QKeyEvent ctrlEnter(QEvent::KeyPress, Qt::Key_Return, Qt::ControlModifier);
    QApplication::sendEvent(canvas.textEditor()->textEdit(), &ctrlEnter);
    QVERIFY(!ctrl->isEditingText());
    QCOMPARE(doc->count(), 1);
}

void TextAnnotationTest::paragraphTextWrapAndExteriorMove()
{
    AnnotationCanvas canvas;
    canvas.resize(500, 350);
    QImage bg(500, 350, QImage::Format_RGB32);
    bg.fill(Qt::white);
    canvas.setImage(bg, 1.0);

    AnnotationController *ctrl = canvas.controller();
    AnnotationDocument *doc = canvas.document();
    ctrl->setTool(ToolKind::Text);

    // Drag a 120 px Paragraph Text box.
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(50, 60), QPointF(50, 60), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, QPointF(170, 120), QPointF(170, 120), Qt::NoButton, Qt::LeftButton,
                     Qt::NoModifier);
    QMouseEvent rel(QEvent::MouseButtonRelease, QPointF(170, 120), QPointF(170, 120), Qt::LeftButton, Qt::NoButton,
                    Qt::NoModifier);
    QApplication::sendEvent(&canvas, &press);
    QApplication::sendEvent(&canvas, &move);
    QApplication::sendEvent(&canvas, &rel);
    QVERIFY(ctrl->isEditingText());

    InlineTextEditor *editor = canvas.textEditor();
    editor->textEdit()->setPlainText(
        QStringLiteral("Long paragraph text that should wrap automatically within the box"));

    // Dragging inside the exterior move margin (e.g. (3, 3) on editor widget) moves without committing.
    const QPointF startTopLeft = editor->currentItem().rect.topLeft();
    QMouseEvent mp(QEvent::MouseButtonPress, QPointF(3, 3), QPointF(103, 103), Qt::LeftButton, Qt::LeftButton,
                   Qt::NoModifier);
    QMouseEvent mm(QEvent::MouseMove, QPointF(23, 33), QPointF(123, 133), Qt::NoButton, Qt::LeftButton,
                   Qt::NoModifier);
    QMouseEvent mr(QEvent::MouseButtonRelease, QPointF(23, 33), QPointF(123, 133), Qt::LeftButton, Qt::NoButton,
                   Qt::NoModifier);
    QApplication::sendEvent(editor, &mp);
    QApplication::sendEvent(editor, &mm);
    QApplication::sendEvent(editor, &mr);
    QVERIFY(ctrl->isEditingText());
    QCOMPARE(editor->currentItem().rect.topLeft(), startTopLeft + QPointF(20.0, 30.0));

    // Clicking on canvas outside the editor commits.
    QMouseEvent clickOut(QEvent::MouseButtonPress, QPointF(400, 300), QPointF(400, 300), Qt::LeftButton,
                         Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &clickOut);
    QVERIFY(!ctrl->isEditingText());
    QCOMPARE(doc->count(), 1);
    const AnnotationItem &para = doc->items().constFirst();
    QVERIFY(para.paragraphMode);
    QCOMPARE(para.rect.topLeft(), startTopLeft + QPointF(20.0, 30.0));
    QCOMPARE(para.rect.width(), 120.0);
    QVERIFY(para.handles().size() == 2); // Left + Right wrap handles.
}

void TextAnnotationTest::doubleClickReEditCancelAndInputIsolation()
{
    AnnotationCanvas canvas;
    canvas.resize(400, 300);
    QImage bg(400, 300, QImage::Format_RGB32);
    bg.fill(Qt::white);
    canvas.setImage(bg, 1.0);

    AnnotationController *ctrl = canvas.controller();
    AnnotationDocument *doc = canvas.document();

    AnnotationItem initial = ctrl->buildTextItemTemplate(QPointF(60, 80));
    initial.text = QStringLiteral("Original");
    initial.rect.setSize(measureAnnotationText(initial));
    const int id = doc->addItem(initial);

    // Double-click in Select mode opens inline re-edit.
    ctrl->setTool(ToolKind::Select);
    const QPointF hitPos = doc->findItem(id)->rect.center();
    QMouseEvent dbl(QEvent::MouseButtonDblClick, hitPos, hitPos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &dbl);
    QVERIFY(ctrl->isEditingText());

    // Keys like Space, 'v', Delete while editing must not invoke controller shortcuts or delete items.
    QKeyEvent spaceEv(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
    QKeyEvent vEv(QEvent::KeyPress, Qt::Key_V, Qt::NoModifier, QStringLiteral("v"));
    QKeyEvent delEv(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(canvas.textEditor()->textEdit(), &spaceEv);
    QApplication::sendEvent(canvas.textEditor()->textEdit(), &vEv);
    QApplication::sendEvent(&canvas, &delEv); // Controller swallows keys while editingText_ is true.
    QCOMPARE(doc->count(), 1);
    QVERIFY(ctrl->isEditingText());

    // Explicit Cancel reverts without modifying the stored text.
    ctrl->requestCancelActiveText();
    QVERIFY(!ctrl->isEditingText());
    QCOMPARE(doc->findItem(id)->text, QStringLiteral("Original"));
}

void TextAnnotationTest::rtlZwnjAndLegibilityModesCrisp()
{
    AnnotationDocument doc;
    // Arabic/Persian text containing U+200C Zero-Width Non-Joiner and RTL script.
    const QString rtlText = QString::fromUtf8("می\u200cخواهم — OK");

    int x = 20;
    for (const TextLegibility leg :
         {TextLegibility::None, TextLegibility::Shadow, TextLegibility::Outline, TextLegibility::Pill}) {
        AnnotationItem t;
        t.kind = ItemKind::Text;
        t.text = rtlText;
        t.color = Qt::white;
        t.font.setPointSize(16);
        t.legibility = leg;
        t.rect = QRectF(x, 20, 120, 40);
        t.rect.setSize(measureAnnotationText(t));
        doc.addItem(t);
        x += 180;
    }

    // Light gray background where white text with Pill / Outline / Shadow stays visibly distinct.
    QImage base1x(780, 100, QImage::Format_RGB32);
    base1x.fill(QColor(215, 215, 215));
    const QImage out1x = renderAnnotatedCrop(base1x, QRectF(0, 0, 780, 100), 1.0, doc);

    QImage base2x(1560, 200, QImage::Format_RGB32);
    base2x.fill(QColor(215, 215, 215));
    const QImage out2x = renderAnnotatedCrop(base2x, QRectF(0, 0, 780, 100), 2.0, doc);

    // Verify Pill mode painted a dark backing pixel inside the 4th item's rect.
    const QRectF pillRect = doc.items().at(3).rect;
    const QColor pillCorner = out1x.pixelColor(int(pillRect.left()) - 3, int(pillRect.top()));
    QVERIFY(pillCorner.red() < 60 && pillCorner.green() < 60 && pillCorner.blue() < 60);

    // Verify Shadow and Outline painted dark pixels around the 2nd & 3rd white-text items.
    const auto hasDarkInk = [](const QImage &img, const QRectF &box) {
        for (int py = int(box.top()) - 3; py <= int(box.bottom()) + 4; ++py) {
            for (int px = int(box.left()) - 3; px <= int(box.right()) + 4; ++px) {
                if (px >= 0 && px < img.width() && py >= 0 && py < img.height()) {
                    if (img.pixelColor(px, py).red() < 120) {
                        return true;
                    }
                }
            }
        }
        return false;
    };
    QVERIFY(hasDarkInk(out1x, doc.items().at(1).rect));
    QVERIFY(hasDarkInk(out1x, doc.items().at(2).rect));
    QVERIFY(hasDarkInk(out2x, QRectF(doc.items().at(2).rect.topLeft() * 2.0,
                                     doc.items().at(2).rect.size() * 2.0)));
    QCOMPARE(out2x.size(), QSize(1560, 200));
}

void TextAnnotationTest::toolbarTextPropertiesAndDefaults()
{
    AnnotationDocument doc;
    AnnotationController ctrl(&doc);
    AnnotationSettings settings;
    AnnotationToolbar toolbar(&ctrl);
    toolbar.setSettings(&settings);

    ctrl.setTool(ToolKind::Text);
    QVERIFY(toolbar.isPropertyBarVisible());
    QVERIFY(toolbar.isTextGroupVisible());
    QVERIFY(!toolbar.isTextActionBoxVisible());
    QVERIFY(toolbar.legibilityButton(TextLegibility::None)->isChecked());

    // Switch legibility to Pill while Text tool is active -> updates AnnotationSettings.
    toolbar.legibilityButton(TextLegibility::Pill)->click();
    QCOMPARE(ctrl.textLegibility(), TextLegibility::Pill);
    QCOMPARE(settings.textLegibility, TextLegibility::Pill);

    // Begin inline text edit -> Apply / Cancel buttons appear.
    const AnnotationItem tpl = ctrl.buildTextItemTemplate(QPointF(20, 20));
    ctrl.beginTextEdit(tpl, false);
    QVERIFY(toolbar.isTextActionBoxVisible());
    ctrl.cancelTextEdit();
    QVERIFY(!toolbar.isTextActionBoxVisible());
}

int main(int argc, char *argv[])
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    TextAnnotationTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "textannotationtest.moc"
