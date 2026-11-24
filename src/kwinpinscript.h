#pragma once

#include <QRect>
#include <QString>

namespace glimply
{

// Qt cannot keep a window above others or place it on Wayland. Pins therefore ask KWin through a
// small per-pin script (verified in the #28 spike on Plasma 6.8): it finds the pin window by its
// unique caption, sets keep-above and the skip flags, moves it to the stored position and reports
// later moves back over D-Bus (ControlService::PinMoved).
struct KWinPinScriptRequest
{
    QString pinId;
    QString caption;
    QRect geometry; // logical, global; ignored when invalid
    QString service; // D-Bus service that receives PinMoved
    QString path;
    QString interface;
};

// JavaScript source for the request; values are JSON-escaped so captions cannot break out.
[[nodiscard]] QString kwinPinScriptSource(const KWinPinScriptRequest &request);
[[nodiscard]] QString kwinPinPluginName(const QString &pinId);

// Writes the script to $XDG_RUNTIME_DIR/glimply/, replaces any earlier script of the same pin in
// KWin and runs it. Asynchronous; failures are only logged (the pin still shows, just unmanaged).
void applyKWinPinScript(const KWinPinScriptRequest &request);
// Unloads the pin's script from KWin and deletes the file.
void unloadKWinPinScript(const QString &pinId);

} // namespace glimply
