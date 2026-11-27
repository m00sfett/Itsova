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


CliResolveResult App::resolveTarget(const QString &query) const
{
    const QString q = query.trimmed();
    if (q.isEmpty()) {
        return {false, {}, {}, {}, tr("Target query is empty.")};
    }

    const QList<ScreenInfo> screens = currentScreens();
    if (q.compare(allMonitorsTargetId(), Qt::CaseInsensitive) == 0) {
        if (screens.isEmpty()) {
            return {false, {}, {}, {}, tr("No monitor is connected.")};
        }
        return {true, allMonitorsTargetId(), QStringLiteral("all"), tr("All monitors"), {}};
    }

    if (const Area *area = store_.area(q)) {
        return {true, area->id, QStringLiteral("area"), area->name, {}};
    }
    if (const Group *group = store_.group(q)) {
        return {true, group->id, QStringLiteral("group"), group->name, {}};
    }
    if (isScreenTargetId(q)) {
        const QString conn = connectorFromScreenTargetId(q);
        for (const ScreenInfo &s : screens) {
            if (s.connector == conn) {
                return {true, q, QStringLiteral("monitor"), store_.monitorName(s), {}};
            }
        }
        return {false, {}, {}, {}, tr("Monitor %1 is not connected.").arg(conn)};
    }

    if (q.startsWith(QLatin1String("monitor:"), Qt::CaseInsensitive)) {
        const QString want = q.mid(8).trimmed();
        for (const ScreenInfo &s : screens) {
            if (s.connector.compare(want, Qt::CaseInsensitive) == 0
                || store_.monitorName(s).compare(want, Qt::CaseInsensitive) == 0) {
                return {true, screenTargetId(s.connector), QStringLiteral("monitor"), store_.monitorName(s), {}};
            }
        }
        return {false, {}, {}, {}, tr("Monitor “%1” was not found or is not connected.").arg(want)};
    }

    QList<CliResolveResult> matches;
    for (const Group &group : store_.groups) {
        if (group.name.compare(q, Qt::CaseInsensitive) == 0) {
            matches.append({true, group.id, QStringLiteral("group"), group.name, {}});
        }
    }
    for (const Area &area : store_.areas) {
        if (area.name.compare(q, Qt::CaseInsensitive) == 0) {
            matches.append({true, area.id, QStringLiteral("area"), area.name, {}});
        }
    }
    for (const ScreenInfo &s : screens) {
        if (s.connector.compare(q, Qt::CaseInsensitive) == 0
            || store_.monitorName(s).compare(q, Qt::CaseInsensitive) == 0) {
            matches.append({true, screenTargetId(s.connector), QStringLiteral("monitor"), store_.monitorName(s), {}});
        }
    }
    if (matches.size() == 1) {
        return matches.first();
    }
    if (matches.size() > 1) {
        return {false, {}, {}, {}, tr("Target “%1” matches multiple targets.").arg(q)};
    }
    return {false, {}, {}, {}, tr("Target “%1” was not found.").arg(q)};
}

