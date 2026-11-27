#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <optional>

class QIODevice;

namespace itsova
{

struct McpImagePayload
{
    QByteArray base64Data;
    QString mimeType;
};

struct McpToolResponse
{
    bool ok = false;
    QJsonObject metadata;
    QString error;
    std::optional<McpImagePayload> image;
};

[[nodiscard]] QJsonObject mcpToolResponseToJson(const McpToolResponse &response);
[[nodiscard]] McpToolResponse mcpToolResponseFromJson(const QJsonObject &obj);

class McpToolBackend
{
public:
    virtual ~McpToolBackend() = default;
    virtual McpToolResponse callTool(const QString &toolName, const QJsonObject &arguments) = 0;
};

class DBusMcpBackend final : public McpToolBackend
{
public:
    McpToolResponse callTool(const QString &toolName, const QJsonObject &arguments) override;
};

// Local stdio JSON-RPC 2.0 MCP server. Zero network listeners.
class McpServer
{
public:
    [[nodiscard]] static QJsonArray toolDefinitions();
    // Processes one JSON-RPC line. Returns serialized JSON-RPC response (without trailing newline),
    // or empty QByteArray for notifications that expect no reply.
    [[nodiscard]] static QByteArray processMessage(const QByteArray &rawLine, McpToolBackend &backend);

    static void runStream(McpToolBackend &backend, QIODevice &in, QIODevice &out);
    static int runStdio(McpToolBackend &backend);
};

} // namespace itsova
