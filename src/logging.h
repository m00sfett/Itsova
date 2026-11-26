#pragma once

#include <QLoggingCategory>
#include <QStringList>

// Logging categories (#92). Info and above always reach the journal; debug is off unless the user
// turns on verbose logging (Settings → General or `glimply --verbose`). Never log secrets or file
// contents; paths are fine.
Q_DECLARE_LOGGING_CATEGORY(lcApp)
Q_DECLARE_LOGGING_CATEGORY(lcCapture)
Q_DECLARE_LOGGING_CATEGORY(lcOverlay)
Q_DECLARE_LOGGING_CATEGORY(lcRecord)
Q_DECLARE_LOGGING_CATEGORY(lcTray)
Q_DECLARE_LOGGING_CATEGORY(lcDbus)
Q_DECLARE_LOGGING_CATEGORY(lcMcp)
Q_DECLARE_LOGGING_CATEGORY(lcSettings)
Q_DECLARE_LOGGING_CATEGORY(lcHooks)

namespace glimply
{

// Switches debug output of every glimply category on or off at runtime.
void setVerboseLogging(bool on);
[[nodiscard]] bool verboseLogging();

// The journalctl arguments behind `glimply logs`: glimply's own journal lines, followed by
// whatever the user passes (for example -f, -n 200 or --since today).
[[nodiscard]] QStringList journalctlArguments(const QStringList &extra);

} // namespace glimply