QJsonObject App::addAreaRect(const QString &name, const QString &connector, const QRect &rect)
{
    if (!rect.isValid() || rect.width() <= 0 || rect.height() <= 0) {
        return {{QStringLiteral("ok"), false}, {QStringLiteral("exitCode"), 2},
                {QStringLiteral("error"), tr("Area width and height must be positive.")}};
    }
    const QList<ScreenInfo> screens = currentScreens();
    if (screens.isEmpty()) {
        return {{QStringLiteral("ok"), false}, {QStringLiteral("exitCode"), 3},
                {QStringLiteral("error"), tr("No monitor is connected.")}};
    }

    const QString trimmedName = name.trimmed();
    QStringList connectors;
    for (const ScreenInfo &s : screens) {
        connectors.append(s.connector);
    }
    if (!trimmedName.isEmpty()) {
        if (sanitizeFilenameToken(trimmedName).isEmpty()) {
            return {{QStringLiteral("ok"), false}, {QStringLiteral("exitCode"), 2},
                    {QStringLiteral("error"), tr("Area name cannot be used in a file name.")}};
        }
        if (!store_.isNameAvailable(trimmedName, QString(), connectors)) {
            return {{QStringLiteral("ok"), false}, {QStringLiteral("exitCode"), 1},
                    {QStringLiteral("error"), tr("“%1” is already used by another target or group.").arg(trimmedName)}};
        }
    }

    QString createdId;
    if (!connector.trimmed().isEmpty()) {
        const auto it = std::find_if(screens.cbegin(), screens.cend(), [&](const ScreenInfo &s) {
            return s.connector.compare(connector.trimmed(), Qt::CaseInsensitive) == 0
                || store_.monitorName(s).compare(connector.trimmed(), Qt::CaseInsensitive) == 0;
        });
        if (it == screens.cend()) {
            return {{QStringLiteral("ok"), false}, {QStringLiteral("exitCode"), 3},
                    {QStringLiteral("error"), tr("Monitor “%1” is not connected.").arg(connector)}};
        }
        if (!QRect(QPoint(), it->geometry.size()).contains(rect)) {
            return {{QStringLiteral("ok"), false}, {QStringLiteral("exitCode"), 2},
                    {QStringLiteral("error"), tr("Rectangle lies outside monitor %1 (%2×%3).")
                                                 .arg(it->connector)
                                                 .arg(it->geometry.width())
                                                 .arg(it->geometry.height())}};
        }
        Area &area = store_.addArea(*it, rect);
        if (!trimmedName.isEmpty()) {
            area.name = trimmedName;
        }
        createdId = area.id;
    } else {
        QRect bounds;
        for (const ScreenInfo &s : screens) {
            bounds = bounds.united(s.geometry);
        }
        if (intersectingScreens(screens, rect).isEmpty()) {
            return {{QStringLiteral("ok"), false}, {QStringLiteral("exitCode"), 2},
                    {QStringLiteral("error"), tr("Global rectangle does not intersect any connected monitor.")}};
        }
        Area &area = store_.addAreaForGlobalRect(screens, rect);
        if (!trimmedName.isEmpty()) {
            area.name = trimmedName;
        }
        createdId = area.id;
    }

    storeChanged();
    const Area *saved = store_.area(createdId);
    return {
        {QStringLiteral("ok"), true},
        {QStringLiteral("targetId"), createdId},
        {QStringLiteral("name"), saved ? saved->name : trimmedName},
    };
}

QString App::openOverlay(const QString &mode, int delaySeconds)
{
    const QString normMode = mode.trimmed().toLower();
    std::optional<bool> forcedFrozen;
    if (normMode == QLatin1String("frozen")) {
        forcedFrozen = true;
    } else if (normMode == QLatin1String("live")) {
        forcedFrozen = false;
    } else if (!normMode.isEmpty()) {
        return tr("Invalid overlay mode: %1").arg(mode);
    }

    const int delay = std::clamp(delaySeconds, 0, 60);
    if (delay > 0) {
        if (countdown_->isActive()) {
            return tr("A capture countdown is already running.");
        }
        runWithDelay(delay, tr("Screen"), [this, forcedFrozen] { startPrintNow(forcedFrozen, true); });
        return {};
    }
    startPrintNow(forcedFrozen, true);
    return {};
}

QJsonObject App::lastCaptureInfo() const
{
    QJsonArray filesArr;
    for (const QString &path : lastPaths_) {
        const QFileInfo fi(path);
        const QSize sz = fi.exists() ? QImageReader(path).size() : QSize();
        const std::optional<ImageFormat> fmt = imageFormatFromPath(path);
        filesArr.append(QJsonObject{
            {QStringLiteral("path"), path},
            {QStringLiteral("exists"), fi.exists()},
            {QStringLiteral("width"), sz.isValid() ? sz.width() : 0},
            {QStringLiteral("height"), sz.isValid() ? sz.height() : 0},
            {QStringLiteral("format"), fmt ? formatConfigKey(*fmt) : fi.suffix().toLower()},
            {QStringLiteral("bytes"), fi.exists() ? fi.size() : 0},
        });
    }
    return {
        {QStringLiteral("hasLastCapture"), !lastPaths_.isEmpty()},
        {QStringLiteral("targetId"), lastCaptureTargetId_},
        {QStringLiteral("displayName"), lastCaptureDisplayName_},
        {QStringLiteral("timestamp"),
         lastCaptureTimestamp_.isValid() ? lastCaptureTimestamp_.toString(Qt::ISODate) : QString()},
        {QStringLiteral("files"), filesArr},
    };
}

