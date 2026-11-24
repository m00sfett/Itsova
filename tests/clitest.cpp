#include "cli.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace glimply;

namespace
{

class MockCliBackend final : public CliBackend
{
public:
    CliCaptureRequest lastCaptureReq;
    CliCaptureResult nextCaptureResult;
    QJsonArray targets{
        QJsonObject{
            {QStringLiteral("targetId"), QStringLiteral("area:111")},
            {QStringLiteral("kind"), QStringLiteral("area")},
            {QStringLiteral("name"), QStringLiteral("DP-1-1")},
            {QStringLiteral("detail"), QStringLiteral("DP-1 · 200 × 100 at 10, 20")},
            {QStringLiteral("available"), true},
            {QStringLiteral("intervalSeconds"), 10},
            {QStringLiteral("intervalMaxCaptures"), 5},
            {QStringLiteral("intervalMaxMinutes"), 2},
            {QStringLiteral("intervalOnlyWhenChanged"), false},
        },
        QJsonObject{
            {QStringLiteral("targetId"), QStringLiteral("all")},
            {QStringLiteral("kind"), QStringLiteral("all")},
            {QStringLiteral("name"), QStringLiteral("All monitors")},
            {QStringLiteral("available"), true},
        },
    };
    QJsonArray monitors{
        QJsonObject{
            {QStringLiteral("connector"), QStringLiteral("DP-1")},
            {QStringLiteral("name"), QStringLiteral("Left")},
            {QStringLiteral("width"), 3072},
            {QStringLiteral("height"), 1728},
            {QStringLiteral("x"), 0},
            {QStringLiteral("y"), 0},
        },
    };
    QJsonArray windows{
        QJsonObject{
            {QStringLiteral("handle"), QStringLiteral("win-uuid-1")},
            {QStringLiteral("appName"), QStringLiteral("konsole")},
            {QStringLiteral("caption"), QStringLiteral("Terminal")},
        },
    };
    QJsonArray activeIntervals{
        QJsonObject{
            {QStringLiteral("targetId"), QStringLiteral("area:111")},
            {QStringLiteral("displayName"), QStringLiteral("DP-1-1")},
            {QStringLiteral("seconds"), 10},
            {QStringLiteral("captureCount"), 3},
        },
    };

    QString lastAddedName;
    QString lastAddedConnector;
    QRect lastAddedRect;
    QString lastRenamedId;
    QString lastRenamedNewName;
    QString lastRemovedId;
    QString lastIntervalId;
    int lastIntervalSeconds = -1;
    int lastIntervalMaxCaptures = -1;
    int lastIntervalMaxMinutes = -1;
    bool lastIntervalOnlyChanged = false;
    bool stoppedAll = false;
    QString lastOverlayMode;
    int lastOverlayDelay = -1;

    CliCaptureResult capture(const CliCaptureRequest &request) override
    {
        lastCaptureReq = request;
        return nextCaptureResult;
    }

    CliSimpleResult listTargets(QJsonArray *outArray) override
    {
        *outArray = targets;
        return {true, 0, {}, {}};
    }

    CliSimpleResult listMonitors(QJsonArray *outArray) override
    {
        *outArray = monitors;
        return {true, 0, {}, {}};
    }

    CliSimpleResult listWindows(QJsonArray *outArray) override
    {
        *outArray = windows;
        return {true, 0, {}, {}};
    }

    CliResolveResult resolveTarget(const QString &query) override
    {
        if (query == QLatin1String("DP-1-1") || query == QLatin1String("area:111")) {
            return {true, QStringLiteral("area:111"), QStringLiteral("area"), QStringLiteral("DP-1-1"), {}};
        }
        if (query == QLatin1String("DP-1")) {
            return {true, QStringLiteral("screen:DP-1"), QStringLiteral("monitor"), QStringLiteral("Left"), {}};
        }
        return {false, {}, {}, {}, QStringLiteral("Target “%1” was not found.").arg(query)};
    }

