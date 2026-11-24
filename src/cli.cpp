#include "cli.h"

#include "ocrengine.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

#include <algorithm>
#include <array>

namespace glimply
{
namespace
{

class CliText
{
    Q_DECLARE_TR_FUNCTIONS(glimply::cli)
};

constexpr auto dbusService = "io.github.m00sfett.glimply";
constexpr auto dbusPath = "/Control";
constexpr auto dbusIface = "io.github.m00sfett.glimply.Control";

void writeLine(QIODevice &dev, const QString &text)
{
    QByteArray utf8 = text.toUtf8();
    if (!utf8.endsWith('\n')) {
        utf8.append('\n');
    }
    dev.write(utf8);
}

void writeJson(QIODevice &dev, const QJsonValue &val)
{
    const QJsonDocument doc = val.isArray() ? QJsonDocument(val.toArray()) : QJsonDocument(val.toObject());
    QByteArray bytes = doc.toJson(QJsonDocument::Indented);
    if (!bytes.endsWith('\n')) {
        bytes.append('\n');
    }
    dev.write(bytes);
}

bool parseIntChecked(const QString &str, int minVal, int maxVal, int *outVal)
{
    bool ok = false;
    const int val = str.toInt(&ok);
    if (!ok || val < minVal || val > maxVal) {
        return false;
    }
    *outVal = val;
    return true;
}

bool parseRectString(const QString &str, QRect *outRect)
{
    const QStringList parts = str.split(QLatin1Char(','));
    if (parts.size() != 4) {
        return false;
    }
    int vals[4]{};
    for (int i = 0; i < 4; ++i) {
        bool ok = false;
        vals[i] = parts.at(i).trimmed().toInt(&ok);
        if (!ok) {
            return false;
        }
    }
    if (vals[2] <= 0 || vals[3] <= 0) {
        return false;
    }
    *outRect = QRect(vals[0], vals[1], vals[2], vals[3]);
    return true;
}

QDBusMessage callControl(const QString &method, const QVariantList &args = {}, int timeoutMs = 30'000,
                         QString *error = nullptr)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        if (error) {
            *error = CliText::tr("Could not connect to the D-Bus session bus.");
        }
        return {};
    }
    QDBusMessage msg = QDBusMessage::createMethodCall(QLatin1String(dbusService), QLatin1String(dbusPath),
                                                      QLatin1String(dbusIface), method);
    msg.setArguments(args);
    const QDBusMessage reply = bus.call(msg, QDBus::Block, timeoutMs);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (error) {
            *error = reply.errorMessage().isEmpty() ? reply.errorName() : reply.errorMessage();
        }
        return {};
    }
    if (reply.type() != QDBusMessage::ReplyMessage) {
        if (error) {
            *error = CliText::tr("Unexpected D-Bus reply calling %1.").arg(method);
        }
        return {};
    }
    return reply;
}

bool parseJsonDoc(const QByteArray &raw, QJsonDocument *doc, QString *error)
{
    QJsonParseError parseErr{};
    *doc = QJsonDocument::fromJson(raw, &parseErr);
    if (parseErr.error != QJsonParseError::NoError) {
        if (error) {
            *error = CliText::tr("Invalid JSON reply from glimply daemon: %1").arg(parseErr.errorString());
        }
        return false;
    }
    return true;
}

int emitError(bool jsonMode, int exitCode, const QString &message, const QJsonObject &extra, QIODevice &out,
              QIODevice &err)
{
    if (jsonMode) {
        QJsonObject obj = extra;
        obj.insert(QStringLiteral("ok"), false);
        obj.insert(QStringLiteral("exitCode"), exitCode);
        obj.insert(QStringLiteral("error"), message);
        writeJson(out, obj);
    } else {
        writeLine(err, message);
    }
    return exitCode;
}

void printSubcommandHelp(const QString &subcommand, QIODevice &out)
{
    if (subcommand == QLatin1String("capture")) {
        writeLine(out,
                  QStringLiteral("Usage: glimply capture <target|all|monitor:...|window:...> [options]\n\n"
                                 "Options:\n"
                                 "  --format <png|jpeg|webp|avif|jxl>  Output image format\n"
                                 "  --quality <1..100>                  Lossy encoder quality\n"
                                 "  --png-compression <0..9>            PNG zlib compression level\n"
                                 "  --lossless                          Use lossless WebP encoding\n"
                                 "  --delay <0..60>                     Countdown seconds before capture\n"
                                 "  --cursor / --no-cursor              Include/exclude mouse pointer\n"
                                 "  --out <path>                        Write image to an explicit file path\n"
                                 "  --stdout                            Stream raw encoded image bytes to stdout\n"
                                 "  --clipboard                         Copy captured image to clipboard\n"
                                 "  --no-save                           Do not save into the capture directory\n"
                                 "  --ocr                               Print recognized text (needs tesseract)\n"
                                 "  --json                              Print JSON metadata\n"
                                 "  -h, --help                          Show this help"));
    } else if (subcommand == QLatin1String("list")) {
        writeLine(out,
                  QStringLiteral("Usage: glimply list [--kind targets|monitors|windows] [--json]\n\n"
                                 "Options:\n"
                                 "  --kind <targets|monitors|windows>   What to list (default: targets)\n"
                                 "  --json                              Output compact/pretty JSON\n"
                                 "  -h, --help                          Show this help"));
    } else if (subcommand == QLatin1String("area")) {
        writeLine(out,
                  QStringLiteral("Usage: glimply area <subcommand> [options]\n\n"
                                 "Subcommands:\n"
                                 "  add [--name NAME] [--monitor CONNECTOR] [--rect X,Y,W,H] [--json]\n"
                                 "  edit <id-or-name>\n"
                                 "  rename <id-or-name> <new-name> [--json]\n"
                                 "  remove <id-or-name> [--json]"));
    } else if (subcommand == QLatin1String("interval")) {
        writeLine(out,
                  QStringLiteral("Usage: glimply interval <subcommand> [options]\n\n"
                                 "Subcommands:\n"
                                 "  start <id-or-name> [--seconds N] [--max-captures N] [--max-minutes N] [--only-changed] [--json]\n"
                                 "  stop [<id-or-name> | --all] [--json]\n"
                                 "  status [--json]"));
    } else if (subcommand == QLatin1String("history")) {
        writeLine(out,
                  QStringLiteral("Usage: glimply history [--limit N] [--json]\n\n"
                                 "Lists recent captures, newest first (interval captures are not recorded).\n\n"
                                 "Options:\n"
                                 "  --limit <N>                         Show at most N captures\n"
                                 "  --json                              Output JSON\n"
                                 "  -h, --help                          Show this help"));
    } else if (subcommand == QLatin1String("overlay")) {
        writeLine(out,
                  QStringLiteral("Usage: glimply overlay [--mode frozen|live] [--delay N]\n\n"
                                 "Options:\n"
                                 "  --mode <frozen|live>                Overlay capture mode override\n"
                                 "  --delay <0..60>                     Countdown seconds before opening\n"
                                 "  -h, --help                          Show this help"));
    }
}

