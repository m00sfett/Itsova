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
    property int delaySeconds: 0
    property bool keepDelayActive: false
    property bool countdownActive: false
    property int activeIntervalCount: 0
    property string lastError: ""
    property var historyEntries: []
    // Screen recording (#30): idle, starting, recording, paused or finishing.
    property string recordingState: "idle"
    property real recordingElapsedMs: 0
    property string recordingTarget: ""
    property bool recordingCanPause: true
    readonly property bool recording: recordingState !== "idle"

    // The popup must be gone before a capture or the frozen selection picture is taken.
    readonly property int closeDelay: 300

    Plasmoid.icon: recordingState === "paused" ? "io.github.m00sfett.glimply-tray-recording-paused"
                 : recording ? "io.github.m00sfett.glimply-tray-recording"
                 : flashing ? "io.github.m00sfett.glimply-tray-flash"
                 : activeIntervalCount > 0 ? "io.github.m00sfett.glimply-tray-interval"
                                           : "io.github.m00sfett.glimply-tray"
    Plasmoid.status: running ? PlasmaCore.Types.ActiveStatus : PlasmaCore.Types.PassiveStatus
    toolTipMainText: "glimply"
    toolTipSubText: !running ? i18n("glimply is not running")
                  : recording ? recordingStatusText()
                  : activeIntervalCount > 0
                    ? i18np("1 interval capture active", "%1 interval captures active", activeIntervalCount)
                    : i18n("Capture fixed areas and monitors")

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
        call("DelaySeconds", [], function (value) {
            root.delaySeconds = typeof value === "number" ? value : 0;
        });
        call("KeepDelayActive", [], function (value) {
            root.keepDelayActive = value === true;
        });
        call("CountdownActive", [], function (value) {
            root.countdownActive = value === true;
        });
        refreshHistory();
        refreshRecording();
        call("Intervals", [], function (json) {
            let list = [];
            try {
                list = JSON.parse(json);
            } catch (e) {
                list = [];
            }
            root.activeIntervalCount = Array.isArray(list) ? list.length : 0;
        });
    }

    function formatElapsed(ms) {
        const total = Math.max(0, Math.floor(ms / 1000));
        const hours = Math.floor(total / 3600);
        const minutes = Math.floor(total / 60) % 60;
        const seconds = String(total % 60).padStart(2, "0");
        return hours > 0 ? hours + ":" + String(minutes).padStart(2, "0") + ":" + seconds
                         : minutes + ":" + seconds;
    }

    function recordingStatusText() {
        switch (recordingState) {
        case "starting":
            return i18n("Starting the recording of %1…", recordingTarget);
        case "paused":
            return i18n("Recording of %1 paused at %2", recordingTarget, formatElapsed(recordingElapsedMs));
        case "finishing":
            return i18n("Saving the recording of %1…", recordingTarget);
        default:
            return i18n("Recording %1: %2", recordingTarget, formatElapsed(recordingElapsedMs));
        }
    }

    function refreshRecording() {
        call("RecordingState", [], function (json) {
            let state = {};
            try {
                state = JSON.parse(json);
            } catch (e) {
                state = {};
            }
            root.recordingState = state.state || "idle";
            root.recordingElapsedMs = state.elapsedMs || 0;
            root.recordingTarget = state.targetName || "";
            root.recordingCanPause = state.canPause !== false;
        });
    }

    function refreshHistory() {
        call("History", [], function (json) {
            let list = [];
            try {
                list = JSON.parse(json);
            } catch (e) {
                console.warn("glimply: invalid history", e);
            }
            root.historyEntries = Array.isArray(list) ? list : [];
        });
    }

    function historyAction(action, entryId, path) {
        if (action !== "copy" && action !== "trash") {
            root.expanded = false;
        }
        call("HistoryAction", [action, entryId, path], function (error) {
            if (error) {
                console.warn("glimply:", error);
            }
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
                root.historyEntries = [];
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

        function dbusIntervalsChanged() {
            root.refresh();
        }

        function dbusHistoryChanged() {
            root.refreshHistory();
        }

        function dbusRecordingChanged() {
            root.refreshRecording();
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

    // While recording, a click on the icon stops it (Q6) instead of opening the popup.
    compactRepresentation: MouseArea {
        property bool wasExpanded: false
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true
        Accessible.name: root.recording ? i18n("Stop recording") : "glimply"
        Accessible.role: Accessible.Button
        onPressed: wasExpanded = root.expanded
        onClicked: {
            if (root.recording && root.recordingState !== "finishing") {
                root.call("StopRecording");
            } else {
                root.expanded = !wasExpanded;
            }
        }

        Kirigami.Icon {
            anchors.fill: parent
            source: Plasmoid.icon
            active: parent.containsMouse
        }
    }

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
                    id: delayButton
                    icon.name: root.countdownActive ? "dialog-cancel" : "chronometer"
                    text: root.countdownActive ? i18n("Cancel")
                        : root.delaySeconds > 0 ? i18n("%1 s", root.delaySeconds)
                        : i18n("Delay")
                    checkable: !root.countdownActive
                    checked: !root.countdownActive && root.delaySeconds > 0
                    onClicked: {
                        if (root.countdownActive) {
                            root.call("CancelCountdown");
                        } else if (delayMenu.visible) {
                            delayMenu.close();
                        } else {
                            delayMenu.popup(0, delayButton.height);
                        }
                    }

                    PlasmaComponents3.Menu {
                        id: delayMenu
                        PlasmaComponents3.MenuItem {
                            text: i18n("No delay")
                            checkable: true
                            checked: root.delaySeconds === 0
                            onTriggered: root.call("SetDelay", [0, false])
                        }
                        PlasmaComponents3.MenuSeparator {}
                        PlasmaComponents3.MenuItem {
                            text: i18n("3 seconds")
                            checkable: true
                            checked: root.delaySeconds === 3
                            onTriggered: root.call("SetDelay", [3, root.keepDelayActive])
                        }
                        PlasmaComponents3.MenuItem {
                            text: i18n("5 seconds")
                            checkable: true
                            checked: root.delaySeconds === 5
                            onTriggered: root.call("SetDelay", [5, root.keepDelayActive])
                        }
                        PlasmaComponents3.MenuItem {
                            text: i18n("10 seconds")
                            checkable: true
                            checked: root.delaySeconds === 10
                            onTriggered: root.call("SetDelay", [10, root.keepDelayActive])
                        }
                        PlasmaComponents3.MenuSeparator {}
                        PlasmaComponents3.MenuItem {
                            text: i18n("Keep delay active")
                            checkable: true
                            checked: root.keepDelayActive
                            onTriggered: root.call("SetDelay", [checked ? (root.delaySeconds > 0 ? root.delaySeconds : 3) : 0, checked])
                        }
                    }
                }
                PlasmaComponents3.ToolButton {
                    icon.name: "media-record"
                    text: i18n("Record")
                    enabled: !root.recording
                    onClicked: root.afterClose(() => root.call("OpenRecordOverlay"))
                }
                PlasmaComponents3.ToolButton {
                    icon.name: "color-picker"
                    display: PlasmaComponents3.AbstractButton.IconOnly
                    text: i18n("Pick Color")
                    PlasmaComponents3.ToolTip.text: text
                    PlasmaComponents3.ToolTip.visible: hovered
                    onClicked: root.afterClose(() => root.call("PickColor"))
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
                    icon.name: "document-edit"
                    text: i18n("Edit Last Capture")
                    enabled: root.hasLastCapture
                    onClicked: {
                        root.expanded = false;
                        root.call("EditLastCapture");
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

        ColumnLayout {
            anchors.fill: parent
            spacing: 0
            visible: root.running

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: Kirigami.Units.smallSpacing
                visible: root.recording
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                    source: root.recordingState === "paused" ? "media-playback-pause" : "media-record"
                }
                PlasmaComponents3.Label {
                    Layout.fillWidth: true
                    text: root.recordingStatusText()
                    elide: Text.ElideMiddle
                    font.features: { "tnum": 1 }
                }
                PlasmaComponents3.ToolButton {
                    visible: root.recordingCanPause && (root.recordingState === "recording" || root.recordingState === "paused")
                    icon.name: root.recordingState === "paused" ? "media-record" : "media-playback-pause"
                    text: root.recordingState === "paused" ? i18n("Resume") : i18n("Pause")
                    onClicked: root.call("PauseRecording", [root.recordingState !== "paused"])
                }
                PlasmaComponents3.ToolButton {
                    icon.name: "media-playback-stop"
                    text: i18n("Stop")
                    enabled: root.recordingState !== "finishing"
                    onClicked: root.call("StopRecording")
                }
            }

            Kirigami.Separator {
                Layout.fillWidth: true
                visible: root.recording
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: Kirigami.Units.smallSpacing
                visible: root.activeIntervalCount > 0
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                    source: "chronometer"
                }
                PlasmaComponents3.Label {
                    Layout.fillWidth: true
                    text: i18np("1 interval capture active", "%1 interval captures active", root.activeIntervalCount)
                    font: Kirigami.Theme.smallFont
                }
                PlasmaComponents3.ToolButton {
                    icon.name: "media-playback-stop"
                    text: i18n("Stop All")
                    onClicked: root.call("StopAllIntervals")
                }
            }

            Kirigami.Separator {
                Layout.fillWidth: true
                visible: root.activeIntervalCount > 0
            }

            PlasmaComponents3.ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true

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

                    footer: RecentSection {
                        width: ListView.view.width
                        entries: root.historyEntries
                        expanded: Plasmoid.configuration.recentExpanded
                        onExpandedToggled: value => Plasmoid.configuration.recentExpanded = value
                        onAction: (action, entryId, path) => root.historyAction(action, entryId, path)
                        onClearRequested: root.call("ClearHistory")
                    }

                    delegate: TargetRow {
                        width: ListView.view.width
                        onCaptureRequested: id => root.afterClose(() => root.call("Trigger", [id]))
                        onToggleIntervalRequested: (id, active) => {
                            if (active) {
                                root.call("StopInterval", [id]);
                            } else {
                                root.afterClose(() => root.call("StartInterval", [id, 0, 0, 0, false]));
                            }
                        }
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
}
