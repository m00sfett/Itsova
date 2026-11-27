#include "logging.h"
#include "notifyconfig.h"
#include "screenrecorder.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QTest>

using namespace itsova;

class LoggingTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void debugIsOffByDefaultInfoIsOn()
    {
        setVerboseLogging(false);
        QVERIFY(lcRecord().isInfoEnabled());
        QVERIFY(lcCapture().isWarningEnabled());
        QVERIFY(!lcRecord().isDebugEnabled());
        QVERIFY(!lcOverlay().isDebugEnabled());
    }

    void verboseSwitchesDebugForEveryCategory()
    {
        setVerboseLogging(true);
        QVERIFY(verboseLogging());
        for (const QLoggingCategory *category : {&lcApp(), &lcCapture(), &lcOverlay(), &lcRecord(), &lcTray(),
                                                 &lcDbus(), &lcMcp(), &lcSettings(), &lcHooks()}) {
            QVERIFY2(category->isDebugEnabled(), category->categoryName());
        }
        setVerboseLogging(false);
        QVERIFY(!lcTray().isDebugEnabled());
    }

    void journalctlArgumentsSelectItsova()
    {
        const QStringList plain = journalctlArguments({});
        QVERIFY(plain.contains(QStringLiteral("--user")));
        QVERIFY(plain.contains(QStringLiteral("SYSLOG_IDENTIFIER=itsova")));
        QVERIFY(plain.contains(QStringLiteral("--since=-1h")));
        const QStringList follow = journalctlArguments({QStringLiteral("-f")});
        QCOMPARE(follow.last(), QStringLiteral("-f"));
        QVERIFY(!follow.contains(QStringLiteral("--since=-1h")));
    }

    // #90/#92: who stopped a recording must be answerable from the journal alone.
    void stopLogsItsOrigin()
    {
        ScreenRecorder recorder;
        QTest::ignoreMessage(QtInfoMsg, QRegularExpression(QStringLiteral("stop requested by \"tray icon click\"")));
        recorder.stop(QStringLiteral("tray icon click"));
    }

    // #91: KNotification reads <componentName>.notifyrc; the shipped file must carry that name and
    // define every event the code sends.
    void notifyRcMatchesComponentAndEvents()
    {
        const QString dataFile =
            QStringLiteral(ITSOVA_SOURCE_DIR "/data/") + notifyComponentName() + QStringLiteral(".notifyrc");
        QFile file(dataFile);
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(dataFile));
        const QString rc = QString::fromUtf8(file.readAll());
        QSet<QString> defined;
        for (const auto &match : QRegularExpression(QStringLiteral("^\\[Event/(\\w+)\\]"),
                                                    QRegularExpression::MultilineOption)
                                     .globalMatch(rc)) {
            defined.insert(match.captured(1));
        }
        QVERIFY(defined.size() >= 12);
        QSet<QString> used;
        const QRegularExpression call(QStringLiteral(
            R"re((?:notify|makeNotification|KNotification)\((?:haveEvents \? )?QStringLiteral\("(\w+)"\))re"));
        QDirIterator it(QStringLiteral(ITSOVA_SOURCE_DIR "/src"), {QStringLiteral("*.cpp")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QFile source(it.next());
            QVERIFY(source.open(QIODevice::ReadOnly));
            for (const auto &match : call.globalMatch(QString::fromUtf8(source.readAll()))) {
                if (match.captured(1) != QLatin1String("notification")) {
                    used.insert(match.captured(1));
                }
            }
        }
        QVERIFY(!used.isEmpty());
        for (const QString &event : std::as_const(used)) {
            QVERIFY2(defined.contains(event), qPrintable(event));
        }
        QVERIFY(QFile::exists(QStringLiteral(ITSOVA_SOURCE_DIR "/data/itsova.notifyrc")) == false);
    }
};

QTEST_GUILESS_MAIN(LoggingTest)
#include "loggingtest.moc"