int handleCapture(const QStringList &args, CliBackend &backend, QIODevice &out, QIODevice &err)
{
    CliCaptureRequest req;
    bool jsonMode = false;
    QStringList positionals;

    for (int i = 0; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        if (arg == QLatin1String("-h") || arg == QLatin1String("--help")) {
            printSubcommandHelp(QStringLiteral("capture"), out);
            return 0;
        }
        const auto nextVal = [&]() -> QString {
            ++i;
            return args.at(i);
        };
        if (arg == QLatin1String("--format")) {
            if (i + 1 >= args.size()) {
                return emitError(jsonMode, 2, CliText::tr("Missing value for --format."), {}, out, err);
            }
            const QString val = nextVal();
            const std::optional<ImageFormat> parsed = imageFormatFromKey(val);
            if (!parsed || !isImageFormatSupported(*parsed)) {
                return emitError(jsonMode, 2, CliText::tr("Unsupported image format: %1").arg(val), {}, out, err);
            }
            req.format = *parsed;
        } else if (arg == QLatin1String("--quality")) {
            if (i + 1 >= args.size()) {
                return emitError(jsonMode, 2, CliText::tr("Missing value for --quality."), {}, out, err);
            }
            int q = 0;
            if (!parseIntChecked(nextVal(), 1, 100, &q)) {
                return emitError(jsonMode, 2, CliText::tr("Quality must be an integer between 1 and 100."), {}, out, err);
            }
            req.quality = q;
        } else if (arg == QLatin1String("--png-compression")) {
            if (i + 1 >= args.size()) {
                return emitError(jsonMode, 2, CliText::tr("Missing value for --png-compression."), {}, out, err);
            }
            int c = 0;
            if (!parseIntChecked(nextVal(), 0, 9, &c)) {
                return emitError(jsonMode, 2, CliText::tr("PNG compression must be between 0 and 9."), {}, out, err);
            }
            req.pngCompression = c;
        } else if (arg == QLatin1String("--lossless")) {
            req.lossless = true;
        } else if (arg == QLatin1String("--delay")) {
            if (i + 1 >= args.size()) {
                return emitError(jsonMode, 2, CliText::tr("Missing value for --delay."), {}, out, err);
            }
            int d = 0;
            if (!parseIntChecked(nextVal(), 0, 60, &d)) {
                return emitError(jsonMode, 2, CliText::tr("Delay must be between 0 and 60 seconds."), {}, out, err);
            }
            req.delaySeconds = d;
        } else if (arg == QLatin1String("--cursor")) {
            req.cursor = true;
        } else if (arg == QLatin1String("--no-cursor")) {
            req.cursor = false;
        } else if (arg == QLatin1String("--out")) {
            if (i + 1 >= args.size()) {
                return emitError(jsonMode, 2, CliText::tr("Missing path for --out."), {}, out, err);
            }
            // The daemon has its own working directory, so resolve relative paths here.
            req.outPath = QFileInfo(nextVal()).absoluteFilePath();
        } else if (arg == QLatin1String("--stdout")) {
            req.toStdout = true;
        } else if (arg == QLatin1String("--clipboard")) {
            req.clipboard = true;
        } else if (arg == QLatin1String("--no-save")) {
            req.noSave = true;
        } else if (arg == QLatin1String("--ocr")) {
            req.ocr = true;
        } else if (arg == QLatin1String("--json")) {
            jsonMode = true;
        } else if (arg.startsWith(QLatin1Char('-'))) {
            return emitError(jsonMode, 2, CliText::tr("Unknown option: %1").arg(arg), {}, out, err);
        } else {
            positionals.append(arg);
        }
    }

    if (req.toStdout && jsonMode) {
        writeLine(err, CliText::tr("--stdout and --json cannot be used together."));
        return 2;
    }
    if (req.toStdout && req.ocr) {
        writeLine(err, CliText::tr("--stdout and --ocr cannot be used together."));
        return 2;
    }
    if (positionals.size() != 1) {
        return emitError(jsonMode, 2, CliText::tr("Expected exactly one capture target."), {}, out, err);
    }
    req.target = positionals.first();

    const CliCaptureResult result = backend.capture(req);
    if (!result.ok) {
        if (result.tempStdoutFile && !result.stdoutFile.isEmpty()) {
            QFile::remove(result.stdoutFile);
        }
        // A group can fail after some members were already saved; still report those files.
        QJsonObject extra;
        if (!result.files.isEmpty()) {
            extra.insert(QStringLiteral("files"), captureResultToJson(result).value(QStringLiteral("files")));
            if (!jsonMode) {
                for (const CliCaptureFile &file : result.files) {
                    writeLine(out, file.path);
                }
            }
        }
        return emitError(jsonMode, result.exitCode > 0 ? result.exitCode : 1,
                         result.error.isEmpty() ? CliText::tr("Capture failed.") : result.error, extra, out, err);
    }

    if (req.toStdout) {
        const QString sourcePath = !result.stdoutFile.isEmpty()
                                       ? result.stdoutFile
                                       : (!result.files.isEmpty() ? result.files.first().path : QString());
        QFile f(sourcePath);
        if (sourcePath.isEmpty() || !f.open(QIODevice::ReadOnly)) {
            if (result.tempStdoutFile && !result.stdoutFile.isEmpty()) {
                QFile::remove(result.stdoutFile);
            }
            writeLine(err, CliText::tr("Could not read captured image bytes for --stdout."));
            return 1;
        }
        std::array<char, 64 * 1024> buf{};
        while (!f.atEnd()) {
            const qint64 n = f.read(buf.data(), static_cast<qint64>(buf.size()));
            if (n > 0) {
                out.write(buf.data(), n);
            } else if (n < 0) {
                f.close();
                if (result.tempStdoutFile) {
                    QFile::remove(sourcePath);
                }
                writeLine(err, CliText::tr("Failed reading image stream."));
                return 1;
            }
        }
        f.close();
        if (result.tempStdoutFile) {
            QFile::remove(sourcePath);
        }
        return 0;
    }

    if (jsonMode) {
        QJsonObject obj = captureResultToJson(result);
        obj.remove(QStringLiteral("stdoutFile"));
        obj.remove(QStringLiteral("tempStdoutFile"));
        writeJson(out, obj);
        return req.ocr && !result.ocrError.isEmpty() ? 1 : 0;
    }

    if (req.ocr) {
        if (!result.ocrError.isEmpty()) {
            // The capture is saved; say where, then report why there is no text.
            for (const CliCaptureFile &file : result.files) {
                writeLine(out, file.path);
            }
            writeLine(err, result.ocrError);
            return 1;
        }
        if (result.ocrTexts.size() == 1) {
            if (!result.ocrTexts.first().text.isEmpty()) {
                writeLine(out, result.ocrTexts.first().text);
            }
        } else {
            QStringList blocks;
            for (const CliOcrText &t : result.ocrTexts) {
                blocks.append(QStringLiteral("== %1 ==\n%2").arg(t.label, t.text));
            }
            writeLine(out, blocks.join(QStringLiteral("\n\n")));
        }
        return 0;
    }

    for (const CliCaptureFile &file : result.files) {
        writeLine(out, file.path);
    }
    if (result.files.isEmpty()) {
        if (req.clipboard) {
            writeLine(out, CliText::tr("Copied %1 to clipboard.").arg(result.displayName));
        } else {
            writeLine(out, CliText::tr("Captured %1.").arg(result.displayName));
        }
    }
    return 0;
}

