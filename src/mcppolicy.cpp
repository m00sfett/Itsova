#include "mcppolicy.h"

namespace glimply
{

// Denial reasons go back to the MCP client and stay English whatever the UI language (#51).
McpPolicyResult evaluateMcpAccess(const McpConfig &config, const QString &toolName, const QString &resolvedTargetId)
{
    if (!config.enabled) {
        return {McpDecision::Deny,
                QStringLiteral("MCP server access is disabled. Enable it in glimply Settings -> Automation -> "
                                  "Local MCP Server.")};
    }

    if (config.accessMode == McpAccessMode::FullSession) {
        return {McpDecision::Allow, {}};
    }

    if (config.accessMode == McpAccessMode::ConfirmPerCapture) {
        if (toolName == QLatin1String("list_targets") || toolName == QLatin1String("list_monitors")
            || toolName == QLatin1String("list_windows") || toolName == QLatin1String("stop_interval")) {
            return {McpDecision::Allow, {}};
        }
        if (toolName == QLatin1String("capture_target") || toolName == QLatin1String("capture_region")
            || toolName == QLatin1String("capture_window") || toolName == QLatin1String("get_last_capture")
            || toolName == QLatin1String("start_interval") || toolName == QLatin1String("annotate_image")) {
            return {McpDecision::NeedsConfirm, {}};
        }
        return {McpDecision::Deny, QStringLiteral("Unknown MCP tool: %1").arg(toolName)};
    }

    // McpAccessMode::Allowlist
    if (toolName == QLatin1String("list_targets") || toolName == QLatin1String("list_monitors")) {
        return {McpDecision::Allow, {}};
    }
    if (toolName == QLatin1String("list_windows")) {
        return {McpDecision::Deny,
                QStringLiteral("Window listing is disabled while MCP access mode is set to Allowlist.")};
    }
    if (toolName == QLatin1String("capture_region") || toolName == QLatin1String("capture_window")) {
        return {McpDecision::Deny,
                QStringLiteral("Arbitrary regions and windows are blocked in MCP Allowlist mode. Choose explicitly "
                                  "allowlisted targets or change access scope in glimply Settings -> Automation.")};
    }
    if (toolName == QLatin1String("annotate_image")) {
        return {McpDecision::Allow, {}};
    }
    if (toolName == QLatin1String("capture_target") || toolName == QLatin1String("start_interval")
        || toolName == QLatin1String("stop_interval")) {
        if (!resolvedTargetId.isEmpty() && config.allowedTargetIds.contains(resolvedTargetId)) {
            return {McpDecision::Allow, {}};
        }
        return {McpDecision::Deny,
                QStringLiteral("Target '%1' is not in the MCP allowlist. Approve it in glimply Settings -> "
                                  "Automation -> Local MCP Server.")
                    .arg(resolvedTargetId)};
    }
    if (toolName == QLatin1String("get_last_capture")) {
        if (!resolvedTargetId.isEmpty() && config.allowedTargetIds.contains(resolvedTargetId)) {
            return {McpDecision::Allow, {}};
        }
        return {McpDecision::Deny,
                QStringLiteral("The last capture is not from an allowlisted MCP target.")};
    }

    return {McpDecision::Deny, QStringLiteral("Unknown MCP tool: %1").arg(toolName)};
}

} // namespace glimply
