// An icon-only tool button with a tooltip and an accessible name.
import QtQuick

import org.kde.plasma.components as PlasmaComponents3

PlasmaComponents3.ToolButton {
    property string iconName
    property string label

    icon.name: iconName
    text: label
    display: PlasmaComponents3.AbstractButton.IconOnly
    Accessible.name: label
    PlasmaComponents3.ToolTip.text: label
    PlasmaComponents3.ToolTip.visible: hovered
    PlasmaComponents3.ToolTip.delay: 500
}