void App::captureAsync(const CliCaptureRequest &request, std::function<void(const CliCaptureResult &)> callback)
{
    const auto fail = [&](int exitCode, const QString &message) {
        CliCaptureResult res;
        res.ok = false;
        res.exitCode = exitCode;
        res.error = message;
        if (callback) {
            callback(res);
        }
    };

    const auto applyOverrides = [&](ExportOptions &opts) {
        if (request.format) {
            opts.format = *request.format;
        } else if (!request.outPath.isEmpty()) {
            if (const std::optional<ImageFormat> inferred = imageFormatFromPath(request.outPath);
                inferred && isImageFormatSupported(*inferred)) {
                opts.format = *inferred;
            }
        }
        if (request.quality) {
            opts.quality = std::clamp(*request.quality, 1, 100);
        }
        if (request.pngCompression) {
            opts.pngCompression = std::clamp(*request.pngCompression, 0, 9);
        }
        if (request.lossless) {
            opts.lossless = *request.lossless;
        }
        if (!isImageFormatSupported(opts.format)) {
            opts.format = ImageFormat::Png;
        }
    };

    const QList<ScreenInfo> screens = currentScreens();
    bool isWindowCapture = false;
    WindowInfo targetWindow;
    TriggerPlan plan;
    QString resolvedTargetId;

    const QString targetTrimmed = request.target.trimmed();
    if (targetTrimmed.startsWith(QLatin1String("window:"), Qt::CaseInsensitive)) {
        const QString winQuery = targetTrimmed.mid(7).trimmed();
        if (winQuery.isEmpty()) {
            fail(2, tr("Window specifier after 'window:' cannot be empty."));
            return;
        }
        const QList<WindowInfo> windows = listWindows(QCoreApplication::applicationPid());
        QList<WindowInfo> matches;
        for (const WindowInfo &w : windows) {
            if (w.handle == winQuery) {
                matches = {w};
                break;
            }
            if (w.appName.compare(winQuery, Qt::CaseInsensitive) == 0
                || w.caption.compare(winQuery, Qt::CaseInsensitive) == 0) {
                matches.append(w);
            }
        }
        if (matches.isEmpty()) {
            for (const WindowInfo &w : windows) {
                if (w.appName.contains(winQuery, Qt::CaseInsensitive)
                    || w.caption.contains(winQuery, Qt::CaseInsensitive)) {
                    matches.append(w);
                }
            }
        }
        if (matches.isEmpty()) {
            fail(3, tr("No open window matches “%1”.").arg(winQuery));
            return;
        }
        if (matches.size() > 1) {
            fail(3, tr("Window query “%1” matches multiple open windows.").arg(winQuery));
            return;
        }
        isWindowCapture = true;
        targetWindow = matches.first();
        resolvedTargetId = QStringLiteral("window:") + targetWindow.handle;
        plan.triggerId = resolvedTargetId;
        plan.displayName = targetWindow.caption.isEmpty() ? windowToken(targetWindow) : targetWindow.caption;
        plan.includeCursor = request.cursor.value_or(false);
    } else if (targetTrimmed.startsWith(QLatin1String("region:"), Qt::CaseInsensitive)) {
        const QString body = targetTrimmed.mid(7);
        const qsizetype sep = body.lastIndexOf(QLatin1Char(':'));
        if (sep <= 0) {
            fail(2, tr("Invalid region specification (expected region:<monitor>:<x>,<y>,<w>,<h>)."));
            return;
        }
        const QString connQuery = body.left(sep).trimmed();
        const QStringList nums = body.mid(sep + 1).split(QLatin1Char(','));
        if (nums.size() != 4) {
            fail(2, tr("Invalid region geometry (expected <x>,<y>,<w>,<h>)."));
            return;
        }
        int coords[4]{};
        for (int i = 0; i < 4; ++i) {
            bool ok = false;
            coords[i] = nums.at(i).trimmed().toInt(&ok);
            if (!ok) {
                fail(2, tr("Invalid region coordinate value."));
                return;
            }
        }
        const QRect rect(coords[0], coords[1], coords[2], coords[3]);
        if (rect.width() <= 0 || rect.height() <= 0) {
            fail(2, tr("Region width and height must be positive."));
            return;
        }
        const auto sIt = std::find_if(screens.cbegin(), screens.cend(), [&](const ScreenInfo &s) {
            return s.connector.compare(connQuery, Qt::CaseInsensitive) == 0
                || store_.monitorName(s).compare(connQuery, Qt::CaseInsensitive) == 0;
        });
        if (sIt == screens.cend()) {
            fail(3, tr("Monitor “%1” is not connected.").arg(connQuery));
            return;
        }
        if (!QRect(QPoint(), sIt->geometry.size()).contains(rect)) {
            fail(2, tr("Region lies outside monitor %1 (%2×%3).")
                        .arg(sIt->connector)
                        .arg(sIt->geometry.width())
                        .arg(sIt->geometry.height()));
            return;
        }
        ExportOptions opts = store_.effectiveExportOptions(screenTargetId(sIt->connector));
        applyOverrides(opts);
        resolvedTargetId = QStringLiteral("region:%1:%2,%3,%4,%5")
                               .arg(sIt->connector)
                               .arg(rect.x())
                               .arg(rect.y())
                               .arg(rect.width())
                               .arg(rect.height());
        plan.triggerId = resolvedTargetId;
        plan.displayName = QStringLiteral("%1-region").arg(store_.monitorName(*sIt));
        plan.connectors = {sIt->connector};
        plan.screens = screens;
        plan.includeCursor = request.cursor.value_or(false);
        plan.outputs.append(
            {OutputSpec::Kind::Area, resolvedTargetId, store_.monitorName(*sIt) + QStringLiteral("-region"),
             sIt->connector, {}, rect, opts});
    } else {
        const CliResolveResult resolved = resolveTarget(targetTrimmed);
        if (!resolved.ok) {
            fail(3, resolved.error);
            return;
        }
        resolvedTargetId = resolved.targetId;
        plan = planTrigger(store_, resolvedTargetId, screens);
        if (plan.outputs.isEmpty()) {
            const QString errMsg = !plan.errors.isEmpty() ? plan.errors.join(QLatin1Char('\n'))
                                                          : (!plan.partialNotifications.isEmpty()
                                                                 ? plan.partialNotifications.join(QLatin1Char('\n'))
                                                                 : tr("Target is not available."));
            fail(3, errMsg);
            return;
        }
        if (plan.outputs.size() > 1 && (!request.outPath.isEmpty() || request.toStdout || request.clipboard)) {
            fail(2, tr("Groups produce multiple images and cannot be written to a single --out path, --stdout, or "
                       "--clipboard."));
            return;
        }
        if (request.cursor) {
            plan.includeCursor = *request.cursor;
        }
        for (OutputSpec &out : plan.outputs) {
            applyOverrides(out.exportOptions);
        }
    }

    ExportOptions windowOpts = store_.exportOptions;
    applyOverrides(windowOpts);

    const auto executeCapture = [this, request, plan, isWindowCapture, targetWindow, windowOpts, resolvedTargetId,
                                 callback] {
        const QDateTime timestamp = QDateTime::currentDateTime();
        const QString directory = store_.effectiveCaptureDirectory();
        const QString filenameTemplate = store_.effectiveFilenameTemplate();

        struct WorkerOutput
        {
            CliCaptureResult cliResult;
            QImage clipboardImage;
            QStringList writtenPaths;
        };

        auto *watcher = new QFutureWatcher<WorkerOutput>(this);
        connect(watcher, &QFutureWatcher<WorkerOutput>::finished, this,
                [this, watcher, request, resolvedTargetId, plan, callback] {
                    pins_->endCapture();
                    WorkerOutput out = watcher->result();
                    watcher->deleteLater();
                    if (out.cliResult.ok && request.clipboard && !out.clipboardImage.isNull()) {
                        QGuiApplication::clipboard()->setImage(out.clipboardImage);
                    }
                    // Files that reached the disk are recorded even when a later group member
                    // failed, so they are not left behind untracked.
                    if (!out.writtenPaths.isEmpty()) {
                        recordCapture(out.writtenPaths, out.cliResult.displayName, resolvedTargetId,
                                      request.triggerKind.isEmpty() ? QStringLiteral("cli") : request.triggerKind);
                        const QString fmt = out.cliResult.files.isEmpty()
                                                ? formatConfigKey(store_.effectiveExportOptions(resolvedTargetId).format)
                                                : out.cliResult.files.first().format;
                        const QString groupId = store_.group(resolvedTargetId) ? resolvedTargetId : QString();
                        const QString trigger =
                            request.triggerKind.isEmpty() ? QStringLiteral("cli") : request.triggerKind;
                        runPostCaptureHook(resolvedTargetId, out.cliResult.displayName, out.writtenPaths,
                                           QDateTime::currentDateTime(), fmt, trigger, false, groupId);
                    } else if (out.cliResult.ok) {
                        tray_->blink();
                        Q_EMIT captured(true);
                    }
                    if (!out.cliResult.ok && out.writtenPaths.isEmpty()) {
                        Q_EMIT captured(false);
                    }
                    if (callback) {
                        callback(out.cliResult);
                    }
                });

        const std::shared_ptr<const OcrEngine> ocrEngine = request.ocr ? ocr_ : nullptr;
        const QStringList ocrLangs = request.ocr ? ocrLanguages() : QStringList();
        // The worker starts once the compositor shows the now transparent pins.
        pins_->beginCapture(watcher, [=] {
            watcher->setFuture(QtConcurrent::run([=] {
                WorkerOutput out;
                out.cliResult.targetId = resolvedTargetId;
                out.cliResult.displayName = plan.displayName;
                out.cliResult.timestamp = timestamp.toString(Qt::ISODate);

                QList<RenderedOutput> outputs;
                if (isWindowCapture) {
                    const CaptureResult cr = CaptureService::captureWindow(targetWindow.handle, plan.includeCursor);
                    if (cr.image.isNull()) {
                        out.cliResult.exitCode = 1;
                        out.cliResult.error = cr.error.isEmpty() ? App::tr("Window capture failed.") : cr.error;
                        return out;
                    }
                    outputs.append({windowToken(targetWindow), cr.image, {}, windowOpts, resolvedTargetId});
                } else {
                    QHash<QString, CaptureResult> frames;
                    std::mutex framesMutex;
                    QtConcurrent::blockingMap(plan.connectors, [&](const QString &connector) {
                        CaptureResult frame = CaptureService::captureScreen(connector, plan.includeCursor);
                        const std::lock_guard lock(framesMutex);
                        frames.insert(connector, std::move(frame));
                    });
                    outputs = renderOutputs(plan, frames);
                }

                const OutputWriter writer(directory, filenameTemplate);
                for (const RenderedOutput &rendered : std::as_const(outputs)) {
                    if (rendered.image.isNull()) {
                        out.cliResult.exitCode = 1;
                        out.cliResult.error = rendered.error;
                        return out;
                    }
                    if (out.clipboardImage.isNull()) {
                        out.clipboardImage = rendered.image;
                    }

                    if (request.noSave) {
                        if (request.toStdout) {
                            QByteArray encoded;
                            QBuffer buf(&encoded);
                            buf.open(QIODevice::WriteOnly);
                            QString encErr;
                            if (!encodeImageToDevice(&buf, rendered.image, rendered.exportOptions, &encErr)) {
                                out.cliResult.exitCode = 1;
                                out.cliResult.error = encErr;
                                return out;
                            }
                            const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
                            const QString tmpBase = runtimeDir.isEmpty() ? QDir::tempPath() : runtimeDir;
                            QTemporaryFile tmp(
                                QDir(tmpBase).filePath(QStringLiteral("itsova-stdout-XXXXXX.%1")
                                                           .arg(canonicalExtension(rendered.exportOptions.format))));
                            tmp.setAutoRemove(false);
                            if (!tmp.open()) {
                                out.cliResult.exitCode = 1;
                                out.cliResult.error = tmp.errorString();
                                return out;
                            }
                            tmp.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                            if (tmp.write(encoded) != encoded.size() || !tmp.flush()) {
                                out.cliResult.exitCode = 1;
                                out.cliResult.error = tmp.errorString();
                                tmp.close();
                                QFile::remove(tmp.fileName());
                                return out;
                            }
                            out.cliResult.stdoutFile = tmp.fileName();
                            out.cliResult.tempStdoutFile = true;
                            tmp.close();
                        }
                        continue;
                    }

                    QString savedPath;
                    QString writeErr;
                    bool wrote = false;
                    if (!request.outPath.isEmpty()) {
                        const QString absOut = QFileInfo(request.outPath).absoluteFilePath();
                        wrote = writeImageToFile(absOut, rendered.image, rendered.exportOptions, &writeErr);
                        savedPath = absOut;
                    } else {
                        wrote = writer.writeImage(rendered.image, rendered.token, timestamp, rendered.exportOptions,
                                                  &savedPath, &writeErr);
                    }
                    if (!wrote) {
                        out.cliResult.exitCode = 1;
                        out.cliResult.error = writeErr;
                        return out;
                    }
                    out.writtenPaths.append(savedPath);
                    out.cliResult.files.append({rendered.targetId, savedPath, rendered.image.width(),
                                                rendered.image.height(),
                                                formatConfigKey(rendered.exportOptions.format), QFileInfo(savedPath).size(),
                                                QString()});
                    if (request.toStdout && out.cliResult.stdoutFile.isEmpty()) {
                        out.cliResult.stdoutFile = savedPath;
                        out.cliResult.tempStdoutFile = false;
                    }
                }

                if (request.ocr) {
                    out.cliResult.ocrRequested = true;
                    if (!ocrEngine || !ocrEngine->available()) {
                        out.cliResult.ocrError = App::tr("Text recognition needs tesseract. Install tesseract and a "
                                                         "language package such as tesseract-data-eng.");
                    } else {
                        for (qsizetype i = 0; i < outputs.size(); ++i) {
                            const OcrResult text = ocrEngine->recognize(outputs.at(i).image, ocrLangs);
                            if (!text.ok) {
                                out.cliResult.ocrError = text.error;
                                break;
                            }
                            out.cliResult.ocrTexts.append({outputs.at(i).token, text.text});
                            if (i < out.cliResult.files.size()) {
                                out.cliResult.files[i].text = text.text;
                            }
                        }
                    }
                }

                out.cliResult.ok = true;
                out.cliResult.exitCode = 0;
                return out;
            }));
        });
    };

    if (request.delaySeconds > 0) {
        if (countdown_->isActive()) {
            fail(1, tr("A capture countdown is already running."));
            return;
        }
        runWithDelay(request.delaySeconds, plan.displayName, executeCapture,
                     [fail] { fail(1, tr("Capture countdown was cancelled.")); });
        return;
    }

    executeCapture();
}