    CliSimpleResult addAreaInteractive() override
    {
        return {true, 0, {}, {}};
    }

    CliSimpleResult addAreaRect(const QString &name, const QString &connector, const QRect &rect) override
    {
        lastAddedName = name;
        lastAddedConnector = connector;
        lastAddedRect = rect;
        return {true,
                0,
                {{QStringLiteral("targetId"), QStringLiteral("area:222")},
                 {QStringLiteral("name"), name.isEmpty() ? QStringLiteral("DP-1-2") : name}},
                {}};
    }

    CliSimpleResult editArea(const QString &) override
    {
        return {true, 0, {}, {}};
    }

    CliSimpleResult renameTarget(const QString &targetId, const QString &newName) override
    {
        lastRenamedId = targetId;
        lastRenamedNewName = newName;
        return {true, 0, {}, {}};
    }

    CliSimpleResult removeTarget(const QString &targetId) override
    {
        lastRemovedId = targetId;
        return {true, 0, {}, {}};
    }

    CliSimpleResult startInterval(const QString &targetId, int seconds, int maxCaptures, int maxMinutes,
                                  bool onlyWhenChanged) override
    {
        lastIntervalId = targetId;
        lastIntervalSeconds = seconds;
        lastIntervalMaxCaptures = maxCaptures;
        lastIntervalMaxMinutes = maxMinutes;
        lastIntervalOnlyChanged = onlyWhenChanged;
        return {true, 0, {}, {}};
    }

    CliSimpleResult stopInterval(const QString &targetId) override
    {
        lastIntervalId = targetId;
        return {true, 0, {}, {}};
    }

    CliSimpleResult stopAllIntervals() override
    {
        stoppedAll = true;
        return {true, 0, {}, {}};
    }

    CliSimpleResult intervalStatus(QJsonArray *outArray) override
    {
        *outArray = activeIntervals;
        return {true, 0, {}, {}};
    }

    QJsonArray historyItems{
        QJsonObject{
            {QStringLiteral("id"), QStringLiteral("e2")},
            {QStringLiteral("timestamp"), QStringLiteral("2026-11-24T15:01:02.000")},
            {QStringLiteral("targetName"), QStringLiteral("Both Monitors")},
            {QStringLiteral("files"),
             QJsonArray{QJsonObject{{QStringLiteral("path"), QStringLiteral("/p/a.png")}},
                        QJsonObject{{QStringLiteral("path"), QStringLiteral("/p/b.png")}}}},
        },
        QJsonObject{
            {QStringLiteral("id"), QStringLiteral("e1")},
            {QStringLiteral("timestamp"), QStringLiteral("2026-11-24T14:00:00.000")},
            {QStringLiteral("targetName"), QStringLiteral("Work Area")},
            {QStringLiteral("files"), QJsonArray{QJsonObject{{QStringLiteral("path"), QStringLiteral("/p/c.png")}}}},
        },
    };

    CliSimpleResult history(QJsonArray *outArray) override
    {
        *outArray = historyItems;
        return {true, 0, {}, {}};
    }

    CliSimpleResult openOverlay(const QString &mode, int delaySeconds) override
    {
        lastOverlayMode = mode;
        lastOverlayDelay = delaySeconds;
        return {true, 0, {}, {}};
    }
};

} // namespace

class CliTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void captureJsonAndOptions();
    void captureStdoutBinaryAndCleanup();
    void captureErrorsAndExitCodes();
    void captureRelativeOutIsAbsolute();
    void capturePartialFailureReportsFiles();
    void listSubcommands();
    void areaSubcommands();
    void intervalAndOverlaySubcommands();
    void requestJsonRoundTrip();
    void historySubcommand();
};

