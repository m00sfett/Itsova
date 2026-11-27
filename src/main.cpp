#include "app.h"
#include "cli.h"
#include "i18n.h"
#include "mcpserver.h"
#include "logging.h"
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
#include <QGuiApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QTimer>
#include <QTranslator>
#include <QUrl>

#include <cstdio>
#include <unistd.h>
#include <vector>

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
        "  overlay            Open the interactive capture overlay\n"
        "  replay             Play a video fullscreen (all-monitor recordings on all monitors)\n"
        "  mcp                Run the local stdio MCP server (disabled by default)"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption(editOption);
    parser.addOption(releaseOption);
    parser.addOption(QCommandLineOption(QStringLiteral("verbose"),
                                        QApplication::translate("main", "Write detailed logs to the system journal.")));
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

// Returns every text untranslated. Used where output must stay English whatever the UI language
// (MCP, CLI JSON), so that no translation catalog loaded by a library leaks in.
class SourceTextTranslator : public QTranslator
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

// Applies the UI language (#51). KDE Frameworks load their own Qt catalogs from QLocale's UI
// languages in startup hooks that run inside the application constructor, and QLocale reads
// LANGUAGE once. So LANGUAGE is set before the application object exists and restored once it
// does, so that programs itsova starts (hooks, viewers, tesseract) keep the user's environment.
class UiLanguageScope
{
public:
    explicit UiLanguageScope(const QString &language)
        : language_(language)
        , hadPrevious_(qEnvironmentVariableIsSet("LANGUAGE"))
        , previous_(qgetenv("LANGUAGE"))
    {
        qputenv("LANGUAGE", language_.toLatin1());
    }

    ~UiLanguageScope()
    {
        restoreEnvironment();
    }

    UiLanguageScope(const UiLanguageScope &) = delete;
    UiLanguageScope &operator=(const UiLanguageScope &) = delete;

    // Call once the application object exists.
    void install(QCoreApplication &app) const
    {
        const bool english = language_ == QLatin1String(itsova::sourceUiLanguage);
        KLocalizedString::setLanguages({english ? QStringLiteral("en_US") : language_});
        // English loads its catalog too: it carries the plural forms ("%n capture(s)").
        auto *own = new QTranslator(&app);
        const QString base = QStringLiteral("itsova_") + language_;
        for (const QString &dir : itsova::translationDirectories()) {
            if (own->load(base, dir)) {
                QCoreApplication::installTranslator(own);
                break;
            }
        }
        if (english) {
            return;
        }
        // Qt's own dialog and widget texts; the locale lookup also finds "pt_BR" for "pt".
        auto *qt = new QTranslator(&app);
        if (qt->load(QLocale(language_), QStringLiteral("qtbase"), QStringLiteral("_"),
                     QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
            QCoreApplication::installTranslator(qt);
        }
    }

    void restoreEnvironment()
    {
        if (restored_) {
            return;
        }
        restored_ = true;
        if (hadPrevious_) {
            qputenv("LANGUAGE", previous_);
        } else {
            qunsetenv("LANGUAGE");
        }
    }

    [[nodiscard]] const QString &language() const
    {
        return language_;
    }

private:
    QString language_;
    bool hadPrevious_ = false;
    QByteArray previous_;
    bool restored_ = false;
};

QString effectiveUiLanguage()
{
    return itsova::resolveUiLanguage(itsova::configuredUiLanguage(), itsova::systemUiLanguages());
}

} // namespace