void App::runPostCaptureHook(const QString &targetId, const QString &displayName, const QStringList &files,
                             const QDateTime &timestamp, const QString &format, const QString &triggerKind,
                             bool isInterval, const QString &groupId)
{
    if (files.isEmpty() || !hooks_) {
        return;
    }
    const HookConfig cfg = store_.effectiveHookConfig(targetId, groupId, isInterval);
    if (!cfg.enabled || cfg.command.trimmed().isEmpty()) {
        return;
    }
    HookInvocation inv;
    inv.command = cfg.command;
    inv.timeoutSeconds = cfg.timeoutSeconds;
    inv.files = files;
    inv.targetId = targetId;
    inv.targetName = displayName;
    inv.triggerKind = triggerKind;
    inv.timestamp = timestamp;
    inv.format = format;
    hooks_->run(inv);
}

void App::testPostCaptureHook(const QString &command, int timeoutSeconds,
                              std::function<void(const HookResult &)> onDone)
{
    if (!hooks_) {
        return;
    }
    hooks_->runTest(command, timeoutSeconds, std::move(onDone));
}

void App::editArea(const QString &areaId)
{
    const Area *area = store_.area(areaId);
    if (!area || selector_ || session_) {
        return;
    }
    // An open settings window has to step aside first; it runs the same overlay itself.
    if (auto *window = qobject_cast<SettingsWindow *>(settings_.data()); window && window->isVisible()) {
        window->editArea(areaId);
        return;
    }
    QRect initialGlobal;
    if (area->isCrossMonitor()) {
        initialGlobal = area->rect;
    } else if (const QScreen *screen = findScreen(area->connector)) {
        initialGlobal = area->rect.translated(screen->geometry().topLeft());
    }
    auto *selector = new AreaSelector(this);
    selector_ = selector;
    connect(selector, &AreaSelector::selected, this, [this, selector, areaId](const QRect &globalRect) {
        selector->deleteLater();
        Area *edited = store_.area(areaId);
        if (!edited) {
            return;
        }
        const QList<ScreenInfo> hit = intersectingScreens(currentScreens(), globalRect);
        if (hit.isEmpty()) {
            return;
        }
        if (hit.size() == 1 && hit.first().geometry.contains(globalRect)) {
            edited->connector = hit.first().connector;
            edited->monitorIdentity = hit.first().identity();
            edited->monitors.clear();
            edited->rect = globalRect.translated(-hit.first().geometry.topLeft());
        } else {
            edited->connector.clear();
            edited->monitorIdentity.clear();
            edited->monitors.clear();
            for (const ScreenInfo &s : hit) {
                edited->monitors.append({s.connector, s.identity()});
            }
            edited->rect = globalRect;
        }
        storeChanged();
    });
    connect(selector, &AreaSelector::cancelled, selector, &QObject::deleteLater);
    selector->start(initialGlobal, store_.showSelectionLoupe);
}