int handleList(const QStringList &args, CliBackend &backend, QIODevice &out, QIODevice &err)
{
    QString kind = QStringLiteral("targets");
    bool jsonMode = false;

    for (int i = 0; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        if (arg == QLatin1String("-h") || arg == QLatin1String("--help")) {
            printSubcommandHelp(QStringLiteral("list"), out);
            return 0;
        }
        if (arg == QLatin1String("--kind")) {
            if (i + 1 >= args.size()) {
                return emitError(jsonMode, 2, CliText::tr("Missing value for --kind."), {}, out, err);
            }
            kind = args.at(++i).trimmed().toLower();
        } else if (arg == QLatin1String("--json")) {
            jsonMode = true;
        } else {
            return emitError(jsonMode, 2, CliText::tr("Unknown option: %1").arg(arg), {}, out, err);
        }
    }

    QJsonArray items;
    CliSimpleResult res;
    if (kind == QLatin1String("targets")) {
        res = backend.listTargets(&items);
    } else if (kind == QLatin1String("monitors")) {
        res = backend.listMonitors(&items);
    } else if (kind == QLatin1String("windows")) {
        res = backend.listWindows(&items);
    } else {
        return emitError(jsonMode, 2, CliText::tr("Invalid --kind value: %1 (expected targets, monitors, or windows).").arg(kind),
                         {}, out, err);
    }

    if (!res.ok) {
        return emitError(jsonMode, res.exitCode, res.error, {}, out, err);
    }
    if (jsonMode) {
        writeJson(out, items);
        return 0;
    }

    for (const QJsonValue &val : std::as_const(items)) {
        const QJsonObject obj = val.toObject();
        if (kind == QLatin1String("targets")) {
            const QString id = obj.value(QStringLiteral("targetId")).toString();
            const QString tKind = obj.value(QStringLiteral("kind")).toString();
            const QString name = obj.value(QStringLiteral("name")).toString();
            const QString detail = obj.value(QStringLiteral("detail")).toString();
            const bool available = obj.value(QStringLiteral("available")).toBool(true);
            const QString availStr = available ? QString() : QStringLiteral(" [unavailable]");
            writeLine(out, detail.isEmpty() ? QStringLiteral("%1\t%2\t%3%4").arg(tKind, name, id, availStr)
                                            : QStringLiteral("%1\t%2\t%3\t%4%5").arg(tKind, name, detail, id, availStr));
        } else if (kind == QLatin1String("monitors")) {
            const QString conn = obj.value(QStringLiteral("connector")).toString();
            const QString name = obj.value(QStringLiteral("name")).toString();
            const int w = obj.value(QStringLiteral("width")).toInt();
            const int h = obj.value(QStringLiteral("height")).toInt();
            const int x = obj.value(QStringLiteral("x")).toInt();
            const int y = obj.value(QStringLiteral("y")).toInt();
            writeLine(out, QStringLiteral("%1\t%2\t%3x%4+%5+%6").arg(conn, name).arg(w).arg(h).arg(x).arg(y));
        } else {
            const QString handle = obj.value(QStringLiteral("handle")).toString();
            const QString app = obj.value(QStringLiteral("appName")).toString();
            const QString cap = obj.value(QStringLiteral("caption")).toString();
            writeLine(out, QStringLiteral("%1\t%2\t%3").arg(handle, app, cap));
        }
    }
    return 0;
}

