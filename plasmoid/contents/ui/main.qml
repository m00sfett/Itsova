/*
    The glimply tray widget: lists areas, groups and monitors with their shortcuts and lets the
    user capture, edit, rename and delete them. All work happens in the glimply service; this
    widget talks to it over the session bus (io.github.m00sfett.glimply, /Control).
*/
import QtQuick
import QtQuick.Layouts

import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents3
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.extras as PlasmaExtras
import org.kde.plasma.plasmoid
import org.kde.plasma.workspace.dbus as DBus

PlasmoidItem {
    id: root

    readonly property string service: "io.github.m00sfett.glimply"
    readonly property string iface: "io.github.m00sfett.glimply.Control"
    readonly property bool running: serviceWatcher.registered
    property bool hasLastCapture: false
    property bool flashing: false
    property string lastError: ""

    // The popup must be gone before a capture or the frozen selection picture is taken.
    readonly property int closeDelay: 300

    Plasmoid.icon: flashing ? "io.github.m00sfett.glimply-flash-symbolic" : "io.github.m00sfett.glimply-symbolic"
    Plasmoid.status: running ? PlasmaCore.Types.ActiveStatus : PlasmaCore.Types.PassiveStatus
    toolTipMainText: "glimply"
    toolTipSubText: running ? i18n("Capture fixed areas and monitors") : i18n("glimply is not running")

    ListModel {
        id: targetModel
    }

    function call(member, args, onReply) {
        DBus.SessionBus.asyncCall({
            service: root.service,
            path: "/Control",
            iface: root.iface,
            member: member,
            arguments: args || []
        }, function (reply) {
            if (onReply) {
                // Replies arrive as a pending-reply object whose value may itself wrap the D-Bus
                // value (for example a string as {value: "..."}).
                let value = reply ? reply.value : undefined;
                while (value !== null && typeof value === "object" && "value" in value) {
                    value = value.value;
                }
                onReply(value);
            }
        }, function (error) {
            console.warn("glimply:", member, error && error.message ? error.message : error);
        });
    }

    function refresh() {
        call("Targets", [], function (json) {
            let list = [];
            try {
                list = JSON.parse(json);
            } catch (e) {
                console.warn("glimply: invalid target list", e);
            }
            targetModel.clear();
            for (const target of list) {
                target.section = target.kind === "group" ? i18n("Groups")
                               : target.kind === "area" ? i18n("Areas")
                               : i18n("Monitors");
                targetModel.append(target);
            }
        });
        call("HasLastCapture", [], function (value) {
            root.hasLastCapture = value === true;
        });
    }

    function afterClose(callback) {
        root.expanded = false;
        closeTimer.callback = callback;
        closeTimer.restart();
    }

    Timer {
        id: closeTimer
        property var callback
        interval: root.closeDelay
        onTriggered: if (callback) callback()
    }

    Timer {
        id: flashTimer
        interval: 220
        onTriggered: root.flashing = false
    }

    DBus.DBusServiceWatcher {
        id: serviceWatcher
        busType: DBus.BusType.Session
        watchedService: root.service
        onRegisteredChanged: {
            if (registered) {
                root.call("RegisterWidget");
                root.refresh();
            } else {
                targetModel.clear();
            }
        }
    }

    // The service announces changes and finished captures as D-Bus signals; the watcher calls the
    // matching dbus<Signal> function.
    DBus.SignalWatcher {
        busType: DBus.BusType.Session
        service: root.service
        path: "/Control"
        iface: root.iface
        enabled: root.running

        function dbusChanged() {
            root.refresh();
        }

        function dbusCaptured(success) {
            if (success) {
                root.flashing = true;
                flashTimer.restart();
                root.hasLastCapture = true;
            }
        }
    }

    Component.onCompleted: if (running) {
        call("RegisterWidget");
        refresh();
    }

    onExpandedChanged: if (root.expanded && root.running) root.refresh()

    fullRepresentation: PlasmaExtras.Representation {
        id: dialog

        Layout.minimumWidth: Kirigami.Units.gridUnit * 24
        Layout.preferredWidth: Kirigami.Units.gridUnit * 28
        Layout.minimumHeight: Kirigami.Units.gridUnit * 14
        Layout.preferredHeight: Kirigami.Units.gridUnit * 24
        collapseMarginsHint: true

        header: PlasmaExtras.PlasmoidHeading {
            visible: root.running
            RowLayout {
                anchors.fill: parent
                spacing: Kirigami.Units.smallSpacing

                PlasmaExtras.Heading {
                    Layout.fillWidth: true
                    Layout.leftMargin: Kirigami.Units.smallSpacing
                    level: 1
                    text: "glimply"
                }
                PlasmaComponents3.ToolButton {
                    icon.name: "list-add"
                    text: i18n("Add Area")
                    onClicked: root.afterClose(() => root.call("AddArea"))
                }
                PlasmaComponents3.ToolButton {
                    icon.name: "configure"
                    display: PlasmaComponents3.AbstractButton.IconOnly
                    text: i18n("Configure glimply…")
                    PlasmaComponents3.ToolTip.text: text
                    PlasmaComponents3.ToolTip.visible: hovered
                    onClicked: {
                        root.expanded = false;
                        root.call("Configure", [""]);
                    }
                }
            }
        }

        footer: PlasmaExtras.PlasmoidHeading {
            visible: root.running
            position: PlasmaComponents3.ToolBar.Footer
            RowLayout {
                anchors.fill: parent
                PlasmaComponents3.ToolButton {
                    icon.name: "document-open"
                    text: i18n("Open Last Capture")
                    enabled: root.hasLastCapture
                    onClicked: {
                        root.expanded = false;
                        root.call("OpenLastCapture");
                    }
                }
                PlasmaComponents3.ToolButton {
                    icon.name: "folder-pictures"
                    text: i18n("Open Capture Folder")
                    onClicked: {
                        root.expanded = false;
                        root.call("OpenCaptureFolder");
                    }
                }
                Item {
                    Layout.fillWidth: true
                }
            }
        }

        PlasmaExtras.PlaceholderMessage {
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: !root.running
            iconName: "io.github.m00sfett.glimply"
            text: i18n("glimply is not running")
            explanation: i18n("Shortcuts and captures need the glimply service.")
            helpfulAction: Kirigami.Action {
                icon.name: "media-playback-start"
                text: i18n("Start glimply")
                // Any call starts the service through D-Bus activation.
                onTriggered: root.call("HasLastCapture")
            }
        }

        PlasmaComponents3.ScrollView {
            anchors.fill: parent
            visible: root.running

            contentItem: ListView {
                id: list
                model: targetModel
                clip: true
                spacing: 0
                topMargin: Kirigami.Units.smallSpacing
                bottomMargin: Kirigami.Units.smallSpacing
                highlight: null
                currentIndex: -1

                section.property: "section"
                section.delegate: Kirigami.ListSectionHeader {
                    required property string section
                    width: ListView.view.width
                    label: section
                }

                delegate: TargetRow {
                    width: ListView.view.width
                    onCaptureRequested: id => root.afterClose(() => root.call("Trigger", [id]))
                    onEditRequested: (id, kind) => {
                        if (kind === "area") {
                            root.afterClose(() => root.call("EditArea", [id]));
                        } else {
                            root.expanded = false;
                            root.call("Configure", [id]);
                        }
                    }
                    onRenameRequested: (id, name, done) => root.call("Rename", [id, name], error => done(error || ""))
                    onDeleteRequested: id => root.call("Remove", [id])
                }

                PlasmaExtras.PlaceholderMessage {
                    anchors.centerIn: parent
                    width: parent.width - Kirigami.Units.gridUnit * 4
                    visible: list.count === 0 && root.running
                    text: i18n("Loading…")
                }
            }
        }
    }
}
