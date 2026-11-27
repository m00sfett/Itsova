#include "app.h"

#include "notifyconfig.h"
#include "annotations/annotationdocument.h"
#include "annotations/overlayexport.h"
#include "annotations/programmaticannotator.h"
#include "areaselector.h"
#include "captureoverlay.h"
#include "logging.h"
#include "recordingmetadata.h"
#include "captureservice.h"
#include "cli.h"
#include "colorpicker.h"
#include "countdowntimer.h"
#include "historystore.h"
#include "hookrunner.h"
#include "intervalcontroller.h"
#include "latencytrace.h"
#include "mcpconfirmdialog.h"
#include "mcppolicy.h"
#include "mcpserver.h"
#include "ocrengine.h"
#include "pinmanager.h"
#include "printkey.h"
#include "quickeditorwindow.h"
#include "recordingplan.h"
#include "recordinghint.h"
#include "screenrecorder.h"
#include "controlservice.h"

#include <algorithm>
#include <mutex>
#include <utility>
#include "imageoutput.h"
#include "screens.h"
#include "settingswindow.h"
#include "shortcutmanager.h"
#include "thumbnailcache.h"
#include "traycontroller.h"
#include "triggerengine.h"
#include "triggerplan.h"
#include "windowlist.h"

#include <KConfig>
#include <KGlobalAccel>
#include <KNotification>

#include <QBuffer>
#include <QClipboard>
#include <QCoreApplication>
#include <QProcess>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QImageReader>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QLocale>
#include <QMimeData>
#include <QPointer>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent>

