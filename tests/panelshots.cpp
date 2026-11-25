// Opt-in visual check of the overlay panel (#69): renders the panel in its main states to PNG files.
// Never run by CTest. Usage: glimply-panel-shots <output-dir> [light|dark] [rtl]
// Run it with the desktop's platform theme (QT_QPA_PLATFORMTHEME=kde) to get Breeze and its icons.

#include "overlaypanel.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QLinearGradient>
#include <QPainter>
#include <QStyleFactory>
#include <QTextStream>

using namespace glimply;

namespace
{

QPalette breeze(bool dark)
{
    QPalette pal;
    const QColor window = dark ? QColor(0x20, 0x23, 0x26) : QColor(0xef, 0xf0, 0xf1);
    const QColor text = dark ? QColor(0xfc, 0xfc, 0xfc) : QColor(0x23, 0x26, 0x29);
    pal.setColor(QPalette::Window, window);
    pal.setColor(QPalette::WindowText, text);
    pal.setColor(QPalette::Base, dark ? QColor(0x14, 0x16, 0x18) : QColor(0xff, 0xff, 0xff));
    pal.setColor(QPalette::Text, text);
    pal.setColor(QPalette::Button, dark ? QColor(0x29, 0x2c, 0x30) : QColor(0xfc, 0xfc, 0xfc));
    pal.setColor(QPalette::ButtonText, text);
    pal.setColor(QPalette::Highlight, QColor(0x3d, 0xae, 0xe9));
    pal.setColor(QPalette::HighlightedText, QColor(0xfc, 0xfc, 0xfc));
    return pal;
}

// A stand-in for the frozen screen behind the panel.
QImage backdrop(const QSize &size)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    QPainter p(&image);
    QLinearGradient g(0, 0, size.width(), size.height());
    g.setColorAt(0, QColor(0x1d, 0x3d, 0x5c));
    g.setColorAt(0.6, QColor(0x6b, 0x5a, 0x7a));
    g.setColorAt(1, QColor(0xa8, 0x79, 0x6a));
    p.fillRect(image.rect(), g);
    p.fillRect(QRect(60, 90, 520, 300), QColor(0x1b, 0x1e, 0x20));
    p.fillRect(QRect(420, 150, 560, 340), QColor(0xf4, 0xf4, 0xf4));
    p.fillRect(image.rect(), QColor(0, 0, 0, 110)); // the frozen overlay dims the picture
    return image;
}

PanelButton *button(const QString &icon, const QString &title, const QString &shortcut = {})
{
    return new PanelButton(icon, title, shortcut, nullptr);
}

QWidget *row(const QList<PanelButton *> &buttons)
{
    auto *w = new QWidget;
    auto *l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    for (PanelButton *b : buttons) {
        b->setParent(w);
        l->addWidget(b);
    }
    l->addStretch();
    return w;
}

void fillStart(CapsulePanel &panel, bool hoverAll, const QString &lang)
{
    const bool de = lang == QLatin1String("de");
    QList<PanelButton *> buttons{
        button(QStringLiteral("monitor-symbolic"), de ? QStringLiteral("Dieser Bildschirm") : QStringLiteral("This Monitor")),
        button(QStringLiteral("view-fullscreen"), de ? QStringLiteral("Alle Bildschirme") : QStringLiteral("All Monitors")),
        button(QStringLiteral("window"), de ? QStringLiteral("Fenster") : QStringLiteral("Window")),
        button(QStringLiteral("select-rectangular"), de ? QStringLiteral("Bereiche") : QStringLiteral("Areas")),
        nullptr,
        button(QStringLiteral("chronometer"), de ? QStringLiteral("Verzögerung") : QStringLiteral("Delay")),
        button(QStringLiteral("draw-freehand"), de ? QStringLiteral("Anmerken") : QStringLiteral("Annotate"),
               QStringLiteral("V A R E B M T N X S")),
        nullptr,
        button(QStringLiteral("overflow-menu"), de ? QStringLiteral("Mehr") : QStringLiteral("More")),
        button(QStringLiteral("window-close"), de ? QStringLiteral("Schließen") : QStringLiteral("Close"),
               QStringLiteral("Esc")),
    };
    for (int i = 0; i < buttons.size(); ++i) {
        if (!buttons.at(i)) {
            panel.addGroupGap();
            continue;
        }
        if (i == 2 || i == 3 || i == 5 || i == 6 || i == 8) {
            buttons.at(i)->setOpensDrawer(true);
        }
        buttons.at(i)->setForcedHover(hoverAll || i == 0);
        panel.addMainButton(buttons.at(i));
    }
}

