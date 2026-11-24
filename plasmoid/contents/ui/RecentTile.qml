/*
    One thumbnail tile of the Recent captures grid. A capture with several files shows a stacked
    edge and a count badge. Click or Enter opens; right click, the Menu key or the corner button
    opens the action menu.
*/
import QtQuick
import QtQuick.Layouts

import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents3

FocusScope {
    id: tile

    // {id, timestamp, targetName, files: [{path, name, thumbnail}]} or a single file wrapped the same way.
    required property var entry
    property bool selected: false
    property string caption: ""
    property string tooltipText: caption
    readonly property int fileCount: entry && entry.files ? entry.files.length : 0
    readonly property string thumbnail: fileCount > 0 ? (entry.files[0].thumbnail || "") : ""

    signal activated()
    signal menuRequested(Item anchor)

    implicitHeight: well.height + captionColumn.implicitHeight + Kirigami.Units.smallSpacing
    activeFocusOnTab: true

    Accessible.role: Accessible.Button
    Accessible.name: caption + (fileCount > 1 ? " " + i18np("(1 file)", "(%1 files)", fileCount) : "")
    Accessible.onPressAction: tile.activated()

    Keys.onReturnPressed: tile.activated()
    Keys.onEnterPressed: tile.activated()
    Keys.onSpacePressed: tile.activated()
    Keys.onMenuPressed: tile.menuRequested(menuButton)

    // Stacked edges behind the well for captures with several files.
    Repeater {
        model: tile.fileCount > 1 ? 2 : 0
        Rectangle {
            required property int index
            x: (2 - index) * 3
            y: -(2 - index) * 3 + 6
            width: well.width - 6
            height: well.height - 6
            radius: Kirigami.Units.cornerRadius
            color: Kirigami.Theme.alternateBackgroundColor
            border.width: 1
            border.color: Qt.alpha(Kirigami.Theme.textColor, 0.15)
        }
    }

    Rectangle {
        id: well
        width: parent.width
        height: Math.round(width * 10 / 16)
        radius: Kirigami.Units.cornerRadius
        color: Kirigami.Theme.alternateBackgroundColor
        border.width: tile.selected || tile.activeFocus || mouse.containsMouse ? 2 : 1
        border.color: tile.selected || tile.activeFocus ? Kirigami.Theme.highlightColor
                    : mouse.containsMouse ? Qt.alpha(Kirigami.Theme.highlightColor, 0.6)
                                          : Qt.alpha(Kirigami.Theme.textColor, 0.15)

        Behavior on border.color {
            ColorAnimation { duration: Kirigami.Units.shortDuration }
        }

        Image {
            anchors.fill: parent
            anchors.margins: 3
            source: tile.thumbnail.length > 0 ? "file://" + tile.thumbnail : ""
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            cache: false
            smooth: true
            mipmap: true
        }

        // Count badge for captures with several files.
        Rectangle {
            visible: tile.fileCount > 1
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: Kirigami.Units.smallSpacing
            width: Math.max(height, badgeLabel.implicitWidth + Kirigami.Units.smallSpacing * 2)
            height: badgeLabel.implicitHeight + 2
            radius: height / 2
            color: Kirigami.Theme.highlightColor

            PlasmaComponents3.Label {
                id: badgeLabel
                anchors.centerIn: parent
                text: tile.fileCount
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.highlightedTextColor
            }
        }

        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: Qt.PointingHandCursor
            onClicked: mouseEvent => {
                tile.forceActiveFocus();
                if (mouseEvent.button === Qt.RightButton) {
                    tile.menuRequested(menuButton);
                } else {
                    tile.activated();
                }
            }
        }

        PlasmaComponents3.ToolButton {
            id: menuButton
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 2
            visible: mouse.containsMouse || hovered || tile.activeFocus || down
            icon.name: "overflow-menu"
            display: PlasmaComponents3.AbstractButton.IconOnly
            text: i18n("Actions")
            Accessible.name: i18n("Actions for %1", tile.caption)
            PlasmaComponents3.ToolTip.text: text
            PlasmaComponents3.ToolTip.visible: hovered
            onClicked: tile.menuRequested(menuButton)
        }

        PlasmaComponents3.ToolTip.text: tile.tooltipText
        PlasmaComponents3.ToolTip.visible: mouse.containsMouse && !menuButton.hovered
        PlasmaComponents3.ToolTip.delay: Kirigami.Units.toolTipDelay
    }

    ColumnLayout {
        id: captionColumn
        anchors.top: well.bottom
        anchors.topMargin: Kirigami.Units.smallSpacing
        width: parent.width
        spacing: 0

        PlasmaComponents3.Label {
            Layout.fillWidth: true
            text: tile.caption
            font: Kirigami.Theme.smallFont
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
            maximumLineCount: 1
        }
    }
}
