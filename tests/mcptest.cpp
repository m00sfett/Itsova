#include "annotations/programmaticannotator.h"
#include "mcpconfirmdialog.h"
#include "mcppolicy.h"
#include "mcpserver.h"
#include "targets.h"

#include <KConfig>

#include <QBuffer>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace
{

using namespace itsova;

class MockMcpBackend final : public McpToolBackend
{
public:
    QString lastTool;
    QJsonObject lastArgs;
    McpToolResponse nextResponse;

    McpToolResponse callTool(const QString &toolName, const QJsonObject &arguments) override
    {
        lastTool = toolName;
        lastArgs = arguments;
        return nextResponse;
    }
};

class McpTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void targetStoreRoundTripAndRemovalCleanup()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString cfgPath = dir.filePath(QStringLiteral("itsovarc"));

        TargetStore store;
        Area a;
        a.id = QStringLiteral("area:one");
        a.name = QStringLiteral("AreaOne");
        a.connector = QStringLiteral("DP-1");
        a.rect = QRect(0, 0, 100, 100);
        store.areas.append(a);

        Group g;
        g.id = QStringLiteral("group:one");
        g.name = QStringLiteral("GroupOne");
        g.targetIds = {a.id};
        store.groups.append(g);

        store.mcp.enabled = true;
        store.mcp.accessMode = McpAccessMode::ConfirmPerCapture;
        store.mcp.allowedTargetIds = {a.id, g.id};
        store.mcp.notifyOnCapture = false;

        {
            KConfig cfg(cfgPath, KConfig::SimpleConfig);
            store.save(cfg);
            cfg.sync();
        }

        TargetStore loaded;
        {
            KConfig cfg(cfgPath, KConfig::SimpleConfig);
            loaded.load(cfg);
        }

        QVERIFY(loaded.mcp.enabled);
        QCOMPARE(loaded.mcp.accessMode, McpAccessMode::ConfirmPerCapture);
        QCOMPARE(loaded.mcp.allowedTargetIds, QStringList({a.id, g.id}));
        QCOMPARE(loaded.mcp.notifyOnCapture, false);

        loaded.remove(a.id);
        QCOMPARE(loaded.mcp.allowedTargetIds, QStringList({g.id}));
    }

    void policyEvaluationAcrossModes()
    {
        McpConfig cfg;
        // Disabled rejects every tool including read-only listing.
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("list_targets")).decision, McpDecision::Deny);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("list_monitors")).decision, McpDecision::Deny);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("capture_target"), QStringLiteral("area:a")).decision,
                 McpDecision::Deny);

        // Allowlist enforces per-target opt-in and blocks arbitrary regions/windows/all.
        cfg.enabled = true;
        cfg.accessMode = McpAccessMode::Allowlist;
        cfg.allowedTargetIds = {QStringLiteral("area:a"), QStringLiteral("group:g")};

        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("list_targets")).decision, McpDecision::Allow);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("list_monitors")).decision, McpDecision::Allow);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("annotate_image")).decision, McpDecision::Allow);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("list_windows")).decision, McpDecision::Deny);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("capture_region")).decision, McpDecision::Deny);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("capture_window")).decision, McpDecision::Deny);

        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("capture_target"), QStringLiteral("area:a")).decision,
                 McpDecision::Allow);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("capture_target"), QStringLiteral("all")).decision,
                 McpDecision::Deny);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("capture_target"), QStringLiteral("area:other")).decision,
                 McpDecision::Deny);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("start_interval"), QStringLiteral("group:g")).decision,
                 McpDecision::Allow);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("stop_interval"), QStringLiteral("all")).decision,
                 McpDecision::Deny);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("get_last_capture"), QStringLiteral("area:a")).decision,
                 McpDecision::Allow);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("get_last_capture"), QString()).decision, McpDecision::Deny);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("get_last_capture"), QStringLiteral("all")).decision,
                 McpDecision::Deny);

        // ConfirmPerCapture allows discovery & stopping intervals, asks on captures/last/start/annotate.
        cfg.accessMode = McpAccessMode::ConfirmPerCapture;
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("list_targets")).decision, McpDecision::Allow);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("list_windows")).decision, McpDecision::Allow);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("stop_interval"), QStringLiteral("all")).decision,
                 McpDecision::Allow);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("capture_target"), QStringLiteral("all")).decision,
                 McpDecision::NeedsConfirm);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("capture_region")).decision, McpDecision::NeedsConfirm);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("capture_window")).decision, McpDecision::NeedsConfirm);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("get_last_capture")).decision, McpDecision::NeedsConfirm);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("start_interval"), QStringLiteral("all")).decision,
                 McpDecision::NeedsConfirm);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("annotate_image")).decision, McpDecision::NeedsConfirm);

        // FullSession permits everything directly.
        cfg.accessMode = McpAccessMode::FullSession;
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("capture_window")).decision, McpDecision::Allow);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("capture_region")).decision, McpDecision::Allow);
        QCOMPARE(evaluateMcpAccess(cfg, QStringLiteral("capture_target"), QStringLiteral("all")).decision,
                 McpDecision::Allow);
    }

    void confirmDialogDefaultsAndAutoDeny()
    {
        {
            McpConfirmDialog dlg(QStringLiteral("Capture test?"), 5);
            auto *box = dlg.findChild<QDialogButtonBox *>();
            QVERIFY(box != nullptr);
            QPushButton *deny = nullptr;
            QPushButton *allow = nullptr;
            for (QAbstractButton *b : box->buttons()) {
                if (box->buttonRole(b) == QDialogButtonBox::RejectRole) {
                    deny = qobject_cast<QPushButton *>(b);
                } else if (box->buttonRole(b) == QDialogButtonBox::AcceptRole) {
                    allow = qobject_cast<QPushButton *>(b);
                }
            }
            QVERIFY(deny != nullptr);
            QVERIFY(allow != nullptr);
            QVERIFY(deny->isDefault());

            QSignalSpy spy(&dlg, &McpConfirmDialog::decided);
            dlg.show();
            QTest::qWait(20);
            allow->click();
            QCOMPARE(spy.count(), 1);
            QCOMPARE(spy.first().first().toBool(), true);
            QVERIFY(dlg.decidedAllowed());
        }

        {
            McpConfirmDialog dlg(QStringLiteral("Auto deny test?"), 1);
            QSignalSpy spy(&dlg, &McpConfirmDialog::decided);
            dlg.show();
            QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 2500);
            QCOMPARE(spy.first().first().toBool(), false);
            QCOMPARE(dlg.decidedAllowed(), false);
        }

        {
            McpConfirmDialog dlg(QStringLiteral("Reject test?"), 5);
            QSignalSpy spy(&dlg, &McpConfirmDialog::decided);
            dlg.show();
            QTest::qWait(20);
            dlg.close();
            QCOMPARE(spy.count(), 1);
            QCOMPARE(spy.first().first().toBool(), false);
        }
    }

    void mcpServerJsonRpcProtocol()
    {
        QCOMPARE(McpServer::toolDefinitions().size(), 10);

        MockMcpBackend backend;

        // 1. Initialize
        const QByteArray initReply = McpServer::processMessage(
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}})",
            backend);
        const QJsonObject initObj = QJsonDocument::fromJson(initReply).object();
        QCOMPARE(initObj.value(QStringLiteral("id")).toInt(), 1);
        const QJsonObject initRes = initObj.value(QStringLiteral("result")).toObject();
        QCOMPARE(initRes.value(QStringLiteral("protocolVersion")).toString(), QStringLiteral("2024-11-05"));
        QCOMPARE(initRes.value(QStringLiteral("serverInfo")).toObject().value(QStringLiteral("name")).toString(),
                 QStringLiteral("itsova"));
        QCOMPARE(initRes.value(QStringLiteral("serverInfo")).toObject().value(QStringLiteral("version")).toString(),
                 QStringLiteral(ITSOVA_VERSION));

        // 2. Notification produces empty reply
        QCOMPARE(McpServer::processMessage(R"({"jsonrpc":"2.0","method":"notifications/initialized"})", backend),
                 QByteArray());

        // 3. ping & tools/list
        const QJsonObject pingObj =
            QJsonDocument::fromJson(McpServer::processMessage(R"({"jsonrpc":"2.0","id":2,"method":"ping"})", backend))
                .object();
        QVERIFY(pingObj.contains(QStringLiteral("result")));

        const QJsonObject listObj =
            QJsonDocument::fromJson(
                McpServer::processMessage(R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})", backend))
                .object();
        QCOMPARE(listObj.value(QStringLiteral("result")).toObject().value(QStringLiteral("tools")).toArray().size(),
                 10);

        // 4. tools/call metadata-only vs include_image vs error
        backend.nextResponse = {true, QJsonObject{{QStringLiteral("targetId"), QStringLiteral("area:one")}}, {},
                                std::nullopt};
        const QJsonObject callMeta =
            QJsonDocument::fromJson(
                McpServer::processMessage(
                    R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"capture_target","arguments":{"target":"AreaOne"}}})",
                    backend))
                .object();
        QCOMPARE(backend.lastTool, QStringLiteral("capture_target"));
        QCOMPARE(backend.lastArgs.value(QStringLiteral("target")).toString(), QStringLiteral("AreaOne"));
        const QJsonObject callMetaRes = callMeta.value(QStringLiteral("result")).toObject();
        QCOMPARE(callMetaRes.value(QStringLiteral("isError")).toBool(true), false);
        QCOMPARE(callMetaRes.value(QStringLiteral("content")).toArray().size(), 1);

        backend.nextResponse = {true, QJsonObject{{QStringLiteral("ok"), true}}, {},
                                McpImagePayload{"AQID", QStringLiteral("image/png")}};
        const QJsonObject callImg =
            QJsonDocument::fromJson(
                McpServer::processMessage(
                    R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"get_last_capture","arguments":{"include_image":true}}})",
                    backend))
                .object();
        const QJsonArray imgContent = callImg.value(QStringLiteral("result")).toObject().value(QStringLiteral("content")).toArray();
        QCOMPARE(imgContent.size(), 2);
        QCOMPARE(imgContent.at(1).toObject().value(QStringLiteral("type")).toString(), QStringLiteral("image"));
        QCOMPARE(imgContent.at(1).toObject().value(QStringLiteral("data")).toString(), QStringLiteral("AQID"));
        QCOMPARE(imgContent.at(1).toObject().value(QStringLiteral("mimeType")).toString(), QStringLiteral("image/png"));

        backend.nextResponse = {false, {}, QStringLiteral("MCP server access is disabled."), std::nullopt};
        const QJsonObject callErr =
            QJsonDocument::fromJson(
                McpServer::processMessage(
                    R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"list_targets"}})", backend))
                .object();
        QCOMPARE(callErr.value(QStringLiteral("result")).toObject().value(QStringLiteral("isError")).toBool(false), true);

        // 5. Parse error and unknown method
        const QJsonObject parseErr = QJsonDocument::fromJson(McpServer::processMessage("{not-json", backend)).object();
        QCOMPARE(parseErr.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(), -32700);

        const QJsonObject methodErr =
            QJsonDocument::fromJson(
                McpServer::processMessage(R"({"jsonrpc":"2.0","id":7,"method":"unknown/op"})", backend))
                .object();
        QCOMPARE(methodErr.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(), -32601);

        // 6. Stream framing with notification filtered
        QByteArray inBytes =
            "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"ping\"}\n"
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
            "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"ping\"}\n";
        QBuffer inBuf(&inBytes);
        inBuf.open(QIODevice::ReadOnly);
        QByteArray outBytes;
        QBuffer outBuf(&outBytes);
        outBuf.open(QIODevice::WriteOnly);
        McpServer::runStream(backend, inBuf, outBuf);
        const QList<QByteArray> outLines = outBytes.trimmed().split('\n');
        QCOMPARE(outLines.size(), 2);
    }

    void programmaticAnnotatorVectorsAndRedaction()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString srcPath = dir.filePath(QStringLiteral("sample.png"));

        QImage src(240, 160, QImage::Format_ARGB32_Premultiplied);
        src.fill(QColor(0x1e, 0x88, 0xe5));
        {
            QPainter p(&src);
            p.fillRect(QRect(20, 20, 30, 20), QColor(0xFF, 0x98, 0x00));
            p.fillRect(QRect(60, 20, 30, 20), QColor(0x4C, 0xAF, 0x50));
            p.fillRect(QRect(100, 20, 40, 30), QColor(0xE9, 0x1E, 0x63));
        }
        src.setText(QStringLiteral("Author"), QStringLiteral("ConfidentialAuthor"));
        {
            QImageWriter writer(srcPath, "png");
            QVERIFY(writer.write(src));
        }
        QFile srcFile(srcPath);
        QVERIFY(srcFile.open(QIODevice::ReadOnly));
        const QByteArray originalBytes = srcFile.readAll();
        srcFile.close();

        QJsonArray ops;
        ops.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("arrow")},
                               {QStringLiteral("x1"), 10}, {QStringLiteral("y1"), 100},
                               {QStringLiteral("x2"), 80}, {QStringLiteral("y2"), 120}});
        ops.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("rect")},
                               {QStringLiteral("x"), 10}, {QStringLiteral("y"), 60},
                               {QStringLiteral("width"), 40}, {QStringLiteral("height"), 30},
                               {QStringLiteral("fill_mode"), QStringLiteral("translucent")}});
        ops.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("ellipse")},
                               {QStringLiteral("x"), 60}, {QStringLiteral("y"), 60},
                               {QStringLiteral("width"), 40}, {QStringLiteral("height"), 30}});
        ops.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("pen")},
                               {QStringLiteral("points"),
                                QJsonArray{QJsonArray{110, 80}, QJsonArray{130, 90}, QJsonArray{150, 85}}}});
        ops.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("highlighter")},
                               {QStringLiteral("points"), QJsonArray{QJsonArray{150, 120}, QJsonArray{210, 120}}}});
        ops.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                               {QStringLiteral("x"), 120}, {QStringLiteral("y"), 100},
                               {QStringLiteral("text"), QStringLiteral("Check")},
                               {QStringLiteral("legibility"), QStringLiteral("pill")}});
        ops.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("step")},
                               {QStringLiteral("x"), 190}, {QStringLiteral("y"), 40},
                               {QStringLiteral("pointer_x"), 170}, {QStringLiteral("pointer_y"), 50},
                               {QStringLiteral("number"), 7}});
        ops.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("spotlight")},
                               {QStringLiteral("x"), 155}, {QStringLiteral("y"), 60},
                               {QStringLiteral("width"), 60}, {QStringLiteral("height"), 50},
                               {QStringLiteral("shape"), QStringLiteral("ellipse")}});
        ops.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("redact")},
                               {QStringLiteral("x"), 20}, {QStringLiteral("y"), 20},
                               {QStringLiteral("width"), 30}, {QStringLiteral("height"), 20},
                               {QStringLiteral("mode"), QStringLiteral("solid")}});
        ops.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("redact")},
                               {QStringLiteral("x"), 60}, {QStringLiteral("y"), 20},
                               {QStringLiteral("width"), 30}, {QStringLiteral("height"), 20},
                               {QStringLiteral("mode"), QStringLiteral("pixelate")}});
        ops.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("redact")},
                               {QStringLiteral("x"), 100}, {QStringLiteral("y"), 20},
                               {QStringLiteral("width"), 40}, {QStringLiteral("height"), 30},
                               {QStringLiteral("mode"), QStringLiteral("blur")}});

        AnnotateRequest req;
        req.inputPath = srcPath;
        req.operations = ops;
        req.exportOptions.format = ImageFormat::Png;
        req.formatExplicit = true;

        const AnnotateResult res = applyProgrammaticAnnotations(req);
        QVERIFY2(res.ok, qPrintable(res.error));
        QCOMPARE(res.appliedCount, 11);
        QVERIFY(res.outputPath.endsWith(QStringLiteral("sample-annotated.png")));
        QCOMPARE(res.width, 240);
        QCOMPARE(res.height, 160);

        // Source PNG was not overwritten.
        {
            QFile f(srcPath);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(f.readAll(), originalBytes);
        }

        // Metadata stripped and solid redaction actually blackened source pixels.
        QImageReader annotatedReader(res.outputPath);
        const QImage outImg = annotatedReader.read();
        QVERIFY(!outImg.isNull());
        QVERIFY(annotatedReader.text(QStringLiteral("Author")).isEmpty());
        QCOMPARE(outImg.pixelColor(30, 30).rgb(), QColor(0, 0, 0).rgb());

        // Overwrite without flag is rejected cleanly.
        AnnotateRequest blocked = req;
        blocked.outputPath = srcPath;
        blocked.overwrite = false;
        const AnnotateResult blockedRes = applyProgrammaticAnnotations(blocked);
        QVERIFY(!blockedRes.ok);

        // Explicit overwrite=true succeeds.
        AnnotateRequest overwriteReq = blocked;
        overwriteReq.overwrite = true;
        const AnnotateResult overwriteRes = applyProgrammaticAnnotations(overwriteReq);
        QVERIFY2(overwriteRes.ok, qPrintable(overwriteRes.error));
        QCOMPARE(overwriteRes.outputPath, srcPath);
    }

    void annotateImageHardening()
    {
        QTemporaryDir captureDir;
        QTemporaryDir outsideDir;
        QVERIFY(captureDir.isValid());
        QVERIFY(outsideDir.isValid());

        const QString srcPath = captureDir.filePath(QStringLiteral("source.png"));
        const QString otherExisting = captureDir.filePath(QStringLiteral("other.png"));
        const QString outsideTarget = outsideDir.filePath(QStringLiteral("outside.png"));

        QImage img(64, 64, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::blue);
        QVERIFY(img.save(srcPath, "PNG"));
        img.fill(Qt::green);
        QVERIFY(img.save(otherExisting, "PNG"));
        img.fill(Qt::red);
        QVERIFY(img.save(outsideTarget, "PNG"));

        QJsonArray ops;
        ops.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("redact")},
                               {QStringLiteral("x"), 4},
                               {QStringLiteral("y"), 4},
                               {QStringLiteral("width"), 16},
                               {QStringLiteral("height"), 16},
                               {QStringLiteral("mode"), QStringLiteral("solid")}});

        // 1. Refuse overwriting *any* existing file without overwrite=true (not just input_path).
        AnnotateRequest req;
        req.inputPath = srcPath;
        req.outputPath = otherExisting;
        req.allowedOutputDir = captureDir.path();
        req.operations = ops;
        req.overwrite = false;
        req.exportOptions.format = ImageFormat::Png;
        req.formatExplicit = true;

        const AnnotateResult denyOther = applyProgrammaticAnnotations(req);
        QVERIFY(!denyOther.ok);
        QVERIFY(QImage(otherExisting).pixelColor(10, 10) == QColor(Qt::green));

        // Confirmation helper reports willOverwrite=true when overwrite=true on existing file.
        const AnnotateOutputTarget overwritePreview = resolveAnnotateOutputTarget(
            srcPath, otherExisting, req.exportOptions, true, captureDir.path(), true);
        QVERIFY2(overwritePreview.ok, qPrintable(overwritePreview.error));
        QVERIFY(overwritePreview.willOverwrite);
        QCOMPARE(overwritePreview.resolvedPath, otherExisting);

        // 2. Reject symlinks even with overwrite=true, and keep symlink target untouched.
        const QString linkPath = captureDir.filePath(QStringLiteral("link.png"));
        QVERIFY(QFile::link(outsideTarget, linkPath));
        AnnotateRequest symReq = req;
        symReq.outputPath = linkPath;
        symReq.overwrite = true;
        const AnnotateResult symRes = applyProgrammaticAnnotations(symReq);
        QVERIFY(!symRes.ok);
        QVERIFY(QImage(outsideTarget).pixelColor(10, 10) == QColor(Qt::red));

        // Direct writeImageToFile also refuses symlinks.
        QString writeErr;
        QVERIFY(!writeImageToFile(linkPath, img, req.exportOptions, &writeErr));
        QVERIFY(!writeErr.isEmpty());

        // 3. Confine outputPath to allowedOutputDir (blocks direct outside paths and traversal).
        AnnotateRequest outsideReq = req;
        outsideReq.outputPath = outsideDir.filePath(QStringLiteral("escape.png"));
        outsideReq.overwrite = true;
        QVERIFY(!applyProgrammaticAnnotations(outsideReq).ok);
        QVERIFY(!QFileInfo::exists(outsideReq.outputPath));

        AnnotateRequest traversalReq = req;
        traversalReq.outputPath = captureDir.filePath(QStringLiteral("../escape2.png"));
        traversalReq.overwrite = true;
        QVERIFY(!applyProgrammaticAnnotations(traversalReq).ok);

        // Symlinked subdirectory pointing outside captureDir is also rejected.
        const QString linkSubdir = captureDir.filePath(QStringLiteral("sublink"));
        QVERIFY(QFile::link(outsideDir.path(), linkSubdir));
        AnnotateRequest sublinkReq = req;
        sublinkReq.outputPath = QDir(linkSubdir).filePath(QStringLiteral("escaped.png"));
        sublinkReq.overwrite = true;
        QVERIFY(!applyProgrammaticAnnotations(sublinkReq).ok);
        QVERIFY(!QFileInfo::exists(outsideDir.filePath(QStringLiteral("escaped.png"))));

        // 4. Empty outputPath with an external inputPath writes inside allowedOutputDir.
        AnnotateRequest defaultInCapture = req;
        defaultInCapture.inputPath = outsideTarget;
        defaultInCapture.outputPath.clear();
        defaultInCapture.overwrite = false;
        const AnnotateResult defaultRes = applyProgrammaticAnnotations(defaultInCapture);
        QVERIFY2(defaultRes.ok, qPrintable(defaultRes.error));
        QVERIFY(defaultRes.outputPath.startsWith(captureDir.path() + QLatin1Char('/')));
    }
};

} // namespace

QTEST_MAIN(McpTest)

#include "mcptest.moc"