int handleArea(const QStringList &args, CliBackend &backend, QIODevice &out, QIODevice &err)
{
    if (args.isEmpty() || args.first() == QLatin1String("-h") || args.first() == QLatin1String("--help")) {
        printSubcommandHelp(QStringLiteral("area"), out);
        return args.isEmpty() ? 2 : 0;
    }

    const QString action = args.first();
    const QStringList rest = args.mid(1);
    bool jsonMode = false;

    if (action == QLatin1String("add")) {
        QString name;
        QString connector;
        QRect rect;
        bool hasRect = false;
        for (int i = 0; i < rest.size(); ++i) {
            const QString &arg = rest.at(i);
            if (arg == QLatin1String("-h") || arg == QLatin1String("--help")) {
                printSubcommandHelp(QStringLiteral("area"), out);
                return 0;
            }
            if (arg == QLatin1String("--name")) {
                if (i + 1 >= rest.size()) {
                    return emitError(jsonMode, 2, CliText::tr("Missing value for --name."), {}, out, err);
                }
                name = rest.at(++i);
            } else if (arg == QLatin1String("--monitor")) {
                if (i + 1 >= rest.size()) {
                    return emitError(jsonMode, 2, CliText::tr("Missing value for --monitor."), {}, out, err);
                }
                connector = rest.at(++i);
            } else if (arg == QLatin1String("--rect")) {
                if (i + 1 >= rest.size()) {
                    return emitError(jsonMode, 2, CliText::tr("Missing value for --rect."), {}, out, err);
                }
                const QString rectStr = rest.at(++i);
                if (!parseRectString(rectStr, &rect)) {
                    return emitError(jsonMode, 2, CliText::tr("Invalid rectangle %1 (expected X,Y,W,H with W>0, H>0).").arg(rectStr),
                                     {}, out, err);
                }
                hasRect = true;
            } else if (arg == QLatin1String("--json")) {
                jsonMode = true;
            } else {
                return emitError(jsonMode, 2, CliText::tr("Unknown option: %1").arg(arg), {}, out, err);
            }
        }
        if (!hasRect) {
            const CliSimpleResult res = backend.addAreaInteractive();
            if (!res.ok) {
                return emitError(jsonMode, res.exitCode, res.error, {}, out, err);
            }
            if (jsonMode) {
                QJsonObject obj;
                obj.insert(QStringLiteral("ok"), true);
                obj.insert(QStringLiteral("interactive"), true);
                writeJson(out, obj);
            } else {
                writeLine(out, CliText::tr("Opened interactive area selector."));
            }
            return 0;
        }
        const CliSimpleResult res = backend.addAreaRect(name, connector, rect);
        if (!res.ok) {
            return emitError(jsonMode, res.exitCode, res.error, {}, out, err);
        }
        if (jsonMode) {
            QJsonObject obj = res.data;
            obj.insert(QStringLiteral("ok"), true);
            writeJson(out, obj);
        } else {
            writeLine(out, QStringLiteral("%1\t%2").arg(res.data.value(QStringLiteral("name")).toString(),
                                                        res.data.value(QStringLiteral("targetId")).toString()));
        }
        return 0;
    }

    if (action == QLatin1String("edit")) {
        QStringList pos;
        for (const QString &arg : rest) {
            if (arg == QLatin1String("-h") || arg == QLatin1String("--help")) {
                printSubcommandHelp(QStringLiteral("area"), out);
                return 0;
            }
            if (arg.startsWith(QLatin1Char('-'))) {
                return emitError(false, 2, CliText::tr("Unknown option: %1").arg(arg), {}, out, err);
            }
            pos.append(arg);
        }
        if (pos.size() != 1) {
            return emitError(false, 2, CliText::tr("Expected an area ID or name."), {}, out, err);
        }
        const CliResolveResult resolved = backend.resolveTarget(pos.first());
        if (!resolved.ok) {
            return emitError(false, 3, resolved.error, {}, out, err);
        }
        if (resolved.kind != QLatin1String("area")) {
            return emitError(false, 2, CliText::tr("“%1” is not an area.").arg(pos.first()), {}, out, err);
        }
        const CliSimpleResult res = backend.editArea(resolved.targetId);
        if (!res.ok) {
            return emitError(false, res.exitCode, res.error, {}, out, err);
        }
        writeLine(out, CliText::tr("Opened editor for area %1.").arg(resolved.name));
        return 0;
    }

    if (action == QLatin1String("rename")) {
        QStringList pos;
        for (const QString &arg : rest) {
            if (arg == QLatin1String("-h") || arg == QLatin1String("--help")) {
                printSubcommandHelp(QStringLiteral("area"), out);
                return 0;
            }
            if (arg == QLatin1String("--json")) {
                jsonMode = true;
            } else if (arg.startsWith(QLatin1Char('-'))) {
                return emitError(jsonMode, 2, CliText::tr("Unknown option: %1").arg(arg), {}, out, err);
            } else {
                pos.append(arg);
            }
        }
        if (pos.size() != 2) {
            return emitError(jsonMode, 2, CliText::tr("Usage: glimply area rename <id-or-name> <new-name> [--json]"),
                             {}, out, err);
        }
        const CliResolveResult resolved = backend.resolveTarget(pos.at(0));
        if (!resolved.ok) {
            return emitError(jsonMode, 3, resolved.error, {}, out, err);
        }
        const CliSimpleResult res = backend.renameTarget(resolved.targetId, pos.at(1));
        if (!res.ok) {
            return emitError(jsonMode, res.exitCode, res.error, {}, out, err);
        }
        if (jsonMode) {
            writeJson(out, QJsonObject{{QStringLiteral("ok"), true},
                                       {QStringLiteral("targetId"), resolved.targetId},
                                       {QStringLiteral("name"), pos.at(1).trimmed()}});
        } else {
            writeLine(out, CliText::tr("Renamed %1 to %2.").arg(resolved.name, pos.at(1).trimmed()));
        }
        return 0;
    }

    if (action == QLatin1String("remove")) {
        QStringList pos;
        for (const QString &arg : rest) {
            if (arg == QLatin1String("-h") || arg == QLatin1String("--help")) {
                printSubcommandHelp(QStringLiteral("area"), out);
                return 0;
            }
            if (arg == QLatin1String("--json")) {
                jsonMode = true;
            } else if (arg.startsWith(QLatin1Char('-'))) {
                return emitError(jsonMode, 2, CliText::tr("Unknown option: %1").arg(arg), {}, out, err);
            } else {
                pos.append(arg);
            }
        }
        if (pos.size() != 1) {
            return emitError(jsonMode, 2, CliText::tr("Usage: glimply area remove <id-or-name> [--json]"), {}, out, err);
        }
        const CliResolveResult resolved = backend.resolveTarget(pos.first());
        if (!resolved.ok) {
            return emitError(jsonMode, 3, resolved.error, {}, out, err);
        }
        if (resolved.kind != QLatin1String("area") && resolved.kind != QLatin1String("group")) {
            return emitError(jsonMode, 2, CliText::tr("Built-in targets cannot be removed."), {}, out, err);
        }
        const CliSimpleResult res = backend.removeTarget(resolved.targetId);
        if (!res.ok) {
            return emitError(jsonMode, res.exitCode, res.error, {}, out, err);
        }
        if (jsonMode) {
            writeJson(out, QJsonObject{{QStringLiteral("ok"), true},
                                       {QStringLiteral("removedTargetId"), resolved.targetId}});
        } else {
            writeLine(out, CliText::tr("Removed %1.").arg(resolved.name));
        }
        return 0;
    }

    return emitError(false, 2, CliText::tr("Unknown area subcommand: %1").arg(action), {}, out, err);
}

