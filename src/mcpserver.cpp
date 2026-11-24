#include "mcpserver.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QIODevice>
#include <QJsonDocument>

#include <iostream>
#include <string>

#ifndef GLIMPLY_VERSION
#define GLIMPLY_VERSION "0.7.0"
#endif

namespace glimply
{
namespace
{

constexpr auto dbusService = "io.github.m00sfett.glimply";
constexpr auto dbusPath = "/Control";
constexpr auto dbusIface = "io.github.m00sfett.glimply.Control";

QJsonObject propString(const QString &desc)
{
    return {{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("description"), desc}};
}

QJsonObject propInt(const QString &desc)
{
    return {{QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("description"), desc}};
}

QJsonObject propBool(const QString &desc)
{
    return {{QStringLiteral("type"), QStringLiteral("boolean")}, {QStringLiteral("description"), desc}};
}

QJsonObject makeTool(const QString &name, const QString &description, const QJsonObject &properties,
                     const QStringList &required = {})
{
    QJsonObject schema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), properties},
    };
    if (!required.isEmpty()) {
        QJsonArray reqArr;
        for (const QString &r : required) {
            reqArr.append(r);
        }
        schema.insert(QStringLiteral("required"), reqArr);
    }
    return {
        {QStringLiteral("name"), name},
        {QStringLiteral("description"), description},
        {QStringLiteral("inputSchema"), schema},
    };
}

QByteArray jsonRpcError(const QJsonValue &id, int code, const QString &message)
{
    QJsonObject obj{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id.isUndefined() ? QJsonValue(QJsonValue::Null) : id},
        {QStringLiteral("error"),
         QJsonObject{
             {QStringLiteral("code"), code},
             {QStringLiteral("message"), message},
         }},
    };
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

QByteArray jsonRpcResult(const QJsonValue &id, const QJsonValue &result)
{
    QJsonObject obj{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("result"), result},
    };
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

} // namespace

QJsonObject mcpToolResponseToJson(const McpToolResponse &response)
{
    QJsonObject obj{
        {QStringLiteral("ok"), response.ok},
        {QStringLiteral("metadata"), response.metadata},
        {QStringLiteral("error"), response.error},
    };
    if (response.image) {
        obj.insert(QStringLiteral("imageBase64"), QString::fromLatin1(response.image->base64Data));
        obj.insert(QStringLiteral("imageMimeType"), response.image->mimeType);
    }
    return obj;
}

McpToolResponse mcpToolResponseFromJson(const QJsonObject &obj)
{
    McpToolResponse res;
    res.ok = obj.value(QStringLiteral("ok")).toBool(false);
    res.metadata = obj.value(QStringLiteral("metadata")).toObject();
    res.error = obj.value(QStringLiteral("error")).toString();
    const QString b64 = obj.value(QStringLiteral("imageBase64")).toString();
    const QString mime = obj.value(QStringLiteral("imageMimeType")).toString();
    if (!b64.isEmpty() && !mime.isEmpty()) {
        res.image = McpImagePayload{b64.toLatin1(), mime};
    }
    return res;
}

McpToolResponse DBusMcpBackend::callTool(const QString &toolName, const QJsonObject &arguments)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return {false, {}, QStringLiteral("Could not connect to the D-Bus session bus."), std::nullopt};
    }
    QDBusMessage msg = QDBusMessage::createMethodCall(QLatin1String(dbusService), QLatin1String(dbusPath),
                                                      QLatin1String(dbusIface), QStringLiteral("McpDispatch"));
    msg.setArguments({toolName, QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact))});
    const QDBusMessage reply = bus.call(msg, QDBus::Block, 120'000);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        return {false, {},
                reply.errorMessage().isEmpty() ? reply.errorName() : reply.errorMessage(),
                std::nullopt};
    }
    const QString payload = reply.arguments().value(0).toString();
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return {false, {}, QStringLiteral("Invalid response from glimply daemon: %1").arg(err.errorString()),
                std::nullopt};
    }
    return mcpToolResponseFromJson(doc.object());
}

