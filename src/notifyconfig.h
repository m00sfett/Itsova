#pragma once

#include <QStandardPaths>
#include <QString>

namespace glimply
{

// KNotification looks events up in <componentName>.notifyrc, so the installed file must carry the
// component's name (#91).
inline QString notifyComponentName()
{
    return QStringLiteral("io.github.m00sfett.glimply");
}

inline QString notifyRcRelativePath()
{
    return QStringLiteral("knotifications6/") + notifyComponentName() + QStringLiteral(".notifyrc");
}

// Installed builds ship the file; uninstalled development builds fall back to system events.
inline bool haveNotifyEvents()
{
    static const bool have =
        !QStandardPaths::locate(QStandardPaths::GenericDataLocation, notifyRcRelativePath()).isEmpty();
    return have;
}

} // namespace glimply