namespace itsova
{


namespace
{

std::optional<McpImagePayload> buildMcpImagePayload(const QString &filePath, int maxDimension, QString *errorOut)
{
    QImageReader reader(filePath);
    reader.setAutoTransform(true);
    QImage img = reader.read();
    if (img.isNull()) {
        if (errorOut) {
            *errorOut = App::tr("Could not read saved image for inline MCP output: %1").arg(reader.errorString());
        }
        return std::nullopt;
    }
    if (maxDimension > 0 && (img.width() > maxDimension || img.height() > maxDimension)) {
        img = img.scaled(maxDimension, maxDimension, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    ExportOptions opts;
    opts.format = imageFormatFromPath(filePath).value_or(ImageFormat::Png);
    if (!isImageFormatSupported(opts.format)) {
        opts.format = ImageFormat::Png;
    }

    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    QString encErr;
    if (!encodeImageToDevice(&buffer, img, opts, &encErr)) {
        if (errorOut) {
            *errorOut = encErr;
        }
        return std::nullopt;
    }

    return McpImagePayload{
        encoded.toBase64(),
        QStringLiteral("image/") + formatConfigKey(opts.format),
    };
}

} // namespace

void App::mcpDispatchAsync(const QString &toolName, const QString &argumentsJson,
                           std::function<void(const McpToolResponse &)> callback)
{
    qCInfo(lcMcp) << "tool call" << toolName;
    const auto fail = [callback](const QString &message) {
        if (callback) {
            callback({false, {}, message, std::nullopt});
        }
    };

    QJsonObject args;
    if (!argumentsJson.trimmed().isEmpty()) {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(argumentsJson.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            fail(tr("Invalid MCP tool arguments JSON: %1").arg(err.errorString()));
            return;
        }
        args = doc.object();
    }

    // Enforce the global opt-in toggle before inspecting targets, windows, or paths.
    const McpPolicyResult basePolicy = evaluateMcpAccess(store_.mcp, toolName);
    if (basePolicy.decision == McpDecision::Deny && !store_.mcp.enabled) {
        fail(basePolicy.reason);
        return;
    }

    const auto runWithOptionalConfirm = [this, callback, fail](const McpPolicyResult &policy,
                                                               const QString &confirmSummary,
                                                               bool delayAfterHide,
                                                               std::function<void()> action) {
        if (policy.decision == McpDecision::Deny) {
            fail(policy.reason);
            return;
        }
        if (policy.decision == McpDecision::Allow) {
            action();
            return;
        }
        auto *dialog = new McpConfirmDialog(confirmSummary, 30);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        QPointer<App> safeApp(this);
        QPointer<McpConfirmDialog> safeDialog(dialog);
        connect(dialog, &McpConfirmDialog::decided, this,
                [safeApp, safeDialog, fail, delayAfterHide, action = std::move(action)](bool allowed) mutable {
                    if (safeDialog) {
                        safeDialog->deleteLater();
                    }
                    if (!allowed) {
                        fail(App::tr("MCP request was denied by user or timed out."));
                        return;
                    }
                    if (!safeApp) {
                        fail(App::tr("Itsova is shutting down."));
                        return;
                    }
                    if (delayAfterHide) {
                        QTimer::singleShot(300, safeApp.data(), std::move(action));
                    } else {
                        action();
                    }
                });
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
    };

    const auto finishCaptureResult = [this, callback, fail](const CliCaptureResult &res, bool includeImage,
                                                            int maxDimension) {
        if (!res.ok) {
            fail(res.error.isEmpty() ? tr("Capture failed.") : res.error);
            return;
        }
        if (store_.mcp.notifyOnCapture) {
            QStringList names;
            names.reserve(res.files.size());
            for (const CliCaptureFile &f : res.files) {
                names.append(QFileInfo(f.path).fileName());
            }
            notify(QStringLiteral("mcpCapture"), tr("MCP captured %1").arg(res.displayName), names);
        }

        QJsonObject meta = captureResultToJson(res);
        std::optional<McpImagePayload> imagePayload;
        if (includeImage && !res.files.isEmpty()) {
            QString imgErr;
            imagePayload = buildMcpImagePayload(res.files.first().path, maxDimension, &imgErr);
            if (!imagePayload) {
                fail(imgErr);
                return;
            }
        }
        if (callback) {
            callback({true, meta, {}, std::move(imagePayload)});
        }
    };

    const auto parseFormatAndQuality = [&](std::optional<ImageFormat> *outFormat, std::optional<int> *outQuality,
                                           QString *errMsg) -> bool {
        if (args.contains(QStringLiteral("format"))) {
            const QString rawFmt = args.value(QStringLiteral("format")).toString().trimmed();
            const std::optional<ImageFormat> parsed = imageFormatFromKey(rawFmt);
            if (!parsed || !isImageFormatSupported(*parsed)) {
                *errMsg = tr("Unsupported image format '%1'.").arg(rawFmt);
                return false;
            }
            *outFormat = *parsed;
        }
        if (args.contains(QStringLiteral("quality"))) {
            const QJsonValue qVal = args.value(QStringLiteral("quality"));
            if (!qVal.isDouble()) {
                *errMsg = tr("'quality' must be an integer between 1 and 100.");
                return false;
            }
            *outQuality = std::clamp(qVal.toInt(), 1, 100);
        }
        return true;
    };

    if (toolName == QLatin1String("list_targets")) {
        if (basePolicy.decision == McpDecision::Deny) {
            fail(basePolicy.reason);
            return;
        }
        QJsonArray arr;
        const bool filterAllowlist = (store_.mcp.accessMode == McpAccessMode::Allowlist);
        for (const MenuTarget &mt : menuTargets()) {
            if (filterAllowlist && !store_.mcp.allowedTargetIds.contains(mt.id)) {
                continue;
            }
            QString kind;
            switch (mt.section) {
            case MenuTarget::Section::Group:
                kind = QStringLiteral("group");
                break;
            case MenuTarget::Section::Area:
                kind = QStringLiteral("area");
                break;
            case MenuTarget::Section::Monitor:
                kind = (mt.id == allMonitorsTargetId()) ? QStringLiteral("all") : QStringLiteral("monitor");
                break;
            }
            arr.append(QJsonObject{
                {QStringLiteral("id"), mt.id},
                {QStringLiteral("name"), mt.name},
                {QStringLiteral("kind"), kind},
                {QStringLiteral("detail"), mt.detail},
                {QStringLiteral("available"), mt.available},
            });
        }
        callback({true, QJsonObject{{QStringLiteral("targets"), arr}}, {}, std::nullopt});
        return;
    }

    if (toolName == QLatin1String("list_monitors")) {
        if (basePolicy.decision == McpDecision::Deny) {
            fail(basePolicy.reason);
            return;
        }
        const bool filterAllowlist = (store_.mcp.accessMode == McpAccessMode::Allowlist);
        bool allowAllConnectors = false;
        QSet<QString> allowedConnectors;
        if (filterAllowlist) {
            const auto addTargetConnectors = [&](const QString &tid, auto &self) -> void {
                if (tid == allMonitorsTargetId()) {
                    allowAllConnectors = true;
                } else if (isScreenTargetId(tid)) {
                    allowedConnectors.insert(connectorFromScreenTargetId(tid));
                } else if (const Area *a = store_.area(tid)) {
                    for (const QString &conn : a->involvedConnectors()) {
                        allowedConnectors.insert(conn);
                    }
                } else if (const Group *g = store_.group(tid)) {
                    for (const QString &memberId : g->targetIds) {
                        if (!store_.group(memberId)) {
                            self(memberId, self);
                        }
                    }
                }
            };
            for (const QString &id : store_.mcp.allowedTargetIds) {
                addTargetConnectors(id, addTargetConnectors);
            }
        }

        QJsonArray arr;
        for (const ScreenInfo &s : currentScreens()) {
            if (filterAllowlist && !allowAllConnectors && !allowedConnectors.contains(s.connector)) {
                continue;
            }
            arr.append(QJsonObject{
                {QStringLiteral("connector"), s.connector},
                {QStringLiteral("name"), store_.monitorName(s)},
                {QStringLiteral("identity"), s.identity()},
                {QStringLiteral("targetId"), screenTargetId(s.connector)},
                {QStringLiteral("x"), s.geometry.x()},
                {QStringLiteral("y"), s.geometry.y()},
                {QStringLiteral("width"), s.geometry.width()},
                {QStringLiteral("height"), s.geometry.height()},
            });
        }
        callback({true, QJsonObject{{QStringLiteral("monitors"), arr}}, {}, std::nullopt});
        return;
    }

    if (toolName == QLatin1String("list_windows")) {
        if (basePolicy.decision == McpDecision::Deny) {
            fail(basePolicy.reason);
            return;
        }
        QJsonArray arr;
        for (const WindowInfo &win : listWindows(QCoreApplication::applicationPid())) {
            arr.append(QJsonObject{
                {QStringLiteral("handle"), win.handle},
                {QStringLiteral("caption"), win.caption},
                {QStringLiteral("appName"), win.appName},
                {QStringLiteral("x"), win.geometry.x()},
                {QStringLiteral("y"), win.geometry.y()},
                {QStringLiteral("width"), win.geometry.width()},
                {QStringLiteral("height"), win.geometry.height()},
                {QStringLiteral("minimized"), win.minimized},
            });
        }
        callback({true, QJsonObject{{QStringLiteral("windows"), arr}}, {}, std::nullopt});
        return;
    }

    if (toolName == QLatin1String("capture_target")) {
        const QString query = args.value(QStringLiteral("target")).toString().trimmed();
        if (query.isEmpty()) {
            fail(tr("'target' is required."));
            return;
        }
        std::optional<ImageFormat> fmt;
        std::optional<int> quality;
        QString optErr;
        if (!parseFormatAndQuality(&fmt, &quality, &optErr)) {
            fail(optErr);
            return;
        }
        const bool includeImage = args.value(QStringLiteral("include_image")).toBool(false);
        const int maxDim = std::max(0, args.value(QStringLiteral("max_dimension")).toInt(0));

        const CliResolveResult resolved = resolveTarget(query);
        const McpPolicyResult policy =
            evaluateMcpAccess(store_.mcp, toolName, resolved.ok ? resolved.targetId : query);
        if (policy.decision == McpDecision::Deny) {
            fail(policy.reason);
            return;
        }
        if (!resolved.ok) {
            fail(resolved.error);
            return;
        }

        CliCaptureRequest req;
        req.target = resolved.targetId;
        req.format = fmt;
        req.quality = quality;
        if (args.contains(QStringLiteral("include_cursor"))) {
            req.cursor = args.value(QStringLiteral("include_cursor")).toBool();
        }
        req.triggerKind = QStringLiteral("mcp");

        const QString summary =
            tr("An external MCP client wants to capture \"%1\" (%2).").arg(resolved.name, resolved.targetId);
        QPointer<App> safeApp(this);
        runWithOptionalConfirm(policy, summary, true, [safeApp, req, finishCaptureResult, includeImage, maxDim, fail] {
            if (!safeApp) {
                fail(App::tr("Itsova is shutting down."));
                return;
            }
            safeApp->captureAsync(req, [finishCaptureResult, includeImage, maxDim](const CliCaptureResult &res) {
                finishCaptureResult(res, includeImage, maxDim);
            });
        });
        return;
    }

    if (toolName == QLatin1String("capture_region")) {
        if (basePolicy.decision == McpDecision::Deny) {
            fail(basePolicy.reason);
            return;
        }
        const QString monitor = args.value(QStringLiteral("monitor")).toString().trimmed();
        const QJsonValue xv = args.value(QStringLiteral("x"));
        const QJsonValue yv = args.value(QStringLiteral("y"));
        const QJsonValue wv = args.value(QStringLiteral("width"));
        const QJsonValue hv = args.value(QStringLiteral("height"));
        if (monitor.isEmpty() || !xv.isDouble() || !yv.isDouble() || !wv.isDouble() || !hv.isDouble()) {
            fail(tr("capture_region requires monitor, x, y, width, and height."));
            return;
        }
        const int x = xv.toInt();
        const int y = yv.toInt();
        const int w = wv.toInt();
        const int h = hv.toInt();
        if (w <= 0 || h <= 0) {
            fail(tr("Region width and height must be positive."));
            return;
        }
        std::optional<ImageFormat> fmt;
        std::optional<int> quality;
        QString optErr;
        if (!parseFormatAndQuality(&fmt, &quality, &optErr)) {
            fail(optErr);
            return;
        }
        const bool includeImage = args.value(QStringLiteral("include_image")).toBool(false);
        const int maxDim = std::max(0, args.value(QStringLiteral("max_dimension")).toInt(0));

        CliCaptureRequest req;
        req.target = QStringLiteral("region:%1:%2,%3,%4,%5").arg(monitor).arg(x).arg(y).arg(w).arg(h);
        req.format = fmt;
        req.quality = quality;
        if (args.contains(QStringLiteral("include_cursor"))) {
            req.cursor = args.value(QStringLiteral("include_cursor")).toBool();
        }
        req.triggerKind = QStringLiteral("mcp");

        const QString summary =
            tr("An external MCP client wants to capture region %1x%2 at (%3, %4) on %5.")
                .arg(w)
                .arg(h)
                .arg(x)
                .arg(y)
                .arg(monitor);
        QPointer<App> safeApp(this);
        runWithOptionalConfirm(basePolicy, summary, true,
                               [safeApp, req, finishCaptureResult, includeImage, maxDim, fail] {
                                   if (!safeApp) {
                                       fail(App::tr("Itsova is shutting down."));
                                       return;
                                   }
                                   safeApp->captureAsync(
                                       req, [finishCaptureResult, includeImage, maxDim](const CliCaptureResult &res) {
                                           finishCaptureResult(res, includeImage, maxDim);
                                       });
                               });
        return;
    }

    if (toolName == QLatin1String("capture_window")) {
        if (basePolicy.decision == McpDecision::Deny) {
            fail(basePolicy.reason);
            return;
        }
        const QString winId = args.value(QStringLiteral("window_id")).toString().trimmed();
        if (winId.isEmpty()) {
            fail(tr("'window_id' is required."));
            return;
        }
        std::optional<ImageFormat> fmt;
        std::optional<int> quality;
        QString optErr;
        if (!parseFormatAndQuality(&fmt, &quality, &optErr)) {
            fail(optErr);
            return;
        }
        const bool includeImage = args.value(QStringLiteral("include_image")).toBool(false);
        const int maxDim = std::max(0, args.value(QStringLiteral("max_dimension")).toInt(0));

        CliCaptureRequest req;
        req.target = QStringLiteral("window:") + winId;
        req.format = fmt;
        req.quality = quality;
        if (args.contains(QStringLiteral("include_cursor"))) {
            req.cursor = args.value(QStringLiteral("include_cursor")).toBool();
        }
        req.triggerKind = QStringLiteral("mcp");

        const QString summary = tr("An external MCP client wants to capture window \"%1\".").arg(winId);
        QPointer<App> safeApp(this);
        runWithOptionalConfirm(basePolicy, summary, true,
                               [safeApp, req, finishCaptureResult, includeImage, maxDim, fail] {
                                   if (!safeApp) {
                                       fail(App::tr("Itsova is shutting down."));
                                       return;
                                   }
                                   safeApp->captureAsync(
                                       req, [finishCaptureResult, includeImage, maxDim](const CliCaptureResult &res) {
                                           finishCaptureResult(res, includeImage, maxDim);
                                       });
                               });
        return;
    }

    if (toolName == QLatin1String("get_last_capture")) {
        const McpPolicyResult policy = evaluateMcpAccess(store_.mcp, toolName, lastCaptureTargetId_);
        if (policy.decision == McpDecision::Deny) {
            fail(policy.reason);
            return;
        }
        if (!hasLastCapture()) {
            fail(tr("No capture has been recorded yet in this Itsova session."));
            return;
        }
        const bool includeImage = args.value(QStringLiteral("include_image")).toBool(false);
        const int maxDim = std::max(0, args.value(QStringLiteral("max_dimension")).toInt(0));
        const QString label = !lastCaptureDisplayName_.isEmpty() ? lastCaptureDisplayName_ : lastPaths_.first();
        const QString summary = tr("An external MCP client wants to read the last capture (%1).").arg(label);
        QPointer<App> safeApp(this);
        runWithOptionalConfirm(policy, summary, false, [safeApp, includeImage, maxDim, callback, fail] {
            if (!safeApp) {
                fail(App::tr("Itsova is shutting down."));
                return;
            }
            const QJsonObject meta = safeApp->lastCaptureInfo();
            std::optional<McpImagePayload> img;
            if (includeImage && !safeApp->lastPaths_.isEmpty()) {
                QString imgErr;
                img = buildMcpImagePayload(safeApp->lastPaths_.first(), maxDim, &imgErr);
                if (!img) {
                    fail(imgErr);
                    return;
                }
            }
            callback({true, meta, {}, std::move(img)});
        });
        return;
    }

    if (toolName == QLatin1String("start_interval")) {
        const QString query = args.value(QStringLiteral("target")).toString().trimmed();
        if (query.isEmpty()) {
            fail(tr("'target' is required."));
            return;
        }
        const CliResolveResult resolved = resolveTarget(query);
        const McpPolicyResult policy =
            evaluateMcpAccess(store_.mcp, toolName, resolved.ok ? resolved.targetId : query);
        if (policy.decision == McpDecision::Deny) {
            fail(policy.reason);
            return;
        }
        if (!resolved.ok) {
            fail(resolved.error);
            return;
        }
        IntervalConfig cfg = store_.targetIntervalConfig(resolved.targetId);
        if (args.contains(QStringLiteral("seconds"))) {
            cfg.seconds = std::clamp(args.value(QStringLiteral("seconds")).toInt(cfg.seconds), 1, 86400);
        }
        if (args.contains(QStringLiteral("max_captures"))) {
            cfg.maxCaptures = std::max(0, args.value(QStringLiteral("max_captures")).toInt(0));
        }
        if (args.contains(QStringLiteral("max_minutes"))) {
            cfg.maxMinutes = std::max(0, args.value(QStringLiteral("max_minutes")).toInt(0));
        }
        if (args.contains(QStringLiteral("only_when_changed"))) {
            cfg.onlyWhenChanged = args.value(QStringLiteral("only_when_changed")).toBool();
        }
        const QString summary =
            tr("An external MCP client wants to start interval captures every %1 s for \"%2\".")
                .arg(cfg.seconds)
                .arg(resolved.name);
        QPointer<App> safeApp(this);
        runWithOptionalConfirm(policy, summary, true, [safeApp, resolved, cfg, callback, fail] {
            if (!safeApp) {
                fail(App::tr("Itsova is shutting down."));
                return;
            }
            const QString err = safeApp->startInterval(resolved.targetId, cfg);
            if (!err.isEmpty()) {
                fail(err);
                return;
            }
            callback({true,
                      QJsonObject{
                          {QStringLiteral("targetId"), resolved.targetId},
                          {QStringLiteral("displayName"), resolved.name},
                          {QStringLiteral("active"), true},
                          {QStringLiteral("seconds"), cfg.seconds},
                          {QStringLiteral("maxCaptures"), cfg.maxCaptures},
                          {QStringLiteral("maxMinutes"), cfg.maxMinutes},
                          {QStringLiteral("onlyWhenChanged"), cfg.onlyWhenChanged},
                      },
                      {},
                      std::nullopt});
        });
        return;
    }

    if (toolName == QLatin1String("stop_interval")) {
        const bool stopAll = args.value(QStringLiteral("all")).toBool(false);
        if (stopAll) {
            const McpPolicyResult policy = evaluateMcpAccess(store_.mcp, toolName, allMonitorsTargetId());
            if (policy.decision == McpDecision::Deny) {
                fail(policy.reason);
                return;
            }
            stopAllIntervals();
            callback({true, QJsonObject{{QStringLiteral("stoppedAll"), true}}, {}, std::nullopt});
            return;
        }
        const QString query = args.value(QStringLiteral("target")).toString().trimmed();
        if (query.isEmpty()) {
            fail(tr("Specify 'target' or set 'all': true."));
            return;
        }
        const CliResolveResult resolved = resolveTarget(query);
        const McpPolicyResult policy =
            evaluateMcpAccess(store_.mcp, toolName, resolved.ok ? resolved.targetId : query);
        if (policy.decision == McpDecision::Deny) {
            fail(policy.reason);
            return;
        }
        if (!resolved.ok) {
            fail(resolved.error);
            return;
        }
        const bool stopped = stopInterval(resolved.targetId);
        callback({true,
                  QJsonObject{
                      {QStringLiteral("targetId"), resolved.targetId},
                      {QStringLiteral("displayName"), resolved.name},
                      {QStringLiteral("stopped"), stopped},
                  },
                  {},
                  std::nullopt});
        return;
    }

    if (toolName == QLatin1String("annotate_image")) {
        const QString inputPath = args.value(QStringLiteral("input_path")).toString().trimmed();
        const QString outputPath = args.value(QStringLiteral("output_path")).toString().trimmed();
        const QJsonValue opsVal = args.value(QStringLiteral("operations"));
        if (inputPath.isEmpty() || !opsVal.isArray() || opsVal.toArray().isEmpty()) {
            fail(tr("annotate_image requires a non-empty input_path and operations array."));
            return;
        }
        std::optional<ImageFormat> fmt;
        std::optional<int> quality;
        QString optErr;
        if (!parseFormatAndQuality(&fmt, &quality, &optErr)) {
            fail(optErr);
            return;
        }
        if (basePolicy.decision == McpDecision::Deny) {
            fail(basePolicy.reason);
            return;
        }

        const QString captureDir = store_.effectiveCaptureDirectory();
        QDir().mkpath(captureDir);
        const QString canonicalCaptureDir = QFileInfo(captureDir).canonicalFilePath();

        if (store_.mcp.accessMode == McpAccessMode::Allowlist) {
            const auto isExistingFileInsideCaptureDir = [&](const QString &candidate) -> bool {
                if (canonicalCaptureDir.isEmpty()) {
                    return false;
                }
                const QFileInfo fi(candidate);
                const QString canon = fi.canonicalFilePath();
                if (canon.isEmpty()) {
                    return false;
                }
                const QString prefix = canonicalCaptureDir.endsWith(QLatin1Char('/'))
                                           ? canonicalCaptureDir
                                           : canonicalCaptureDir + QLatin1Char('/');
                return canon.startsWith(prefix);
            };
            if (!isExistingFileInsideCaptureDir(inputPath)) {
                fail(tr("In MCP Allowlist mode, input_path must be an existing file inside Itsova's capture "
                        "directory."));
                return;
            }
        }

        AnnotateRequest req;
        req.inputPath = inputPath;
        req.outputPath = outputPath;
        req.allowedOutputDir = captureDir;
        req.operations = opsVal.toArray();
        req.overwrite = args.value(QStringLiteral("overwrite")).toBool(false);
        req.exportOptions = store_.exportOptions;
        if (fmt) {
            req.exportOptions.format = *fmt;
            req.formatExplicit = true;
        }
        if (quality) {
            req.exportOptions.quality = *quality;
        }

        // Enforce capture-directory confinement, symlink rejection, and overwrite protection across all MCP modes.
        const AnnotateOutputTarget target = resolveAnnotateOutputTarget(
            req.inputPath, req.outputPath, req.exportOptions, req.formatExplicit, req.allowedOutputDir, req.overwrite);
        if (!target.ok) {
            fail(target.error);
            return;
        }

        QString summary = tr("An external MCP client wants to annotate \"%1\" (%2 operation(s)).\n\nOutput: %3")
                              .arg(QFileInfo(inputPath).fileName())
                              .arg(req.operations.size())
                              .arg(target.resolvedPath);
        if (target.willOverwrite) {
            summary += QStringLiteral("\n\n") + tr("Warning: this will overwrite the existing file.");
        }
        QPointer<App> safeApp(this);
        runWithOptionalConfirm(basePolicy, summary, false, [safeApp, req, callback, fail] {
            if (!safeApp) {
                fail(App::tr("Itsova is shutting down."));
                return;
            }
            auto *watcher = new QFutureWatcher<AnnotateResult>(safeApp.data());
            QObject::connect(watcher, &QFutureWatcher<AnnotateResult>::finished, safeApp.data(),
                             [watcher, callback, fail] {
                                 const AnnotateResult res = watcher->result();
                                 watcher->deleteLater();
                                 if (!res.ok) {
                                     fail(res.error);
                                     return;
                                 }
                                 callback({true,
                                           QJsonObject{
                                               {QStringLiteral("outputPath"), res.outputPath},
                                               {QStringLiteral("width"), res.width},
                                               {QStringLiteral("height"), res.height},
                                               {QStringLiteral("format"), res.format},
                                               {QStringLiteral("bytes"), res.bytes},
                                               {QStringLiteral("appliedCount"), res.appliedCount},
                                           },
                                           {},
                                           std::nullopt});
                             });
            watcher->setFuture(QtConcurrent::run([req] { return applyProgrammaticAnnotations(req); }));
        });
        return;
    }

    fail(tr("Unknown MCP tool: %1").arg(toolName));
}

} // namespace itsova