QJsonArray McpServer::toolDefinitions()
{
    QJsonArray tools;

    tools.append(makeTool(QStringLiteral("list_targets"),
                          QStringLiteral("List accessible screenshot targets (areas, groups, monitors) with geometry "
                                         "and names."),
                          {}));

    tools.append(makeTool(QStringLiteral("list_monitors"),
                          QStringLiteral("List accessible connected monitors with logical geometry and connector "
                                         "names."),
                          {}));

    tools.append(makeTool(QStringLiteral("list_windows"),
                          QStringLiteral("List open desktop windows (requires Confirm or Full session access mode)."),
                          {}));

    tools.append(makeTool(
        QStringLiteral("capture_target"),
        QStringLiteral("Capture a saved area, group, monitor, or 'all' monitors cleanly via glimply."),
        {
            {QStringLiteral("target"), propString(QStringLiteral("Target ID or name (area, group, monitor, or 'all')."))},
            {QStringLiteral("format"), propString(QStringLiteral("Optional image format: png, jpeg, webp, avif, jxl."))},
            {QStringLiteral("quality"), propInt(QStringLiteral("Optional encoder quality (1..100)."))},
            {QStringLiteral("include_cursor"), propBool(QStringLiteral("Override mouse cursor inclusion."))},
            {QStringLiteral("include_image"), propBool(QStringLiteral("Also return base64-encoded image content."))},
            {QStringLiteral("max_dimension"), propInt(QStringLiteral("Optional max pixel width/height for inline image."))},
        },
        {QStringLiteral("target")}));

    tools.append(makeTool(
        QStringLiteral("capture_region"),
        QStringLiteral("Capture a logical monitor region directly (requires Confirm or Full session mode)."),
        {
            {QStringLiteral("monitor"), propString(QStringLiteral("Monitor connector or custom name."))},
            {QStringLiteral("x"), propInt(QStringLiteral("Monitor-local logical X."))},
            {QStringLiteral("y"), propInt(QStringLiteral("Monitor-local logical Y."))},
            {QStringLiteral("width"), propInt(QStringLiteral("Logical width in pixels (> 0)."))},
            {QStringLiteral("height"), propInt(QStringLiteral("Logical height in pixels (> 0)."))},
            {QStringLiteral("format"), propString(QStringLiteral("Optional image format: png, jpeg, webp, avif, jxl."))},
            {QStringLiteral("quality"), propInt(QStringLiteral("Optional encoder quality (1..100)."))},
            {QStringLiteral("include_cursor"), propBool(QStringLiteral("Include mouse cursor."))},
            {QStringLiteral("include_image"), propBool(QStringLiteral("Also return base64-encoded image content."))},
            {QStringLiteral("max_dimension"), propInt(QStringLiteral("Optional max pixel width/height for inline image."))},
        },
        {QStringLiteral("monitor"), QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("width"),
         QStringLiteral("height")}));

    tools.append(makeTool(
        QStringLiteral("capture_window"),
        QStringLiteral("Capture an individual desktop window via KWin (requires Confirm or Full session mode)."),
        {
            {QStringLiteral("window_id"), propString(QStringLiteral("Window handle UUID, appName, or title."))},
            {QStringLiteral("format"), propString(QStringLiteral("Optional image format: png, jpeg, webp, avif, jxl."))},
            {QStringLiteral("quality"), propInt(QStringLiteral("Optional encoder quality (1..100)."))},
            {QStringLiteral("include_cursor"), propBool(QStringLiteral("Include mouse cursor."))},
            {QStringLiteral("include_image"), propBool(QStringLiteral("Also return base64-encoded image content."))},
            {QStringLiteral("max_dimension"), propInt(QStringLiteral("Optional max pixel width/height for inline image."))},
        },
        {QStringLiteral("window_id")}));

    tools.append(makeTool(
        QStringLiteral("get_last_capture"),
        QStringLiteral("Inspect metadata (and optionally image data) for the most recent glimply capture."),
        {
            {QStringLiteral("include_image"), propBool(QStringLiteral("Also return base64-encoded image content."))},
            {QStringLiteral("max_dimension"), propInt(QStringLiteral("Optional max pixel width/height for inline image."))},
        }));

    tools.append(makeTool(
        QStringLiteral("start_interval"),
        QStringLiteral("Start scheduled interval captures for a target or group."),
        {
            {QStringLiteral("target"), propString(QStringLiteral("Target ID or name."))},
            {QStringLiteral("seconds"), propInt(QStringLiteral("Interval seconds (1..86400)."))},
            {QStringLiteral("max_captures"), propInt(QStringLiteral("Optional stop after N saved captures."))},
            {QStringLiteral("max_minutes"), propInt(QStringLiteral("Optional stop after N minutes."))},
            {QStringLiteral("only_when_changed"), propBool(QStringLiteral("Skip unchanged frames."))},
        },
        {QStringLiteral("target")}));

    tools.append(makeTool(
        QStringLiteral("stop_interval"),
        QStringLiteral("Stop an active interval capture or all active intervals."),
        {
            {QStringLiteral("target"), propString(QStringLiteral("Target ID/name, or omit with all=true."))},
            {QStringLiteral("all"), propBool(QStringLiteral("Stop all active intervals (Confirm/Full modes only)."))},
        }));

    tools.append(makeTool(
        QStringLiteral("annotate_image"),
        QStringLiteral("Apply vector callouts (arrow, rect, ellipse, pen, highlighter, text, step, spotlight) and "
                       "irreversible pixel redactions (redact: solid/pixelate/blur) onto an image file."),
        {
            {QStringLiteral("input_path"), propString(QStringLiteral("Path to an existing source image."))},
            {QStringLiteral("operations"),
             QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                         {QStringLiteral("description"), QStringLiteral("List of annotation/redaction objects.")},
                         {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}}}},
            {QStringLiteral("output_path"), propString(QStringLiteral("Optional output path (defaults to -annotated file)."))},
            {QStringLiteral("overwrite"), propBool(QStringLiteral("Explicitly allow overwriting input_path."))},
            {QStringLiteral("format"), propString(QStringLiteral("Optional output format: png, jpeg, webp, avif, jxl."))},
            {QStringLiteral("quality"), propInt(QStringLiteral("Optional encoder quality (1..100)."))},
        },
        {QStringLiteral("input_path"), QStringLiteral("operations")}));

    return tools;
}

