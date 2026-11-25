// Opt-in check for #73: does every translated panel button title fit its 64 px button?
// Reads "lang<TAB>title" lines from stdin (workspace helper: i18n73.py titles) and lays each title
// out exactly like PanelButton (fitPanelTitle: up to 3 lines, 11 px down to 8 px, then elided).
// Prints the titles that only fit when shrunk and those that are elided; exit code 1 if any title
// is elided. Never run by CTest.
#include "overlaypanel.h"

#include <QApplication>
#include <QTextStream>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTextStream in(stdin);
    QTextStream out(stdout);
    int total = 0;
    int shrunk = 0;
    int elided = 0;
    while (!in.atEnd()) {
        const QString line = in.readLine();
        const qsizetype tab = line.indexOf(QLatin1Char('\t'));
        if (tab <= 0) {
            continue;
        }
        const QString lang = line.left(tab);
        const QString title = line.mid(tab + 1);
        const glimply::PanelTitleLayout layout =
            glimply::fitPanelTitle(title, QFont(), glimply::PanelButton::side - 8);
        ++total;
        if (layout.elided) {
            ++elided;
            out << "ELIDED  " << lang << "  " << title << "  -> " << layout.lines.join(QLatin1String(" | ")) << '\n';
        } else if (layout.shrunk) {
            ++shrunk;
            out << "shrunk  " << lang << "  " << title << "  " << layout.pixelSize << " px, "
                << layout.lines.size() << " lines\n";
        }
    }
    out << total << " titles, " << shrunk << " shrunk, " << elided << " elided\n";
    return elided > 0 ? 1 : 0;
}
