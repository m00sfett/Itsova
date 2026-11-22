#include "app.h"
#include "printkey.h"

#include <KConfig>
#include <KConfigGroup>
#include <KDBusService>
#include <KLocalizedString>

#include <QApplication>
#include <QTranslator>

namespace
{

// glimply ships no translations yet. Without this, KDE and Qt widgets (shortcut fields, dialog
// buttons, search fields) would follow the system language while glimply's own text is English.
// Remove once glimply installs its translations.
class EnglishTranslator : public QTranslator
{
public:
    using QTranslator::QTranslator;

    QString translate(const char *, const char *sourceText, const char *, int) const override
    {
        return QString::fromUtf8(sourceText);
    }

    bool isEmpty() const override
    {
        return false;
    }
};

} // namespace

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("glimply"));
    QApplication::setApplicationDisplayName(QStringLiteral("glimply"));
    QApplication::setApplicationVersion(QStringLiteral(GLIMPLY_VERSION));
    QApplication::setOrganizationDomain(QStringLiteral("m00sfett.github.io"));
    QApplication::setDesktopFileName(QStringLiteral("io.github.m00sfett.glimply"));
    KLocalizedString::setLanguages({QStringLiteral("en_US")});
    QApplication::installTranslator(new EnglishTranslator(&application));
    // A tray application keeps running when the settings window closes.
    QApplication::setQuitOnLastWindowClosed(false);

    // Used by the uninstaller: give the Print key back to its previous owner (usually Spectacle).
    if (QApplication::arguments().contains(QStringLiteral("--release-print-key"))) {
        KConfig config(QStringLiteral("glimplyrc"), KConfig::SimpleConfig);
        glimply::PrintKey::release(config, true);
        config.group(QStringLiteral("General")).writeEntry("UsePrintKey", false);
        config.sync();
        return 0;
    }

    // One instance per session: a second start just exits.
    KDBusService service(KDBusService::Unique);

    glimply::App app;
    return QApplication::exec();
}
