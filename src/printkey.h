#pragma once

#include <QString>
#include <QStringList>

class KConfig;

namespace glimply
{

// Takes the Print key over from other global shortcuts (usually Spectacle) and gives it back
// exactly. The previous owners and their full key lists are kept in glimply's configuration, so
// handing back also works after a restart or from the uninstaller.
namespace PrintKey
{

// Action id of glimply's own Print shortcut.
[[nodiscard]] QString actionId();

// True while glimply holds the Print key (as far as glimply knows; the user may still change it
// in System Settings).
[[nodiscard]] bool isClaimed(const KConfig &config);
// True if glimply has ever decided about the Print key (claimed it, or handed it back).
[[nodiscard]] bool isDecided(const KConfig &config);

// Removes Print from every other action and remembers those actions. Returns their visible names.
QStringList claim(KConfig &config);

// Gives Print back to the remembered actions. clearOwnAction also clears glimply's own action
// directly through KGlobalAccel, for use while glimply itself is not running.
void release(KConfig &config, bool clearOwnAction);

} // namespace PrintKey

} // namespace glimply
