/*
    One target in the glimply widget. Clicking the row captures it; the buttons on the right edit,
    rename and delete areas and groups. Renaming and deleting happen inline, without dialogs.
*/
import QtQuick
import QtQuick.Layouts

import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents3

PlasmaComponents3.ItemDelegate {
    id: row

    required property string targetId
    required property string kind
    required property string name
    required property string detail
    required property string shortcut
    required property bool available

    // "normal", "rename" or "confirmDelete"
    property string mode: "normal"
    property string error: ""

    readonly property bool editable: kind === "area" || kind === "group"

    signal captureRequested(string id)
    signal editRequested(string id, string kind)
    signal renameRequested(string id, string name, var done)
    signal deleteRequested(string id)

    hoverEnabled: mode === "normal"
    down: mode === "normal" && pressed
    onClicked: if (mode === "normal" && available) captureRequested(targetId)

    Accessible.name: shortcut.length > 0 ? i18nc("target name, shortcut", "%1, %2", name, shortcut) : name
    Accessible.description: i18n("Press to capture")

    function startRename() {
        error = "";
        mode = "rename";
        nameField.text = name;
        nameField.forceActiveFocus();
        nameField.selectAll();
    }

    function commitRename() {
        const wanted = nameField.text.trim();
        if (wanted === name) {
            mode = "normal";
            return;
        }
        renameRequested(targetId, wanted, function (message) {
            if (message.length > 0) {
                error = message;
            } else {
                error = "";
                mode = "normal";
            }
        });
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        RowLayout {
            spacing: Kirigami.Units.smallSpacing
            Layout.fillWidth: true

            Kirigami.Icon {
                Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                source: row.kind === "group" ? "view-group"
                      : row.kind === "area" ? "transform-crop"
                      : "monitor-symbolic"
                opacity: row.available ? 1 : 0.5
            }

            // Name and detail, or the rename field.
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                visible: row.mode !== "rename"

                PlasmaComponents3.Label {
                    Layout.fillWidth: true
                    text: row.mode === "confirmDelete" ? i18n("Delete “%1”?", row.name) : row.name
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                }
                PlasmaComponents3.Label {
                    Layout.fillWidth: true
                    text: row.mode === "confirmDelete"
                          ? (row.kind === "area" ? i18n("Its shortcut is cleared. Saved captures are kept.")
                                                 : i18n("Its targets stay. Only the group and its shortcut go."))
                          : row.detail
                    visible: text.length > 0
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    font: Kirigami.Theme.smallFont
                    opacity: 0.7
                }
            }

            PlasmaComponents3.TextField {
                id: nameField
                Layout.fillWidth: true
                visible: row.mode === "rename"
                Accessible.name: i18n("New name")
                onAccepted: row.commitRename()
                Keys.onEscapePressed: {
                    row.error = "";
                    row.mode = "normal";
                }
            }

            PlasmaComponents3.Label {
                visible: row.mode === "normal" && row.shortcut.length > 0
                text: row.shortcut
                font: Kirigami.Theme.smallFont
                opacity: 0.7
                Layout.rightMargin: Kirigami.Units.smallSpacing
            }

            // Normal actions
            RowLayout {
                visible: row.mode === "normal"
                spacing: 0

                RowButton {
                    iconName: "camera-photo"
                    label: i18n("Capture now")
                    enabled: row.available
                    onClicked: row.captureRequested(row.targetId)
                }
                RowButton {
                    visible: row.editable
                    iconName: row.kind === "area" ? "edit-select" : "document-edit"
                    label: row.kind === "area" ? i18n("Edit area on screen") : i18n("Edit group…")
                    enabled: row.kind !== "area" || row.available
                    onClicked: row.editRequested(row.targetId, row.kind)
                }
                RowButton {
                    visible: row.editable
                    iconName: "edit-rename"
                    label: i18n("Rename")
                    onClicked: row.startRename()
                }
                RowButton {
                    visible: row.editable
                    iconName: "edit-delete"
                    label: i18n("Delete")
                    onClicked: row.mode = "confirmDelete"
                }
            }

            // Rename actions
            RowLayout {
                visible: row.mode === "rename"
                spacing: 0
                RowButton {
                    iconName: "dialog-ok-apply"
                    label: i18n("Apply")
                    onClicked: row.commitRename()
                }
                RowButton {
                    iconName: "dialog-cancel"
                    label: i18n("Cancel")
                    onClicked: {
                        row.error = "";
                        row.mode = "normal";
                    }
                }
            }

            // Delete confirmation
            RowLayout {
                visible: row.mode === "confirmDelete"
                spacing: Kirigami.Units.smallSpacing
                PlasmaComponents3.Button {
                    icon.name: "edit-delete"
                    text: i18n("Delete")
                    onClicked: {
                        row.mode = "normal";
                        row.deleteRequested(row.targetId);
                    }
                }
                PlasmaComponents3.Button {
                    text: i18n("Cancel")
                    onClicked: row.mode = "normal"
                }
            }
        }

        PlasmaComponents3.Label {
            Layout.fillWidth: true
            Layout.leftMargin: Kirigami.Units.iconSizes.medium + Kirigami.Units.smallSpacing
            visible: row.error.length > 0
            text: row.error
            color: Kirigami.Theme.negativeTextColor
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
        }
    }
}