QByteArray McpServer::processMessage(const QByteArray &rawLine, McpToolBackend &backend)
{
    const QByteArray trimmed = rawLine.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return jsonRpcError(QJsonValue(QJsonValue::Null), -32700,
                            QStringLiteral("Parse error: %1").arg(err.errorString()));
    }

    const QJsonObject req = doc.object();
    const bool hasId = req.contains(QStringLiteral("id"));
    const QJsonValue id = req.value(QStringLiteral("id"));
    const QString method = req.value(QStringLiteral("method")).toString();
    const QJsonObject params = req.value(QStringLiteral("params")).toObject();

    // Notifications carry no id and expect zero response bytes.
    if (!hasId || method.startsWith(QLatin1String("notifications/"))) {
        return {};
    }

    if (method == QLatin1String("initialize")) {
        const QString clientProto = params.value(QStringLiteral("protocolVersion")).toString();
        const QString proto = (clientProto == QLatin1String("2024-11-05") || clientProto == QLatin1String("2025-03-26"))
                                  ? clientProto
                                  : QStringLiteral("2025-03-26");
        return jsonRpcResult(
            id,
            QJsonObject{
                {QStringLiteral("protocolVersion"), proto},
                {QStringLiteral("capabilities"), QJsonObject{{QStringLiteral("tools"), QJsonObject{{QStringLiteral("listChanged"), false}}}}},
                {QStringLiteral("serverInfo"),
                 QJsonObject{{QStringLiteral("name"), QStringLiteral("glimply")},
                             {QStringLiteral("version"), QStringLiteral(GLIMPLY_VERSION)}}},
            });
    }

    if (method == QLatin1String("ping")) {
        return jsonRpcResult(id, QJsonObject{});
    }

    if (method == QLatin1String("tools/list")) {
        return jsonRpcResult(id, QJsonObject{{QStringLiteral("tools"), toolDefinitions()}});
    }

    if (method == QLatin1String("tools/call")) {
        const QString toolName = params.value(QStringLiteral("name")).toString().trimmed();
        if (toolName.isEmpty()) {
            return jsonRpcError(id, -32602, QStringLiteral("tools/call requires a 'name' string."));
        }
        const QJsonObject arguments = params.value(QStringLiteral("arguments")).toObject();
        const McpToolResponse res = backend.callTool(toolName, arguments);

        QJsonArray content;
        if (!res.ok) {
            content.append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("text")},
                {QStringLiteral("text"),
                 res.error.isEmpty() ? QStringLiteral("MCP tool call failed.") : res.error},
            });
            return jsonRpcResult(id, QJsonObject{{QStringLiteral("content"), content},
                                                 {QStringLiteral("isError"), true}});
        }

        content.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("text")},
            {QStringLiteral("text"), QString::fromUtf8(QJsonDocument(res.metadata).toJson(QJsonDocument::Compact))},
        });
        if (res.image && !res.image->base64Data.isEmpty()) {
            content.append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("image")},
                {QStringLiteral("data"), QString::fromLatin1(res.image->base64Data)},
                {QStringLiteral("mimeType"), res.image->mimeType},
            });
        }
        return jsonRpcResult(id, QJsonObject{{QStringLiteral("content"), content},
                                             {QStringLiteral("isError"), false}});
    }

    return jsonRpcError(id, -32601, QStringLiteral("Method not found: %1").arg(method));
}

void McpServer::runStream(McpToolBackend &backend, QIODevice &in, QIODevice &out)
{
    while (!in.atEnd() || in.bytesAvailable() > 0) {
        const QByteArray line = in.readLine();
        if (line.isEmpty()) {
            break;
        }
        const QByteArray reply = processMessage(line, backend);
        if (!reply.isEmpty()) {
            out.write(reply);
            out.write("\n");
        }
    }
}

int McpServer::runStdio(McpToolBackend &backend)
{
    std::string line;
    while (std::getline(std::cin, line)) {
        const QByteArray reply = processMessage(QByteArray::fromStdString(line), backend);
        if (!reply.isEmpty()) {
            std::cout.write(reply.constData(), reply.size());
            std::cout.put('\n');
            std::cout.flush();
        }
    }
    return 0;
}

} // namespace glimply
