#include "app.h"

#include <KDBusService>

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("glimply"));
    QApplication::setApplicationDisplayName(QStringLiteral("glimply"));
    QApplication::setApplicationVersion(QStringLiteral(GLIMPLY_VERSION));
    QApplication::setOrganizationDomain(QStringLiteral("m00sfett.github.io"));
    QApplication::setDesktopFileName(QStringLiteral("io.github.m00sfett.glimply"));
    // A tray application keeps running when the settings window closes.
    QApplication::setQuitOnLastWindowClosed(false);

    // One instance per session: a second start just exits.
    KDBusService service(KDBusService::Unique);

    glimply::App app;
    return QApplication::exec();
}