void fillSelection(CapsulePanel &panel, const QString &lang, PanelButton **markup)
{
    const bool de = lang == QLatin1String("de");
    auto *save = button(QStringLiteral("document-save"), de ? QStringLiteral("Speichern") : QStringLiteral("Save"),
                        QStringLiteral("Enter"));
    save->setPrimary(true);
    panel.addMainButton(save);
    panel.addMainButton(button(QStringLiteral("edit-copy"), de ? QStringLiteral("Kopieren") : QStringLiteral("Copy")));
    panel.addMainButton(button(QStringLiteral("window-pin"), de ? QStringLiteral("Anheften") : QStringLiteral("Pin")));
    panel.addMainButton(
        button(QStringLiteral("edit-select-text"), de ? QStringLiteral("Text kopieren") : QStringLiteral("Copy Text")));
    panel.addGroupGap();
    *markup = button(QStringLiteral("draw-freehand"), de ? QStringLiteral("Anmerken") : QStringLiteral("Annotate"));
    (*markup)->setOpensDrawer(true);
    panel.addMainButton(*markup);
    panel.addGroupGap();
    auto *more = button(QStringLiteral("overflow-menu"), de ? QStringLiteral("Mehr") : QStringLiteral("More"));
    more->setOpensDrawer(true);
    panel.addMainButton(more);
    panel.addMainButton(button(QStringLiteral("window-close"), de ? QStringLiteral("Schließen") : QStringLiteral("Close")));
}

void shoot(QWidget &panel, const QString &path)
{
    panel.adjustSize();
    // Let the 120 ms hover animations finish.
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 300) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    const QSize canvas(1100, std::max(260, panel.height() + 40));
    QImage image = backdrop(canvas);
    QPainter p(&image);
    const QPoint at((canvas.width() - panel.width()) / 2, 16);
    panel.render(&p, at, QRegion(), QWidget::DrawChildren);
    p.end();
    image.save(path);
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 2) {
        QTextStream(stderr) << "Usage: glimply-panel-shots <output-dir> [light|dark] [rtl] [de]\n";
        return 2;
    }
    const QString dir = args.at(1);
    QDir().mkpath(dir);
    const bool dark = !args.contains(QStringLiteral("light"));
    const bool rtl = args.contains(QStringLiteral("rtl"));
    const QString lang = args.contains(QStringLiteral("de")) ? QStringLiteral("de") : QStringLiteral("en");
    if (QStyle *style = QStyleFactory::create(QStringLiteral("Breeze"))) {
        QApplication::setStyle(style);
    }
    if (QIcon::themeName().isEmpty() || QIcon::themeName() == QLatin1String("hicolor")) {
        QIcon::setThemeName(dark ? QStringLiteral("breeze-dark") : QStringLiteral("breeze"));
    }
    QApplication::setPalette(breeze(dark));
    QApplication::setLayoutDirection(rtl ? Qt::RightToLeft : Qt::LeftToRight);
    const QString suffix = QStringLiteral("-%1%2-%3.png").arg(dark ? QStringLiteral("dark") : QStringLiteral("light"),
                                                              rtl ? QStringLiteral("-rtl") : QString(), lang);

    {
        CapsulePanel panel(nullptr);
        fillStart(panel, false, lang);
        shoot(panel, dir + QStringLiteral("/start") + suffix);
    }
    {
        CapsulePanel panel(nullptr);
        fillStart(panel, true, lang);
        shoot(panel, dir + QStringLiteral("/start-hover-all") + suffix);
    }
    {
        CapsulePanel panel(nullptr);
        PanelButton *markup = nullptr;
        fillSelection(panel, lang, &markup);
        shoot(panel, dir + QStringLiteral("/selection") + suffix);
    }
    {
        CapsulePanel panel(nullptr);
        fillStart(panel, false, lang);
        QList<PanelButton *> delay;
        for (const int s : {3, 5, 10}) {
            auto *b = button(QString(), QStringLiteral("%1 s").arg(s));
            b->setGlyphText(QStringLiteral("%1 s").arg(s));
            b->setActive(s == 5);
            delay.append(b);
        }
        delay.append(button(QStringLiteral("chronometer"), QStringLiteral("Custom…")));
        delay.append(button(QStringLiteral("view-refresh"), QStringLiteral("Keep delay active")));
        panel.setDrawer(row(delay));
        const auto children = panel.findChildren<PanelButton *>();
        for (PanelButton *b : children) {
            if (b->title() == QLatin1String("Delay") || b->title() == QStringLiteral("Verzögerung")) {
                b->setActive(true);
                b->setBadge(QStringLiteral("5 s"));
                b->setForcedHover(false);
            } else if (b->title() == QLatin1String("This Monitor") || b->title() == QLatin1String("Dieser Bildschirm")) {
                b->setForcedHover(false);
            }
        }
        shoot(panel, dir + QStringLiteral("/delay-drawer") + suffix);
    }
    {
        CapsulePanel panel(nullptr);
        PanelButton *markup = nullptr;
        fillSelection(panel, lang, &markup);
        for (PanelButton *b : panel.findChildren<PanelButton *>()) {
            b->setActive(b->title() == QLatin1String("More") || b->title() == QLatin1String("Mehr"));
        }
        QList<PanelButton *> more{
            button(QStringLiteral("document-save-as"), QStringLiteral("Save As…")),
            button(QStringLiteral("bookmark-new"), QStringLiteral("Save as Area")),
            button(QStringLiteral("list-add"), QStringLiteral("Add Rectangle")),
            button(QStringLiteral("document-edit"), QStringLiteral("Edit in Window"), QStringLiteral("Ctrl+E")),
            button(QStringLiteral("color-picker"), QStringLiteral("Pick Color"), QStringLiteral("I")),
        };
        more.at(2)->setForcedHover(true);
        panel.setDrawer(row(more));
        shoot(panel, dir + QStringLiteral("/selection-more-drawer") + suffix);
    }
    return 0;
}
