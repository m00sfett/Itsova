#pragma once

#include "targets.h"

#include <QString>

namespace glimply
{

enum class McpDecision {
    Allow,
    NeedsConfirm,
    Deny,
};

struct McpPolicyResult
{
    McpDecision decision = McpDecision::Deny;
    QString reason;
};

// Pure security policy evaluator for the local MCP server. Enforces disabled-by-default opt-in and
// Allowlist / ConfirmPerCapture / FullSession scoping rules.
[[nodiscard]] McpPolicyResult evaluateMcpAccess(const McpConfig &config,
                                                  const QString &toolName,
                                                  const QString &resolvedTargetId = QString());

} // namespace glimply
