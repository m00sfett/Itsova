/*
    Collapsible "Recent" section of the widget: a grid of the newest captures with quick actions.
    Captures with several files expand into a row of their files below the grid.
*/
import QtQuick
import QtQuick.Layouts

import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents3

ColumnLayout {
    id: section

    property var entries: []
    property bool expanded: true
    property bool showAll: false
    property string expandedGroupId: ""

    readonly property int columns: 4
    readonly property int collapsedCount: 8
    readonly property var visibleEntries: showAll ? entries : entries.slice(0, collapsedCount)
    readonly property var expandedGroup: {
        for (const entry of entries) {
            if (entry.id === expandedGroupId && entry.files.length > 1) {
                return entry;
            }
        }
        return null;
    }

    signal action(string action, string entryId, string path)
    signal clearRequested()
    signal expandedToggled(bool open)

    spacing: Kirigami.Units.smallSpacing

    function timeText(iso) {
        const date = new Date(iso);
        if (isNaN(date.getTime())) {
            return "";
        }
        const now = new Date();
        const time = Qt.formatTime(date, Qt.locale().timeFormat(Locale.ShortFormat));
        if (date.toDateString() === now.toDateString()) {
            return time;
        }
        const yesterday = new Date(now.getFullYear(), now.getMonth(), now.getDate() - 1);
        if (date.toDateString() === yesterday.toDateString()) {
            return i18nc("@label capture time", "Yesterday %1", time);
        }
        return Qt.formatDateTime(date, Qt.locale().dateTimeFormat(Locale.ShortFormat));
    }

    function fullTime(iso) {
        const date = new Date(iso);
        return isNaN(date.getTime()) ? iso : Qt.formatDateTime(date, Qt.locale().dateTimeFormat(Locale.LongFormat));
    }

    function tooltipFor(entry) {
        const names = entry.files.map(file => file.name).join("\n");
        return entry.targetName + "\n" + fullTime(entry.timestamp) + "\n" + names;
    }

    // Header row in the style of a list section header, with a disclosure arrow.
    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: Kirigami.Units.smallSpacing
        Layout.rightMargin: Kirigami.Units.smallSpacing
        spacing: Kirigami.Units.smallSpacing

        PlasmaComponents3.ToolButton {
            Layout.fillWidth: true
            text: i18nc("@title section", "Recent")
            Accessible.name: section.expanded ? i18n("Collapse recent captures") : i18n("Expand recent captures")
            contentItem: RowLayout {
                spacing: Kirigami.Units.smallSpacing
                Kirigami.Icon {
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                    source: section.expanded ? "arrow-down" : "arrow-right"
                }
                PlasmaComponents3.Label {
                    Layout.fillWidth: true
                    text: section.entries.length > 0 ? i18nc("@title section", "Recent (%1)", section.entries.length)
                                                     : i18nc("@title section", "Recent")
                    font.bold: true
                    elide: Text.ElideRight
                }
            }
            onClicked: section.expandedToggled(!section.expanded)
        }
        PlasmaComponents3.ToolButton {
            visible: section.expanded && section.entries.length > 0
            icon.name: "edit-clear-history"
            display: PlasmaComponents3.AbstractButton.IconOnly
            text: i18n("Clear History")
            Accessible.name: text
            PlasmaComponents3.ToolTip.text: i18n("Clear the list (image files are kept)")
            PlasmaComponents3.ToolTip.visible: hovered
            onClicked: section.clearRequested()
        }
    }

    PlasmaComponents3.Label {
        Layout.fillWidth: true
        Layout.leftMargin: Kirigami.Units.largeSpacing
        Layout.rightMargin: Kirigami.Units.largeSpacing
        visible: section.expanded && section.entries.length === 0
        text: i18n("No captures yet. Press Print or a target shortcut.")
        font: Kirigami.Theme.smallFont
        opacity: 0.7
        wrapMode: Text.WordWrap
    }

    GridLayout {
        id: grid
        Layout.fillWidth: true
        Layout.leftMargin: Kirigami.Units.largeSpacing
        Layout.rightMargin: Kirigami.Units.largeSpacing
        visible: section.expanded && section.entries.length > 0
        columns: section.columns
        columnSpacing: Kirigami.Units.largeSpacing
        rowSpacing: Kirigami.Units.largeSpacing

        Repeater {
            model: section.visibleEntries
            delegate: RecentTile {
                required property var modelData
                entry: modelData
                Layout.fillWidth: true
                Layout.preferredWidth: (grid.width - grid.columnSpacing * (section.columns - 1)) / section.columns
                caption: section.timeText(modelData.timestamp) + " · " + modelData.targetName
                tooltipText: section.tooltipFor(modelData)
                selected: modelData.id === section.expandedGroupId && fileCount > 1
                onActivated: {
                    if (fileCount > 1) {
                        section.expandedGroupId = section.expandedGroupId === modelData.id ? "" : modelData.id;
                    } else {
                        section.action("open", modelData.id, modelData.files[0].path);
                    }
                }
                onMenuRequested: anchor => {
                    if (fileCount > 1) {
                        groupMenu.entry = modelData;
                        groupMenu.popup(anchor, 0, anchor.height);
                    } else {
                        fileMenu.entryId = modelData.id;
                        fileMenu.path = modelData.files[0].path;
                        fileMenu.popup(anchor, 0, anchor.height);
                    }
                }
            }
        }
    }

    PlasmaComponents3.ToolButton {
        Layout.alignment: Qt.AlignHCenter
        visible: section.expanded && section.entries.length > section.collapsedCount
        text: section.showAll ? i18n("Show fewer") : i18n("Show all (%1)", section.entries.length)
        icon.name: section.showAll ? "go-up" : "go-down"
        onClicked: section.showAll = !section.showAll
    }

    // Files of the selected multi-file capture.
    ColumnLayout {
        Layout.fillWidth: true
        Layout.leftMargin: Kirigami.Units.largeSpacing
        Layout.rightMargin: Kirigami.Units.largeSpacing
        visible: section.expanded && section.expandedGroup !== null
        spacing: Kirigami.Units.smallSpacing

        PlasmaComponents3.Label {
            Layout.fillWidth: true
            text: section.expandedGroup ? i18np("%2: 1 file", "%2: %1 files", section.expandedGroup.files.length,
                                                section.expandedGroup.targetName)
                                        : ""
            font: Kirigami.Theme.smallFont
            elide: Text.ElideRight
        }

        GridLayout {
            id: memberGrid
            Layout.fillWidth: true
            columns: section.columns
            columnSpacing: Kirigami.Units.largeSpacing
            rowSpacing: Kirigami.Units.largeSpacing

            Repeater {
                model: section.expandedGroup ? section.expandedGroup.files : []
                delegate: RecentTile {
                    required property var modelData
                    entry: ({id: section.expandedGroup.id, timestamp: section.expandedGroup.timestamp,
                             targetName: section.expandedGroup.targetName, files: [modelData]})
                    Layout.fillWidth: true
                    Layout.preferredWidth: (memberGrid.width - memberGrid.columnSpacing * (section.columns - 1))
                                           / section.columns
                    caption: modelData.name
                    tooltipText: modelData.path
                    onActivated: section.action("open", section.expandedGroup.id, modelData.path)
                    onMenuRequested: anchor => {
                        fileMenu.entryId = section.expandedGroup.id;
                        fileMenu.path = modelData.path;
                        fileMenu.popup(anchor, 0, anchor.height);
                    }
                }
            }
        }
    }

    PlasmaComponents3.Menu {
        id: fileMenu
        property string entryId
        property string path

        PlasmaComponents3.MenuItem {
            icon.name: "document-open"
            text: i18n("Open")
            onTriggered: section.action("open", fileMenu.entryId, fileMenu.path)
        }
        PlasmaComponents3.MenuItem {
            icon.name: "document-edit"
            text: i18n("Edit in Window")
            onTriggered: section.action("edit", fileMenu.entryId, fileMenu.path)
        }
        PlasmaComponents3.MenuItem {
            icon.name: "edit-copy"
            text: i18n("Copy")
            onTriggered: section.action("copy", fileMenu.entryId, fileMenu.path)
        }
        PlasmaComponents3.MenuItem {
            icon.name: "folder-open"
            text: i18n("Show in Folder")
            onTriggered: section.action("folder", fileMenu.entryId, fileMenu.path)
        }
        PlasmaComponents3.MenuItem {
            icon.name: "window-pin"
            text: i18n("Pin to Screen")
            onTriggered: section.action("pin", fileMenu.entryId, fileMenu.path)
        }
        PlasmaComponents3.MenuSeparator {}
        PlasmaComponents3.MenuItem {
            icon.name: "user-trash"
            text: i18n("Move to Trash")
            onTriggered: section.action("trash", fileMenu.entryId, fileMenu.path)
        }
    }

    PlasmaComponents3.Menu {
        id: groupMenu
        property var entry: null

        PlasmaComponents3.MenuItem {
            icon.name: "view-list-icons"
            text: section.expandedGroupId === (groupMenu.entry ? groupMenu.entry.id : "") ? i18n("Hide Files")
                                                                                            : i18n("Show Files")
            onTriggered: section.expandedGroupId = section.expandedGroupId === groupMenu.entry.id ? ""
                                                                                                  : groupMenu.entry.id
        }
        PlasmaComponents3.MenuItem {
            icon.name: "folder-open"
            text: i18n("Show in Folder")
            onTriggered: section.action("folder", groupMenu.entry.id, "")
        }
        PlasmaComponents3.MenuSeparator {}
        PlasmaComponents3.MenuItem {
            icon.name: "user-trash"
            text: i18n("Move All to Trash")
            onTriggered: section.action("trash", groupMenu.entry.id, "")
        }
    }
}