void App::applyAutostart()
{
    if (!store_.autostartInitialized) {
        // First start: autostart is on by default (decision), and the choice is written down.
        store_.autostart = true;
        store_.autostartInitialized = true;
        store_.save(*config_);
    }

    const QString fileName = QGuiApplication::desktopFileName() + QStringLiteral(".desktop");
    const QString autostartDir =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/autostart");
    const QString autostartPath = autostartDir + QLatin1Char('/') + fileName;

    if (!store_.autostart) {
        if (QFile::exists(autostartPath) && !QFile::remove(autostartPath)) {
            notify(QStringLiteral("settingsProblem"), tr("Autostart"), {tr("Could not remove %1.").arg(autostartPath)});
        }
        return;
    }

    // Start the executable that is running now, so a moved or reinstalled binary stays consistent.
    QByteArray content;
    const QString installed = QStandardPaths::locate(QStandardPaths::ApplicationsLocation, fileName);
    QFile source(installed);
    if (!installed.isEmpty() && source.open(QIODevice::ReadOnly)) {
        content = source.readAll();
    } else {
        content = QByteArrayLiteral("[Desktop Entry]\nType=Application\nName=Itsova\nExec=@EXEC@\n"
                                    "Icon=io.github.m00sfett.itsova\nStartupNotify=false\n"
                                    "X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2\n");
    }
    QStringList lines = QString::fromUtf8(content).split(QLatin1Char('\n'));
    for (QString &line : lines) {
        if (line.startsWith(QLatin1String("Exec="))) {
            line = QStringLiteral("Exec=") + QCoreApplication::applicationFilePath();
        }
    }
    content = lines.join(QLatin1Char('\n')).toUtf8();

    QFile existing(autostartPath);
    if (existing.open(QIODevice::ReadOnly) && existing.readAll() == content) {
        return;
    }
    existing.close();
    QDir().mkpath(autostartDir);
    QSaveFile file(autostartPath);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit()) {
        notify(QStringLiteral("settingsProblem"), tr("Autostart"), {tr("Could not write %1.").arg(autostartPath)});
    }
}

} // namespace itsova
