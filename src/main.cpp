#include "app.h"
#include "cli.h"
#include "printkey.h"

#include <KConfig>
#include <KConfigGroup>
#include <KDBusService>
#include <KLocalizedString>

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QTranslator>
#include <QUrl>

#include <cstdio>

namespace
{

void routeQtLogsToStderr(QtMsgType, const QMessageLogContext &, const QString &message)
{
    const QByteArray bytes = message.toUtf8();
    std::fwrite(bytes.constData(), 1, static_cast<std::size_t>(bytes.size()), stderr);
    std::fputc('\n', stderr);
}

void configureParser(QCommandLineParser &parser, const QCommandLineOption &editOption,
                     const QCommandLineOption &releaseOption)
{
    parser.setApplicationDescription(QApplication::translate(
        "main",
        "Lightweight KDE screenshot and annotation tool\n\n"
        "Scriptable CLI subcommands:\n"
        "  capture <target>   Capture an area, group, monitor, all monitors, or window\n"
        "  list               List configured targets, monitors, or open windows\n"
        "  area               Add, edit, rename, or remove saved areas\n"
        "  interval           Start, stop, or inspect interval captures\n"
        "  overlay            Open the interactive capture overlay"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption(editOption);
    parser.addOption(releaseOption);
    parser.addPositionalArgument(
        QStringLiteral("files"),
        QApplication::translate("main", "Image file(s) to open in the Quick Editor window."),
        QStringLiteral("[files...]"));
}

QStringList extractEditPaths(const QStringList &arguments, const QString &workingDirectory)
{
    QCommandLineOption editOption(
        {QStringLiteral("e"), QStringLiteral("edit")},
        QApplication::translate("main", "Open an image file in the Quick Editor window."),
        QStringLiteral("file"));
    QCommandLineOption releaseOption(
        QStringLiteral("release-print-key"),
        QApplication::translate("main", "Release the Print key shortcut and exit."));
    QCommandLineParser parser;
    configureParser(parser, editOption, releaseOption);
    if (!parser.parse(arguments)) {
        return {};
    }

    QStringList raw = parser.values(editOption);
    raw.append(parser.positionalArguments());

    const QDir baseDir(workingDirectory.isEmpty() ? QDir::currentPath() : workingDirectory);
    QStringList resolved;
    for (const QString &entry : std::as_const(raw)) {
        const QString trimmed = entry.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        const QUrl url = QUrl::fromUserInput(trimmed, baseDir.absolutePath(), QUrl::AssumeLocalFile);
        const QString local = url.isLocalFile() ? url.toLocalFile() : baseDir.absoluteFilePath(trimmed);
        const QString abs = QFileInfo(local).absoluteFilePath();
        if (!abs.isEmpty() && !resolved.contains(abs)) {
            resolved.append(abs);
        }
    }
    return resolved;
}

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
    if (argc >= 2 && glimply::isCliSubcommand(QString::fromLocal8Bit(argv[1]))) {
        qInstallMessageHandler(routeQtLogsToStderr);
        QCoreApplication cliApp(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("glimply"));
        QCoreApplication::setApplicationVersion(QStringLiteral(GLIMPLY_VERSION));
        QFile outFile;
        QFile errFile;
        if (!outFile.open(stdout, QIODevice::WriteOnly) || !errFile.open(stderr, QIODevice::WriteOnly)) {
            std::fputs("glimply: failed to open standard streams.\n", stderr);
            return 1;
        }
        glimply::DBusCliBackend backend;
        const int code = glimply::runCli(QCoreApplication::arguments(), backend, outFile, errFile);
        outFile.flush();
        errFile.flush();
        return code;
    }

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

    {
        QCommandLineOption editOption(
            {QStringLiteral("e"), QStringLiteral("edit")},
            QApplication::translate("main", "Open an image file in the Quick Editor window."),
            QStringLiteral("file"));
        QCommandLineOption releaseOption(
            QStringLiteral("release-print-key"),
            QApplication::translate("main", "Release the Print key shortcut and exit."));
        QCommandLineParser parser;
        configureParser(parser, editOption, releaseOption);
        parser.process(application);
    }

    const QStringList initialPaths = extractEditPaths(QApplication::arguments(), QDir::currentPath());

    // One instance per session: a second start forwards its arguments and exits.
    KDBusService service(KDBusService::Unique);

    glimply::App app;
    QObject::connect(&service, &KDBusService::activateRequested, &app,
                     [&app](const QStringList &arguments, const QString &workingDirectory) {
                         const QStringList paths = extractEditPaths(arguments, workingDirectory);
                         for (const QString &path : paths) {
                             app.openEditorForFile(path);
                         }
                     });

    if (!initialPaths.isEmpty()) {
        QTimer::singleShot(0, &app, [&app, initialPaths] {
            for (const QString &path : initialPaths) {
                app.openEditorForFile(path);
            }
        });
    }

    return QApplication::exec();
}