void CliTest::historySubcommand()
{
    MockCliBackend backend;
    QBuffer out;
    QBuffer err;
    out.open(QIODevice::ReadWrite);
    err.open(QIODevice::ReadWrite);
    QCOMPARE(runCli({QStringLiteral("glimply"), QStringLiteral("history")}, backend, out, err), 0);
    const QStringList lines = QString::fromUtf8(out.data()).trimmed().split(QLatin1Char('\n'));
    QCOMPARE(lines.size(), 3);
    QCOMPARE(lines.first(), QStringLiteral("2026-11-24 15:01:02\tBoth Monitors\t/p/a.png"));
    QCOMPARE(lines.last(), QStringLiteral("2026-11-24 14:00:00\tWork Area\t/p/c.png"));

    QBuffer jsonOut;
    jsonOut.open(QIODevice::ReadWrite);
    QCOMPARE(runCli({QStringLiteral("glimply"), QStringLiteral("history"), QStringLiteral("--limit"), QStringLiteral("1"),
                     QStringLiteral("--json")},
                    backend, jsonOut, err),
             0);
    const QJsonArray parsed = QJsonDocument::fromJson(jsonOut.data()).array();
    QCOMPARE(parsed.size(), 1);
    QCOMPARE(parsed.first().toObject().value(QStringLiteral("id")).toString(), QStringLiteral("e2"));

    QBuffer badOut;
    badOut.open(QIODevice::ReadWrite);
    QCOMPARE(runCli({QStringLiteral("glimply"), QStringLiteral("history"), QStringLiteral("--limit"), QStringLiteral("0")},
                    backend, badOut, err),
             2);
    QVERIFY(isCliSubcommand(QStringLiteral("history")));
}

void CliTest::captureJsonAndOptions()
{
    MockCliBackend backend;
    backend.nextCaptureResult.ok = true;
    backend.nextCaptureResult.exitCode = 0;
    backend.nextCaptureResult.targetId = QStringLiteral("area:111");
    backend.nextCaptureResult.displayName = QStringLiteral("DP-1-1");
    backend.nextCaptureResult.timestamp = QStringLiteral("2026-11-24T15:00:00");
    backend.nextCaptureResult.files = {
        {QStringLiteral("area:111"), QStringLiteral("/tmp/shot.jpg"), 200, 100, QStringLiteral("jpeg"), 4321}};

    QBuffer out;
    QBuffer err;
    out.open(QIODevice::ReadWrite);
    err.open(QIODevice::ReadWrite);

    const int rc = runCli({QStringLiteral("glimply"), QStringLiteral("capture"), QStringLiteral("DP-1-1"),
                           QStringLiteral("--format"), QStringLiteral("jpeg"), QStringLiteral("--quality"),
                           QStringLiteral("82"), QStringLiteral("--delay"), QStringLiteral("3"), QStringLiteral("--cursor"),
                           QStringLiteral("--out"), QStringLiteral("/tmp/shot.jpg"), QStringLiteral("--json")},
                          backend, out, err);

    QCOMPARE(rc, 0);
    QVERIFY(err.data().isEmpty());
    QCOMPARE(backend.lastCaptureReq.target, QStringLiteral("DP-1-1"));
    QCOMPARE(backend.lastCaptureReq.format, ImageFormat::Jpeg);
    QCOMPARE(backend.lastCaptureReq.quality, 82);
    QCOMPARE(backend.lastCaptureReq.delaySeconds, 3);
    QCOMPARE(backend.lastCaptureReq.cursor, true);
    QCOMPARE(backend.lastCaptureReq.outPath, QStringLiteral("/tmp/shot.jpg"));

    QJsonParseError parseErr{};
    const QJsonDocument doc = QJsonDocument::fromJson(out.data(), &parseErr);
    QCOMPARE(parseErr.error, QJsonParseError::NoError);
    QVERIFY(doc.object().value(QStringLiteral("ok")).toBool());
    QCOMPARE(doc.object().value(QStringLiteral("files")).toArray().size(), 1);
    QCOMPARE(doc.object().value(QStringLiteral("files")).toArray().first().toObject().value(QStringLiteral("width")).toInt(),
             200);
}