int handleInterval(const QStringList &args, CliBackend &backend, QIODevice &out, QIODevice &err)
{
    if (args.isEmpty() || args.first() == QLatin1String("-h") || args.first() == QLatin1String("--help")) {
        printSubcommandHelp(QStringLiteral("interval"), out);
        return args.isEmpty() ? 2 : 0;
    }

    const QString action = args.first();
    const QStringList rest = args.mid(1);
    bool jsonMode = false;

    if (action == QLatin1String("start")) {
        std::optional<int> seconds;
        std::optional<int> maxCaptures;
        std::optional<int> maxMinutes;
        std::optional<bool> onlyChanged;
        QStringList pos;
        for (int i = 0; i < rest.size(); ++i) {
            const QString &arg = rest.at(i);
            if (arg == QLatin1String("-h") || arg == QLatin1String("--help")) {
                printSubcommandHelp(QStringLiteral("interval"), out);
                return 0;
            }
            if (arg == QLatin1String("--seconds")) {
                int v = 0;
                if (i + 1 >= rest.size() || !parseIntChecked(rest.at(++i), 1, 86400, &v)) {
                    return emitError(jsonMode, 2, CliText::tr("--seconds must be between 1 and 86400."), {}, out, err);
                }
                seconds = v;
            } else if (arg == QLatin1String("--max-captures")) {
                int v = 0;
                if (i + 1 >= rest.size() || !parseIntChecked(rest.at(++i), 0, 100000, &v)) {
                    return emitError(jsonMode, 2, CliText::tr("--max-captures must be between 0 and 100000."), {}, out,
                                     err);
                }
                maxCaptures = v;
            } else if (arg == QLatin1String("--max-minutes")) {
                int v = 0;
                if (i + 1 >= rest.size() || !parseIntChecked(rest.at(++i), 0, 10080, &v)) {
                    return emitError(jsonMode, 2, CliText::tr("--max-minutes must be between 0 and 10080."), {}, out,
                                     err);
                }
                maxMinutes = v;
            } else if (arg == QLatin1String("--only-changed")) {
                onlyChanged = true;
            } else if (arg == QLatin1String("--json")) {
                jsonMode = true;
            } else if (arg.startsWith(QLatin1Char('-'))) {
                return emitError(jsonMode, 2, CliText::tr("Unknown option: %1").arg(arg), {}, out, err);
            } else {
                pos.append(arg);
            }
        }
        if (pos.size() != 1) {
            return emitError(jsonMode, 2, CliText::tr("Expected exactly one target to start an interval for."), {}, out,
                             err);
        }
        const CliResolveResult resolved = backend.resolveTarget(pos.first());
        if (!resolved.ok) {
            return emitError(jsonMode, 3, resolved.error, {}, out, err);
        }

        int finalSeconds = 0;
        int finalMaxCap = 0;
        int finalMaxMin = 0;
        bool finalOnlyChanged = false;
        if (seconds || maxCaptures || maxMinutes || onlyChanged) {
            QJsonArray targets;
            if (backend.listTargets(&targets).ok) {
                for (const QJsonValue &v : std::as_const(targets)) {
                    const QJsonObject t = v.toObject();
                    if (t.value(QStringLiteral("targetId")).toString() == resolved.targetId) {
                        finalSeconds = t.value(QStringLiteral("intervalSeconds")).toInt(5);
                        finalMaxCap = t.value(QStringLiteral("intervalMaxCaptures")).toInt(0);
                        finalMaxMin = t.value(QStringLiteral("intervalMaxMinutes")).toInt(0);
                        finalOnlyChanged = t.value(QStringLiteral("intervalOnlyWhenChanged")).toBool(false);
                        break;
                    }
                }
            }
            if (finalSeconds <= 0) {
                finalSeconds = 5;
            }
            if (seconds) {
                finalSeconds = *seconds;
            }
            if (maxCaptures) {
                finalMaxCap = *maxCaptures;
            }
            if (maxMinutes) {
                finalMaxMin = *maxMinutes;
            }
            if (onlyChanged) {
                finalOnlyChanged = *onlyChanged;
            }
        }

        const CliSimpleResult res =
            backend.startInterval(resolved.targetId, finalSeconds, finalMaxCap, finalMaxMin, finalOnlyChanged);
        if (!res.ok) {
            return emitError(jsonMode, res.exitCode, res.error, {}, out, err);
        }
        if (jsonMode) {
            writeJson(out, QJsonObject{{QStringLiteral("ok"), true},
                                       {QStringLiteral("targetId"), resolved.targetId},
                                       {QStringLiteral("name"), resolved.name}});
        } else {
            writeLine(out, CliText::tr("Started interval capture for %1.").arg(resolved.name));
        }
        return 0;
    }

    if (action == QLatin1String("stop")) {
        bool stopAll = false;
        QStringList pos;
        for (const QString &arg : rest) {
            if (arg == QLatin1String("-h") || arg == QLatin1String("--help")) {
                printSubcommandHelp(QStringLiteral("interval"), out);
                return 0;
            }
            if (arg == QLatin1String("--all")) {
                stopAll = true;
            } else if (arg == QLatin1String("--json")) {
                jsonMode = true;
            } else if (arg.startsWith(QLatin1Char('-'))) {
                return emitError(jsonMode, 2, CliText::tr("Unknown option: %1").arg(arg), {}, out, err);
            } else {
                pos.append(arg);
            }
        }
        if (stopAll && !pos.isEmpty()) {
            return emitError(jsonMode, 2, CliText::tr("Do not pass a target name together with --all."), {}, out, err);
        }
        if (stopAll || pos.isEmpty()) {
            if (!stopAll && pos.isEmpty()) {
                return emitError(jsonMode, 2, CliText::tr("Specify a target or --all."), {}, out, err);
            }
            const CliSimpleResult res = backend.stopAllIntervals();
            if (!res.ok) {
                return emitError(jsonMode, res.exitCode, res.error, {}, out, err);
            }
            if (jsonMode) {
                writeJson(out, QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("stoppedAll"), true}});
            } else {
                writeLine(out, CliText::tr("Stopped all active intervals."));
            }
            return 0;
        }
        const CliResolveResult resolved = backend.resolveTarget(pos.first());
        if (!resolved.ok) {
            return emitError(jsonMode, 3, resolved.error, {}, out, err);
        }
        const CliSimpleResult res = backend.stopInterval(resolved.targetId);
        if (!res.ok) {
            return emitError(jsonMode, res.exitCode, res.error, {}, out, err);
        }
        if (jsonMode) {
            writeJson(out, QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("targetId"), resolved.targetId}});
        } else {
            writeLine(out, CliText::tr("Stopped interval for %1.").arg(resolved.name));
        }
        return 0;
    }

    if (action == QLatin1String("status")) {
        for (const QString &arg : rest) {
            if (arg == QLatin1String("-h") || arg == QLatin1String("--help")) {
                printSubcommandHelp(QStringLiteral("interval"), out);
                return 0;
            }
            if (arg == QLatin1String("--json")) {
                jsonMode = true;
            } else {
                return emitError(jsonMode, 2, CliText::tr("Unknown option: %1").arg(arg), {}, out, err);
            }
        }
        QJsonArray items;
        const CliSimpleResult res = backend.intervalStatus(&items);
        if (!res.ok) {
            return emitError(jsonMode, res.exitCode, res.error, {}, out, err);
        }
        if (jsonMode) {
            writeJson(out, items);
            return 0;
        }
        if (items.isEmpty()) {
            writeLine(out, CliText::tr("No active intervals."));
            return 0;
        }
        for (const QJsonValue &v : std::as_const(items)) {
            const QJsonObject obj = v.toObject();
            writeLine(out, QStringLiteral("%1\t%2 s\t%3 saved\t%4")
                               .arg(obj.value(QStringLiteral("displayName")).toString())
                               .arg(obj.value(QStringLiteral("seconds")).toInt())
                               .arg(obj.value(QStringLiteral("captureCount")).toInt())
                               .arg(obj.value(QStringLiteral("targetId")).toString()));
        }
        return 0;
    }

    return emitError(false, 2, CliText::tr("Unknown interval subcommand: %1").arg(action), {}, out, err);
}

