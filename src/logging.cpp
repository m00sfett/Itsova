#include "logging.h"

// Debug is off by default (QtInfoMsg); distributions often disable debug output globally anyway.
Q_LOGGING_CATEGORY(lcApp, "glimply.app", QtInfoMsg)
Q_LOGGING_CATEGORY(lcCapture, "glimply.capture", QtInfoMsg)
Q_LOGGING_CATEGORY(lcOverlay, "glimply.overlay", QtInfoMsg)
Q_LOGGING_CATEGORY(lcRecord, "glimply.record", QtInfoMsg)
Q_LOGGING_CATEGORY(lcTray, "glimply.tray", QtInfoMsg)
Q_LOGGING_CATEGORY(lcDbus, "glimply.dbus", QtInfoMsg)
Q_LOGGING_CATEGORY(lcMcp, "glimply.mcp", QtInfoMsg)
Q_LOGGING_CATEGORY(lcSettings, "glimply.settings", QtInfoMsg)
Q_LOGGING_CATEGORY(lcHooks, "glimply.hooks", QtInfoMsg)

namespace glimply
{

namespace
{
bool verbose = false;
}

void setVerboseLogging(bool on)
{
    if (verbose == on) {
        return;
    }
    verbose = on;
    // Filter rules override the categories' default level; an empty rule set restores them.
    QLoggingCategory::setFilterRules(on ? QStringLiteral("glimply.*.debug=true") : QString());
    qCInfo(lcApp) << "verbose logging" << (on ? "on" : "off");
}

bool verboseLogging()
{
    return verbose;
}

QStringList journalctlArguments(const QStringList &extra)
{
    QStringList args{QStringLiteral("--user"), QStringLiteral("SYSLOG_IDENTIFIER=glimply"),
                     QStringLiteral("--output=short-precise")};
    // Without options show the recent past instead of paging through every line ever logged.
    if (extra.isEmpty()) {
        args << QStringLiteral("--since=-1h") << QStringLiteral("--no-pager");
    }
    args += extra;
    return args;
}

} // namespace glimply