void CliTest::captureStdoutBinaryAndCleanup()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    QImage img(8, 8, QImage::Format_RGBA8888);
    img.fill(Qt::red);
    const QString tmpPath = tempDir.filePath(QStringLiteral("temp-stdout.png"));
    QVERIFY(writeImageToFile(tmpPath, img, ExportOptions{ImageFormat::Png, 90, 6, false}));

    QFile f(tmpPath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray expectedBytes = f.readAll();
    f.close();
    QVERIFY(!expectedBytes.isEmpty());

    MockCliBackend backend;
    backend.nextCaptureResult.ok = true;
    backend.nextCaptureResult.exitCode = 0;
    backend.nextCaptureResult.displayName = QStringLiteral("DP-1");
    backend.nextCaptureResult.stdoutFile = tmpPath;
    backend.nextCaptureResult.tempStdoutFile = true;

    QBuffer out;
    QBuffer err;
    out.open(QIODevice::ReadWrite);
    err.open(QIODevice::ReadWrite);

    const int rc = runCli({QStringLiteral("glimply"), QStringLiteral("capture"), QStringLiteral("DP-1"),
                           QStringLiteral("--stdout"), QStringLiteral("--no-save")},
                          backend, out, err);
    QCOMPARE(rc, 0);
    QVERIFY(err.data().isEmpty());
    QCOMPARE(out.data(), expectedBytes);
    QVERIFY(!QFile::exists(tmpPath));
}

void CliTest::captureErrorsAndExitCodes()
{
    MockCliBackend backend;
    QBuffer out;
    QBuffer err;
    out.open(QIODevice::ReadWrite);
    err.open(QIODevice::ReadWrite);

    QCOMPARE(runCli({QStringLiteral("glimply"), QStringLiteral("capture"), QStringLiteral("DP-1"),
                     QStringLiteral("--stdout"), QStringLiteral("--json")},
                    backend, out, err),
             2);
    QVERIFY(out.data().isEmpty());
    QVERIFY(!err.data().isEmpty());

    out.setData({});
    err.setData({});
    QCOMPARE(runCli({QStringLiteral("glimply"), QStringLiteral("capture"), QStringLiteral("DP-1"),
                     QStringLiteral("--quality"), QStringLiteral("0")},
                    backend, out, err),
             2);

    out.setData({});
    err.setData({});
    backend.nextCaptureResult.ok = false;
    backend.nextCaptureResult.exitCode = 3;
    backend.nextCaptureResult.error = QStringLiteral("Not found");
    QCOMPARE(runCli({QStringLiteral("glimply"), QStringLiteral("capture"), QStringLiteral("missing")}, backend, out, err),
             3);
}

void CliTest::captureRelativeOutIsAbsolute()
{
    MockCliBackend backend;
    backend.nextCaptureResult.ok = true;
    QBuffer out;
    QBuffer err;
    out.open(QIODevice::ReadWrite);
    err.open(QIODevice::ReadWrite);

    const int rc = runCli({QStringLiteral("glimply"), QStringLiteral("capture"), QStringLiteral("DP-1-1"),
                           QStringLiteral("--out"), QStringLiteral("sub/shot.png")},
                          backend, out, err);
    QCOMPARE(rc, 0);
    // The daemon runs in another working directory; the CLI must send an absolute path.
    QCOMPARE(backend.lastCaptureReq.outPath, QDir::current().absoluteFilePath(QStringLiteral("sub/shot.png")));
}