int handleOverlay(const QStringList &args, CliBackend &backend, QIODevice &out, QIODevice &err)
{
    QString mode;
    int delay = 0;

    for (int i = 0; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        if (arg == QLatin1String("-h") || arg == QLatin1String("--help")) {
            printSubcommandHelp(QStringLiteral("overlay"), out);
            return 0;
        }
        if (arg == QLatin1String("--mode")) {
            if (i + 1 >= args.size()) {
                return emitError(false, 2, CliText::tr("Missing value for --mode."), {}, out, err);
            }
            mode = args.at(++i).trimmed().toLower();
            if (mode != QLatin1String("frozen") && mode != QLatin1String("live")) {
                return emitError(false, 2, CliText::tr("--mode must be 'frozen' or 'live'."), {}, out, err);
            }
        } else if (arg == QLatin1String("--delay")) {
            if (i + 1 >= args.size() || !parseIntChecked(args.at(++i), 0, 60, &delay)) {
                return emitError(false, 2, CliText::tr("--delay must be between 0 and 60."), {}, out, err);
            }
        } else {
            return emitError(false, 2, CliText::tr("Unknown option: %1").arg(arg), {}, out, err);
        }
    }

    const CliSimpleResult res = backend.openOverlay(mode, delay);
    if (!res.ok) {
        return emitError(false, res.exitCode, res.error, {}, out, err);
    }
    return 0;
}

} // namespace

QJsonObject captureRequestToJson(const CliCaptureRequest &request)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("target"), request.target);
    if (request.format) {
        obj.insert(QStringLiteral("format"), formatConfigKey(*request.format));
    }
    if (request.quality) {
        obj.insert(QStringLiteral("quality"), *request.quality);
    }
    if (request.pngCompression) {
        obj.insert(QStringLiteral("pngCompression"), *request.pngCompression);
    }
    if (request.lossless) {
        obj.insert(QStringLiteral("lossless"), *request.lossless);
    }
    if (request.cursor) {
        obj.insert(QStringLiteral("cursor"), *request.cursor);
    }
    obj.insert(QStringLiteral("delaySeconds"), request.delaySeconds);
    if (!request.outPath.isEmpty()) {
        obj.insert(QStringLiteral("outPath"), request.outPath);
    }
    obj.insert(QStringLiteral("toStdout"), request.toStdout);
    obj.insert(QStringLiteral("clipboard"), request.clipboard);
    obj.insert(QStringLiteral("noSave"), request.noSave);
    if (request.ocr) {
        obj.insert(QStringLiteral("ocr"), true);
    }
    if (!request.triggerKind.isEmpty()) {
        obj.insert(QStringLiteral("triggerKind"), request.triggerKind);
    }
    return obj;
}

CliCaptureRequest captureRequestFromJson(const QJsonObject &obj, bool *ok)
{
    CliCaptureRequest req;
    req.target = obj.value(QStringLiteral("target")).toString();
    if (obj.contains(QStringLiteral("format"))) {
        req.format = imageFormatFromKey(obj.value(QStringLiteral("format")).toString());
        if (!req.format) {
            if (ok) {
                *ok = false;
            }
            return req;
        }
    }
    if (obj.contains(QStringLiteral("quality"))) {
        req.quality = obj.value(QStringLiteral("quality")).toInt();
    }
    if (obj.contains(QStringLiteral("pngCompression"))) {
        req.pngCompression = obj.value(QStringLiteral("pngCompression")).toInt();
    }
    if (obj.contains(QStringLiteral("lossless"))) {
        req.lossless = obj.value(QStringLiteral("lossless")).toBool();
    }
    if (obj.contains(QStringLiteral("cursor"))) {
        req.cursor = obj.value(QStringLiteral("cursor")).toBool();
    }
    req.delaySeconds = std::clamp(obj.value(QStringLiteral("delaySeconds")).toInt(0), 0, 60);
    req.outPath = obj.value(QStringLiteral("outPath")).toString();
    req.toStdout = obj.value(QStringLiteral("toStdout")).toBool(false);
    req.clipboard = obj.value(QStringLiteral("clipboard")).toBool(false);
    req.noSave = obj.value(QStringLiteral("noSave")).toBool(false);
    req.ocr = obj.value(QStringLiteral("ocr")).toBool(false);
    req.triggerKind = obj.value(QStringLiteral("triggerKind")).toString();
    if (ok) {
        *ok = !req.target.isEmpty();
    }
    return req;
}

QJsonObject captureResultToJson(const CliCaptureResult &result)
{
    QJsonArray filesArr;
    for (const CliCaptureFile &f : result.files) {
        QJsonObject file{
            {QStringLiteral("targetId"), f.targetId},
            {QStringLiteral("path"), f.path},
            {QStringLiteral("width"), f.width},
            {QStringLiteral("height"), f.height},
            {QStringLiteral("format"), f.format},
            {QStringLiteral("bytes"), f.bytes},
        };
        if (result.ocrRequested) {
            file.insert(QStringLiteral("text"), f.text);
        }
        filesArr.append(file);
    }
    QJsonObject obj{
        {QStringLiteral("ok"), result.ok},
        {QStringLiteral("exitCode"), result.exitCode},
        {QStringLiteral("targetId"), result.targetId},
        {QStringLiteral("displayName"), result.displayName},
        {QStringLiteral("timestamp"), result.timestamp},
        {QStringLiteral("files"), filesArr},
        {QStringLiteral("error"), result.error},
    };
    if (!result.stdoutFile.isEmpty()) {
        obj.insert(QStringLiteral("stdoutFile"), result.stdoutFile);
        obj.insert(QStringLiteral("tempStdoutFile"), result.tempStdoutFile);
    }
    if (result.ocrRequested) {
        QJsonArray texts;
        QStringList plain;
        for (const CliOcrText &t : result.ocrTexts) {
            texts.append(QJsonObject{{QStringLiteral("label"), t.label}, {QStringLiteral("text"), t.text}});
            plain.append(t.text);
        }
        obj.insert(QStringLiteral("texts"), texts);
        obj.insert(QStringLiteral("text"), joinOcrTexts(plain));
        if (!result.ocrError.isEmpty()) {
            obj.insert(QStringLiteral("ocrError"), result.ocrError);
        }
    }
    return obj;
}

