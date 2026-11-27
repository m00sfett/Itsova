#pragma once

#include <QString>
#include <QStringList>

class KConfig;

namespace itsova
{

// Takes the Print key over from other global shortcuts (usually Spectacle) and gives it back
// exactly. The previous owners and their full key lists are kept in itsova's configuration, so
// handing back also works after a restart or from the uninstaller.
namespace PrintKey
{

// Action id of itsova's own Print shortcut.
[[nodiscard]] QString actionId();

// True while itsova holds the Print key (as far as itsova knows; the user may still change it
// in System Settings).
[[nodiscard]] bool isClaimed(const KConfig &config);
// True if itsova has ever decided about the Print key (claimed it, or handed it back).
[[nodiscard]] bool isDecided(const KConfig &config);

// Removes Print from every other action and remembers those actions. Returns their visible names.
QStringList claim(KConfig &config);

// Gives Print back to the remembered actions. clearOwnAction also clears itsova's own action
// directly through KGlobalAccel, for use while itsova itself is not running.
void release(KConfig &config, bool clearOwnAction);

} // namespace PrintKey

} // namespace itsova
