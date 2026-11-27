#include "logging.h"

// Debug is off by default (QtInfoMsg); distributions often disable debug output globally anyway.
Q_LOGGING_CATEGORY(lcApp, "itsova.app", QtInfoMsg)
Q_LOGGING_CATEGORY(lcCapture, "itsova.capture", QtInfoMsg)
Q_LOGGING_CATEGORY(lcOverlay, "itsova.overlay", QtInfoMsg)
Q_LOGGING_CATEGORY(lcRecord, "itsova.record", QtInfoMsg)
Q_LOGGING_CATEGORY(lcTray, "itsova.tray", QtInfoMsg)
Q_LOGGING_CATEGORY(lcDbus, "itsova.dbus", QtInfoMsg)
Q_LOGGING_CATEGORY(lcMcp, "itsova.mcp", QtInfoMsg)
Q_LOGGING_CATEGORY(lcSettings, "itsova.settings", QtInfoMsg)
Q_LOGGING_CATEGORY(lcHooks, "itsova.hooks", QtInfoMsg)

namespace itsova
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
    QLoggingCategory::setFilterRules(on ? QStringLiteral("itsova.*.debug=true") : QString());
    qCInfo(lcApp) << "verbose logging" << (on ? "on" : "off");
}

bool verboseLogging()
{
    return verbose;
}

QStringList journalctlArguments(const QStringList &extra)
{
    QStringList args{QStringLiteral("--user"), QStringLiteral("SYSLOG_IDENTIFIER=itsova"),
                     QStringLiteral("--output=short-precise")};
    // Without options show the recent past instead of paging through every line ever logged.
    if (extra.isEmpty()) {
        args << QStringLiteral("--since=-1h") << QStringLiteral("--no-pager");
    }
    args += extra;
    return args;
}

} // namespace itsova