CliCaptureResult captureResultFromJson(const QJsonObject &obj)
{
    CliCaptureResult r;
    r.ok = obj.value(QStringLiteral("ok")).toBool(false);
    r.exitCode = obj.value(QStringLiteral("exitCode")).toInt(r.ok ? 0 : 1);
    r.targetId = obj.value(QStringLiteral("targetId")).toString();
    r.displayName = obj.value(QStringLiteral("displayName")).toString();
    r.timestamp = obj.value(QStringLiteral("timestamp")).toString();
    r.stdoutFile = obj.value(QStringLiteral("stdoutFile")).toString();
    r.tempStdoutFile = obj.value(QStringLiteral("tempStdoutFile")).toBool(false);
    r.error = obj.value(QStringLiteral("error")).toString();
    r.ocrRequested = obj.contains(QStringLiteral("texts"));
    r.ocrError = obj.value(QStringLiteral("ocrError")).toString();
    for (const QJsonValue &v : obj.value(QStringLiteral("texts")).toArray()) {
        r.ocrTexts.append({v.toObject().value(QStringLiteral("label")).toString(),
                           v.toObject().value(QStringLiteral("text")).toString()});
    }
    const QJsonArray arr = obj.value(QStringLiteral("files")).toArray();
    for (const QJsonValue &v : arr) {
        const QJsonObject fObj = v.toObject();
        r.files.append({
            fObj.value(QStringLiteral("targetId")).toString(),
            fObj.value(QStringLiteral("path")).toString(),
            fObj.value(QStringLiteral("width")).toInt(),
            fObj.value(QStringLiteral("height")).toInt(),
            fObj.value(QStringLiteral("format")).toString(),
            fObj.value(QStringLiteral("bytes")).toInteger(),
            fObj.value(QStringLiteral("text")).toString(),
        });
    }
    return r;
}