void CliTest::capturePartialFailureReportsFiles()
{
    MockCliBackend backend;
    backend.nextCaptureResult.ok = false;
    backend.nextCaptureResult.exitCode = 1;
    backend.nextCaptureResult.error = QStringLiteral("Area B: monitor capture failed.");
    backend.nextCaptureResult.files.append(
        {QStringLiteral("area:111"), QStringLiteral("/tmp/a.png"), 10, 10, QStringLiteral("png"), 123});

    QBuffer out;
    QBuffer err;
    out.open(QIODevice::ReadWrite);
    err.open(QIODevice::ReadWrite);
    const int rc = runCli({QStringLiteral("glimply"), QStringLiteral("capture"), QStringLiteral("Dual"),
                           QStringLiteral("--json")},
                          backend, out, err);
    QCOMPARE(rc, 1);
    const QJsonObject obj = QJsonDocument::fromJson(out.data()).object();
    QVERIFY(!obj.value(QStringLiteral("ok")).toBool());
    const QJsonArray files = obj.value(QStringLiteral("files")).toArray();
    QCOMPARE(files.size(), 1);
    QCOMPARE(files.first().toObject().value(QStringLiteral("path")).toString(), QStringLiteral("/tmp/a.png"));

    QBuffer textOut;
    QBuffer textErr;
    textOut.open(QIODevice::ReadWrite);
    textErr.open(QIODevice::ReadWrite);
    QCOMPARE(runCli({QStringLiteral("glimply"), QStringLiteral("capture"), QStringLiteral("Dual")}, backend,
                    textOut, textErr),
             1);
    QVERIFY(textOut.data().contains("/tmp/a.png"));
    QVERIFY(textErr.data().contains("monitor capture failed"));
}

void CliTest::listSubcommands()
{
    MockCliBackend backend;
    QBuffer out;
    QBuffer err;
    out.open(QIODevice::ReadWrite);
    err.open(QIODevice::ReadWrite);

    QCOMPARE(runCli({QStringLiteral("glimply"), QStringLiteral("list"), QStringLiteral("--kind"),
                     QStringLiteral("windows"), QStringLiteral("--json")},
                    backend, out, err),
             0);
    QVERIFY(err.data().isEmpty());
    const QJsonDocument doc = QJsonDocument::fromJson(out.data());
    QVERIFY(doc.isArray());
    QCOMPARE(doc.array().first().toObject().value(QStringLiteral("appName")).toString(), QStringLiteral("konsole"));

    out.setData({});
    err.setData({});
    QCOMPARE(runCli({QStringLiteral("glimply"), QStringLiteral("list"), QStringLiteral("--kind"),
                     QStringLiteral("invalid")},
                    backend, out, err),
             2);
}

void CliTest::areaSubcommands()
{
    MockCliBackend backend;
    QBuffer out;
    QBuffer err;
    out.open(QIODevice::ReadWrite);
    err.open(QIODevice::ReadWrite);

    QCOMPARE(runCli({QStringLiteral("glimply"), QStringLiteral("area"), QStringLiteral("add"), QStringLiteral("--name"),
                     QStringLiteral("Box"), QStringLiteral("--monitor"), QStringLiteral("DP-1"),
                     QStringLiteral("--rect"), QStringLiteral("10,20,300,200"), QStringLiteral("--json")},
                    backend, out, err),
             0);
    QCOMPARE(backend.lastAddedName, QStringLiteral("Box"));
    QCOMPARE(backend.lastAddedConnector, QStringLiteral("DP-1"));
    QCOMPARE(backend.lastAddedRect, QRect(10, 20, 300, 200));

    out.setData({});
    err.setData({});
    QCOMPARE(runCli({QStringLiteral("glimply"), QStringLiteral("area"), QStringLiteral("rename"),
                     QStringLiteral("DP-1-1"), QStringLiteral("RenamedBox"), QStringLiteral("--json")},
                    backend, out, err),
             0);
    QCOMPARE(backend.lastRenamedId, QStringLiteral("area:111"));
    QCOMPARE(backend.lastRenamedNewName, QStringLiteral("RenamedBox"));

    out.setData({});
    err.setData({});
    QCOMPARE(runCli({QStringLiteral("glimply"), QStringLiteral("area"), QStringLiteral("remove"),
                     QStringLiteral("DP-1"), QStringLiteral("--json")},
                    backend, out, err),
             2);

    out.setData({});
    err.setData({});
    QCOMPARE(runCli({QStringLiteral("glimply"), QStringLiteral("area"), QStringLiteral("remove"),
                     QStringLiteral("DP-1-1"), QStringLiteral("--json")},
                    backend, out, err),
             0);
    QCOMPARE(backend.lastRemovedId, QStringLiteral("area:111"));
}