int main(int argc, char *argv[])
{
    // `itsova logs [journalctl options]` (#92): itsova's lines from the user journal.
    if (argc >= 2 && qstrcmp(argv[1], "logs") == 0) {
        QStringList extra;
        for (int i = 2; i < argc; ++i) {
            extra.append(QString::fromLocal8Bit(argv[i]));
        }
        if (extra.contains(QStringLiteral("-h")) || extra.contains(QStringLiteral("--help"))) {
            std::fputs("Usage: itsova logs [journalctl options]\n"
                       "Shows itsova's lines from the user journal (last hour without options).\n"
                       "Examples: itsova logs -f, itsova logs -n 200, itsova logs --since today\n",
                       stdout);
            return 0;
        }
        const QStringList args = itsova::journalctlArguments(extra);
        std::vector<QByteArray> storage{QByteArrayLiteral("journalctl")};
        for (const QString &arg : args) {
            storage.push_back(arg.toLocal8Bit());
        }
        std::vector<char *> argvOut;
        for (QByteArray &arg : storage) {
            argvOut.push_back(arg.data());
        }
        argvOut.push_back(nullptr);
        execvp("journalctl", argvOut.data());
        std::fputs("itsova: journalctl not found.\n", stderr);
        return 1;
    }

    if (argc >= 2 && itsova::isCliSubcommand(QString::fromLocal8Bit(argv[1]))) {
        qInstallMessageHandler(routeQtLogsToStderr);
        // JSON output stays English whatever the UI language (#51); human-readable text follows it.
        bool jsonOutput = false;
        for (int i = 2; i < argc; ++i) {
            if (qstrcmp(argv[i], "--json") == 0) {
                jsonOutput = true;
            }
        }
        UiLanguageScope language(jsonOutput ? QString::fromLatin1(itsova::sourceUiLanguage) : effectiveUiLanguage());
        QCoreApplication cliApp(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("itsova"));
        QCoreApplication::setApplicationVersion(QStringLiteral(ITSOVA_VERSION));
        if (jsonOutput) {
            QCoreApplication::installTranslator(new SourceTextTranslator(&cliApp));
        } else {
            language.install(cliApp);
        }
        language.restoreEnvironment();
        QFile outFile;
        QFile errFile;
        if (!outFile.open(stdout, QIODevice::WriteOnly) || !errFile.open(stderr, QIODevice::WriteOnly)) {
            std::fputs("itsova: failed to open standard streams.\n", stderr);
            return 1;
        }
        itsova::DBusCliBackend backend;
        const int code = itsova::runCli(QCoreApplication::arguments(), backend, outFile, errFile);
        outFile.flush();
        errFile.flush();
        return code;
    }

    if (argc >= 2 && QString::fromLocal8Bit(argv[1]).compare(QLatin1String("mcp"), Qt::CaseInsensitive) == 0) {
        qInstallMessageHandler(routeQtLogsToStderr);
        // MCP texts stay English (#51).
        UiLanguageScope language(QString::fromLatin1(itsova::sourceUiLanguage));
        QCoreApplication mcpApp(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("itsova"));
        QCoreApplication::setApplicationVersion(QStringLiteral(ITSOVA_VERSION));
        QCoreApplication::setOrganizationDomain(QStringLiteral("m00sfett.github.io"));
        QCoreApplication::installTranslator(new SourceTextTranslator(&mcpApp));
        language.restoreEnvironment();
        const QStringList args = QCoreApplication::arguments();
        if (args.contains(QStringLiteral("--help")) || args.contains(QStringLiteral("-h"))) {
            std::fputs("Usage: itsova mcp\n"
                       "Runs the local stdio JSON-RPC 2.0 MCP server (disabled by default; enable in\n"
                       "Itsova Settings -> Automation -> Local MCP Server).\n",
                       stderr);
            return 0;
        }
        itsova::DBusMcpBackend backend;
        return itsova::McpServer::runStdio(backend);
    }

    UiLanguageScope language(effectiveUiLanguage());
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("itsova"));
    QApplication::setApplicationDisplayName(QStringLiteral("Itsova"));
    QApplication::setApplicationVersion(QStringLiteral(ITSOVA_VERSION));
    QApplication::setOrganizationDomain(QStringLiteral("m00sfett.github.io"));
    QApplication::setDesktopFileName(QStringLiteral("io.github.m00sfett.itsova"));
    language.install(application);
    language.restoreEnvironment();
    itsova::setActiveUiLanguage(language.language());
    // Qt derives the direction from the qtbase catalog; set it from the language as well, since
    // not every UI language has a Qt catalog.
    QGuiApplication::setLayoutDirection(QLocale(language.language()).textDirection());
    // A tray application keeps running when the settings window closes.
    QApplication::setQuitOnLastWindowClosed(false);
    // Nor when the last QEventLoopLocker goes (a finished notification): with the tray icon hidden
    // (#123) there may be no window and no item left, and itsova only quits when asked to.
    QCoreApplication::setQuitLockEnabled(false);

    // Used by the uninstaller: give the Print key back to its previous owner (usually Spectacle).
    if (QApplication::arguments().contains(QStringLiteral("--release-print-key"))) {
        KConfig config(QStringLiteral("itsovarc"), KConfig::SimpleConfig);
        itsova::PrintKey::release(config, true);
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

    itsova::App app;
    if (QApplication::arguments().contains(QStringLiteral("--verbose"))) {
        app.setVerboseFromCommandLine(true);
    }
    QObject::connect(&service, &KDBusService::activateRequested, &app,
                     [&app](const QStringList &arguments, const QString &workingDirectory) {
                         if (arguments.contains(QStringLiteral("--verbose"))) {
                             app.setVerboseFromCommandLine(true);
                         }
                         const QStringList paths = extractEditPaths(arguments, workingDirectory);
                         for (const QString &path : paths) {
                             app.openEditorForFile(path);
                         }
                         // A plain second start (launcher, `itsova`) opens the settings, the way
                         // back when the tray icon is hidden (#123).
                         if (arguments.size() <= 1) {
                             qCInfo(lcApp) << "second start without arguments: opening the settings";
                             app.showSettings();
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