CliCaptureResult DBusCliBackend::capture(const CliCaptureRequest &request)
{
    CliCaptureResult result;
    const QByteArray payload = QJsonDocument(captureRequestToJson(request)).toJson(QJsonDocument::Compact);
    const int timeoutMs = std::max(30'000, (request.delaySeconds + 35) * 1000);
    QString err;
    const QDBusMessage reply = callControl(QStringLiteral("CaptureSync"), {QString::fromUtf8(payload)}, timeoutMs, &err);
    if (!err.isEmpty()) {
        result.ok = false;
        result.exitCode = 1;
        result.error = err;
        return result;
    }
    const QString json = reply.arguments().value(0).toString();
    QJsonDocument doc;
    if (!parseJsonDoc(json.toUtf8(), &doc, &err)) {
        result.ok = false;
        result.exitCode = 1;
        result.error = err;
        return result;
    }
    return captureResultFromJson(doc.object());
}

CliSimpleResult DBusCliBackend::listTargets(QJsonArray *outArray)
{
    QString err;
    const QDBusMessage reply = callControl(QStringLiteral("Targets"), {}, 10'000, &err);
    if (!err.isEmpty()) {
        return {false, 1, {}, err};
    }
    QJsonDocument doc;
    if (!parseJsonDoc(reply.arguments().value(0).toString().toUtf8(), &doc, &err)) {
        return {false, 1, {}, err};
    }
    *outArray = doc.array();
    return {true, 0, {}, {}};
}

CliSimpleResult DBusCliBackend::listMonitors(QJsonArray *outArray)
{
    QString err;
    const QDBusMessage reply = callControl(QStringLiteral("ListMonitors"), {}, 10'000, &err);
    if (!err.isEmpty()) {
        return {false, 1, {}, err};
    }
    QJsonDocument doc;
    if (!parseJsonDoc(reply.arguments().value(0).toString().toUtf8(), &doc, &err)) {
        return {false, 1, {}, err};
    }
    *outArray = doc.array();
    return {true, 0, {}, {}};
}

CliSimpleResult DBusCliBackend::listWindows(QJsonArray *outArray)
{
    QString err;
    const QDBusMessage reply = callControl(QStringLiteral("ListWindows"), {}, 10'000, &err);
    if (!err.isEmpty()) {
        return {false, 1, {}, err};
    }
    QJsonDocument doc;
    if (!parseJsonDoc(reply.arguments().value(0).toString().toUtf8(), &doc, &err)) {
        return {false, 1, {}, err};
    }
    *outArray = doc.array();
    return {true, 0, {}, {}};
}

CliResolveResult DBusCliBackend::resolveTarget(const QString &query)
{
    QString err;
    const QDBusMessage reply = callControl(QStringLiteral("ResolveTarget"), {query}, 10'000, &err);
    if (!err.isEmpty()) {
        return {false, {}, {}, {}, err};
    }
    QJsonDocument doc;
    if (!parseJsonDoc(reply.arguments().value(0).toString().toUtf8(), &doc, &err)) {
        return {false, {}, {}, {}, err};
    }
    const QJsonObject obj = doc.object();
    return {obj.value(QStringLiteral("ok")).toBool(false),
            obj.value(QStringLiteral("targetId")).toString(),
            obj.value(QStringLiteral("kind")).toString(),
            obj.value(QStringLiteral("name")).toString(),
            obj.value(QStringLiteral("error")).toString()};
}

CliSimpleResult DBusCliBackend::addAreaInteractive()
{
    QString err;
    callControl(QStringLiteral("AddArea"), {}, 10'000, &err);
    return {err.isEmpty(), err.isEmpty() ? 0 : 1, {}, err};
}

CliSimpleResult DBusCliBackend::addAreaRect(const QString &name, const QString &connector, const QRect &rect)
{
    QString err;
    const QDBusMessage reply = callControl(
        QStringLiteral("AddAreaRect"), {name, connector, rect.x(), rect.y(), rect.width(), rect.height()}, 10'000, &err);
    if (!err.isEmpty()) {
        return {false, 1, {}, err};
    }
    QJsonDocument doc;
    if (!parseJsonDoc(reply.arguments().value(0).toString().toUtf8(), &doc, &err)) {
        return {false, 1, {}, err};
    }
    const QJsonObject obj = doc.object();
    const bool ok = obj.value(QStringLiteral("ok")).toBool(false);
    return {ok, ok ? 0 : obj.value(QStringLiteral("exitCode")).toInt(1), obj,
            obj.value(QStringLiteral("error")).toString()};
}

CliSimpleResult DBusCliBackend::editArea(const QString &targetId)
{
    QString err;
    callControl(QStringLiteral("EditArea"), {targetId}, 10'000, &err);
    return {err.isEmpty(), err.isEmpty() ? 0 : 1, {}, err};
}

CliSimpleResult DBusCliBackend::renameTarget(const QString &targetId, const QString &newName)
{
    QString err;
    const QDBusMessage reply = callControl(QStringLiteral("Rename"), {targetId, newName}, 10'000, &err);
    if (!err.isEmpty()) {
        return {false, 1, {}, err};
    }
    const QString msg = reply.arguments().value(0).toString();
    return {msg.isEmpty(), msg.isEmpty() ? 0 : 1, {}, msg};
}

CliSimpleResult DBusCliBackend::removeTarget(const QString &targetId)
{
    QString err;
    callControl(QStringLiteral("Remove"), {targetId}, 10'000, &err);
    return {err.isEmpty(), err.isEmpty() ? 0 : 1, {}, err};
}

CliSimpleResult DBusCliBackend::startInterval(const QString &targetId, int seconds, int maxCaptures, int maxMinutes,
                                            bool onlyWhenChanged)
{
    QString err;
    const QDBusMessage reply = callControl(
        QStringLiteral("StartInterval"), {targetId, seconds, maxCaptures, maxMinutes, onlyWhenChanged}, 10'000, &err);
    if (!err.isEmpty()) {
        return {false, 1, {}, err};
    }
    const QString msg = reply.arguments().value(0).toString();
    return {msg.isEmpty(), msg.isEmpty() ? 0 : 1, {}, msg};
}

CliSimpleResult DBusCliBackend::stopInterval(const QString &targetId)
{
    QString err;
    callControl(QStringLiteral("StopInterval"), {targetId}, 10'000, &err);
    return {err.isEmpty(), err.isEmpty() ? 0 : 1, {}, err};
}

CliSimpleResult DBusCliBackend::stopAllIntervals()
{
    QString err;
    callControl(QStringLiteral("StopAllIntervals"), {}, 10'000, &err);
    return {err.isEmpty(), err.isEmpty() ? 0 : 1, {}, err};
}

CliSimpleResult DBusCliBackend::intervalStatus(QJsonArray *outArray)
{
    QString err;
    const QDBusMessage reply = callControl(QStringLiteral("Intervals"), {}, 10'000, &err);
    if (!err.isEmpty()) {
        return {false, 1, {}, err};
    }
    QJsonDocument doc;
    if (!parseJsonDoc(reply.arguments().value(0).toString().toUtf8(), &doc, &err)) {
        return {false, 1, {}, err};
    }
    *outArray = doc.array();
    return {true, 0, {}, {}};
}

CliSimpleResult DBusCliBackend::openOverlay(const QString &mode, int delaySeconds)
{
    QString err;
    const QDBusMessage reply = callControl(QStringLiteral("OpenOverlay"), {mode, delaySeconds}, 10'000, &err);
    if (!err.isEmpty()) {
        return {false, 1, {}, err};
    }
    const QString msg = reply.arguments().value(0).toString();
    return {msg.isEmpty(), msg.isEmpty() ? 0 : 1, {}, msg};
}

static int handleHistory(const QStringList &args, CliBackend &backend, QIODevice &out, QIODevice &err)
{
    bool jsonMode = args.contains(QStringLiteral("--json"));
    int limit = -1;
    for (int i = 0; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        if (arg == QLatin1String("-h") || arg == QLatin1String("--help")) {
            printSubcommandHelp(QStringLiteral("history"), out);
            return 0;
        }
        if (arg == QLatin1String("--json")) {
            continue;
        }
        if (arg == QLatin1String("--limit")) {
            bool ok = false;
            limit = i + 1 < args.size() ? args.at(i + 1).toInt(&ok) : 0;
            ++i;
            if (!ok || limit < 1) {
                return emitError(jsonMode, 2, CliText::tr("--limit expects a positive number."), {}, out, err);
            }
            continue;
        }
        return emitError(jsonMode, 2, CliText::tr("Unknown option: %1").arg(arg), {}, out, err);
    }
    QJsonArray items;
    const CliSimpleResult res = backend.history(&items);
    if (!res.ok) {
        return emitError(jsonMode, res.exitCode, res.error, {}, out, err);
    }
    if (limit > 0) {
        while (items.size() > limit) {
            items.removeLast();
        }
    }
    if (jsonMode) {
        writeJson(out, items);
        return 0;
    }
    if (items.isEmpty()) {
        writeLine(out, CliText::tr("No recent captures."));
        return 0;
    }
    for (const QJsonValue &v : std::as_const(items)) {
        const QJsonObject obj = v.toObject();
        const QDateTime time = QDateTime::fromString(obj.value(QStringLiteral("timestamp")).toString(), Qt::ISODateWithMs);
        const QString when = time.isValid() ? time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                                            : obj.value(QStringLiteral("timestamp")).toString();
        for (const QJsonValue &file : obj.value(QStringLiteral("files")).toArray()) {
            writeLine(out, QStringLiteral("%1\t%2\t%3")
                               .arg(when, obj.value(QStringLiteral("targetName")).toString(),
                                    file.toObject().value(QStringLiteral("path")).toString()));
        }
    }
    return 0;
}

CliSimpleResult DBusCliBackend::history(QJsonArray *outArray)
{
    QString err;
    const QDBusMessage reply = callControl(QStringLiteral("History"), {}, 10'000, &err);
    if (!err.isEmpty()) {
        return {false, 1, {}, err};
    }
    QJsonDocument doc;
    if (!parseJsonDoc(reply.arguments().value(0).toString().toUtf8(), &doc, &err)) {
        return {false, 1, {}, err};
    }
    *outArray = doc.array();
    return {true, 0, {}, {}};
}

bool isCliSubcommand(const QString &arg)
{
    return arg == QLatin1String("capture") || arg == QLatin1String("list") || arg == QLatin1String("area")
        || arg == QLatin1String("interval") || arg == QLatin1String("overlay") || arg == QLatin1String("history");
}

int runCli(const QStringList &arguments, CliBackend &backend, QIODevice &out, QIODevice &err)
{
    if (arguments.size() < 2) {
        writeLine(err, CliText::tr("Missing subcommand."));
        return 2;
    }
    const QString &subcommand = arguments.at(1);
    const QStringList subArgs = arguments.mid(2);

    if (subcommand == QLatin1String("capture")) {
        return handleCapture(subArgs, backend, out, err);
    }
    if (subcommand == QLatin1String("list")) {
        return handleList(subArgs, backend, out, err);
    }
    if (subcommand == QLatin1String("area")) {
        return handleArea(subArgs, backend, out, err);
    }
    if (subcommand == QLatin1String("interval")) {
        return handleInterval(subArgs, backend, out, err);
    }
    if (subcommand == QLatin1String("overlay")) {
        return handleOverlay(subArgs, backend, out, err);
    }
    if (subcommand == QLatin1String("history")) {
        return handleHistory(subArgs, backend, out, err);
    }

    writeLine(err, CliText::tr("Unknown subcommand: %1").arg(subcommand));
    return 2;
}

} // namespace glimply