void CliTest::intervalAndOverlaySubcommands()
{
    MockCliBackend backend;
    QBuffer out;
    QBuffer err;
    out.open(QIODevice::ReadWrite);
    err.open(QIODevice::ReadWrite);

    // Passing only --only-changed should preserve target's saved seconds/maxCaptures/maxMinutes from Targets().
    QCOMPARE(runCli({QStringLiteral("glimply"), QStringLiteral("interval"), QStringLiteral("start"),
                     QStringLiteral("DP-1-1"), QStringLiteral("--only-changed"), QStringLiteral("--json")},
                    backend, out, err),
             0);
    QCOMPARE(backend.lastIntervalId, QStringLiteral("area:111"));
    QCOMPARE(backend.lastIntervalSeconds, 10);
    QCOMPARE(backend.lastIntervalMaxCaptures, 5);
    QCOMPARE(backend.lastIntervalMaxMinutes, 2);
    QCOMPARE(backend.lastIntervalOnlyChanged, true);

    out.setData({});
    err.setData({});
    QCOMPARE(runCli({QStringLiteral("glimply"), QStringLiteral("interval"), QStringLiteral("stop"),
                     QStringLiteral("--all"), QStringLiteral("--json")},
                    backend, out, err),
             0);
    QVERIFY(backend.stoppedAll);

    out.setData({});
    err.setData({});
    QCOMPARE(runCli({QStringLiteral("glimply"), QStringLiteral("overlay"), QStringLiteral("--mode"),
                     QStringLiteral("live"), QStringLiteral("--delay"), QStringLiteral("4")},
                    backend, out, err),
             0);
    QCOMPARE(backend.lastOverlayMode, QStringLiteral("live"));
    QCOMPARE(backend.lastOverlayDelay, 4);

    out.setData({});
    err.setData({});
    QCOMPARE(runCli({QStringLiteral("glimply"), QStringLiteral("overlay"), QStringLiteral("--mode"),
                     QStringLiteral("bad")},
                    backend, out, err),
             2);
}

void CliTest::requestJsonRoundTrip()
{
    CliCaptureRequest req;
    req.target = QStringLiteral("all");
    req.format = ImageFormat::WebP;
    req.quality = 88;
    req.lossless = true;
    req.cursor = true;
    req.delaySeconds = 5;
    req.outPath = QStringLiteral("/tmp/out.webp");
    req.toStdout = true;
    req.clipboard = true;
    req.noSave = true;

    bool ok = false;
    const CliCaptureRequest decoded = captureRequestFromJson(captureRequestToJson(req), &ok);
    QVERIFY(ok);
    QCOMPARE(decoded.target, req.target);
    QCOMPARE(decoded.format, req.format);
    QCOMPARE(decoded.quality, req.quality);
    QCOMPARE(decoded.lossless, req.lossless);
    QCOMPARE(decoded.cursor, req.cursor);
    QCOMPARE(decoded.delaySeconds, req.delaySeconds);
    QCOMPARE(decoded.outPath, req.outPath);
    QCOMPARE(decoded.toStdout, req.toStdout);
    QCOMPARE(decoded.clipboard, req.clipboard);
    QCOMPARE(decoded.noSave, req.noSave);
}

QTEST_MAIN(CliTest)
#include "clitest.moc"
