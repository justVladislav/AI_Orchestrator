pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

// PC-1D: the opt-in, LOCAL preview of the Project Context Snapshot for the conversation's project. It shows
// metadata only (fingerprint, token budget, one row per section); the rendered text, file names and paths never
// reach QML. Previewing sends nothing: the user may separately quote and attach these exact reviewed bytes to one
// next Chat message. The preview is built only when the user presses Preview, off the UI thread, by the Backend
// (backend.projectContext); Project Memory is OFF by default.
Rectangle {
    id: preview

    objectName: "chatProjectContextPreview"

    required property var hostWindow
    required property var colors

    // backend.projectContext (ProjectContextController) or null.
    property var controller: null
    // The conversation's project; empty when the conversation has none.
    property string projectId: ""

    // Fingerprint of the snapshot already armed for the NEXT message (empty when none): only used to say so here.
    property string attachedFingerprint: ""
    // True for a moment after the full fingerprint was copied.
    property bool copied: false

    signal previewRequested(bool includeProjectMemory)
    signal clearRequested()
    signal attachRequested()

    readonly property bool hasProject: projectId.length > 0
    readonly property bool busy: controller !== null && controller.busy
    // The controller may still hold the result for another project: show only what belongs to THIS project.
    readonly property bool matches: controller !== null && hasProject && controller.projectId === projectId
    readonly property bool showSnapshot: matches && controller.hasSnapshot
    readonly property bool showError: matches && !controller.busy && !controller.hasSnapshot
                                      && controller.errorCode.length > 0
    readonly property bool attached: showSnapshot && attachedFingerprint.length > 0
                                     && attachedFingerprint === controller.fingerprint

    function authorityText(authority) {
        if (authority === "derived")
            return qsTr("derived, not authoritative")
        if (authority === "human_curated")
            return qsTr("curated note")
        return qsTr("authoritative")
    }

    function sectionText(row) {
        let text = qsTr("%1 - %2 - %3 of %4 tokens - %5 items")
            .arg(row.label).arg(preview.authorityText(row.authority))
            .arg(row.tokens).arg(row.limitTokens).arg(row.included)
        if (row.omitted > 0)
            text += qsTr(", %1 omitted").arg(row.omitted)
        return text
    }

    implicitHeight: content.implicitHeight + hostWindow.dp(22)
    radius: colors.cornerRadiusSmall
    color: colors.inputBackground
    border.width: 1
    border.color: colors.border

    ColumnLayout {
        id: content
        anchors.fill: parent
        anchors.margins: preview.hostWindow.dp(11)
        spacing: preview.hostWindow.dp(8)

        Text {
            Layout.fillWidth: true
            text: qsTr("Project context preview")
            color: preview.colors.textPrimary
            font.pixelSize: preview.hostWindow.dp(12)
            font.weight: Font.DemiBold
        }

        Text {
            Layout.fillWidth: true
            text: preview.hasProject
                  ? qsTr("Preview what the Backend can attach. Previewing alone sends nothing; attachment is confirmed separately with a token and cost estimate.")
                  : qsTr("Select a conversation with a project to preview its context.")
            color: preview.colors.textMuted
            wrapMode: Text.WordWrap
            font.pixelSize: preview.hostWindow.dp(10.5)
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: preview.hostWindow.dp(8)

            Basic.Switch {
                id: memorySwitch
                objectName: "chatProjectContextMemorySwitch"
                enabled: preview.hasProject && !preview.busy
                checked: false
                Accessible.name: qsTr("Include Project Memory")
                // The empty contentItem below leaves the Switch no width of its own, so without an explicit size the
                // label that follows it in the row is drawn over the toggle.
                padding: 0
                implicitWidth: preview.hostWindow.dp(38)
                implicitHeight: preview.hostWindow.dp(20)
                indicator: Rectangle {
                    implicitWidth: preview.hostWindow.dp(38)
                    implicitHeight: preview.hostWindow.dp(20)
                    radius: height / 2
                    color: memorySwitch.checked ? preview.colors.accent : preview.colors.buttonBackground
                    border.width: 1
                    border.color: memorySwitch.checked ? preview.colors.accent : preview.colors.borderStrong
                    Rectangle {
                        width: preview.hostWindow.dp(14)
                        height: width
                        radius: width / 2
                        x: memorySwitch.checked ? parent.width - width - preview.hostWindow.dp(3)
                                                : preview.hostWindow.dp(3)
                        anchors.verticalCenter: parent.verticalCenter
                        color: preview.colors.textOnAccent
                    }
                }
                contentItem: Item { }
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("Include Project Memory (derived, not authoritative)")
                color: preview.colors.textSecondary
                wrapMode: Text.WordWrap
                font.pixelSize: preview.hostWindow.dp(10.5)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: preview.hostWindow.dp(8)

            ChatActionButton {
                objectName: "chatProjectContextPreviewButton"
                text: qsTr("Preview")
                enabled: preview.hasProject && !preview.busy
                hostWindow: preview.hostWindow
                colors: preview.colors
                onClicked: preview.previewRequested(memorySwitch.checked)
            }

            ChatActionButton {
                text: qsTr("Clear")
                visible: preview.showSnapshot || preview.showError
                hostWindow: preview.hostWindow
                colors: preview.colors
                onClicked: preview.clearRequested()
            }
        }

        // Right under Preview, so it is on screen without scrolling the (long) snapshot details below.
        ChatActionButton {
            objectName: "chatProjectContextAttachButton"
            Layout.alignment: Qt.AlignLeft
            visible: preview.showSnapshot
            text: qsTr("Attach to next message...")
            primary: true
            enabled: !preview.busy
            hostWindow: preview.hostWindow
            colors: preview.colors
            onClicked: preview.attachRequested()
        }

        Text {
            objectName: "chatProjectContextAttachedNote"
            Layout.fillWidth: true
            visible: preview.attached
            text: qsTr("This snapshot is attached to the next message (shown above the message box).")
            color: preview.colors.success
            wrapMode: Text.WordWrap
            font.pixelSize: preview.hostWindow.dp(10.5)
        }

        Text {
            Layout.fillWidth: true
            visible: preview.busy
            text: qsTr("Building the preview - the workspace is being scanned...")
            color: preview.colors.textMuted
            wrapMode: Text.WordWrap
            font.pixelSize: preview.hostWindow.dp(10.5)
        }

        Text {
            objectName: "chatProjectContextError"
            Layout.fillWidth: true
            visible: preview.showError
            text: preview.showError
                  ? qsTr("Preview failed: %1 (%2)").arg(preview.controller.errorText).arg(preview.controller.errorCode)
                  : ""
            color: preview.colors.error
            wrapMode: Text.WordWrap
            font.pixelSize: preview.hostWindow.dp(10.5)
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: preview.showSnapshot
            spacing: preview.hostWindow.dp(4)

            // The figures below are read-only but selectable, so they can be copied out and written down.
            TextEdit {
                objectName: "chatProjectContextSummary"
                Layout.fillWidth: true
                readOnly: true
                selectByMouse: true
                text: preview.showSnapshot
                      ? qsTr("Fingerprint %1 - %2 of %3 estimated tokens%4")
                            .arg(preview.controller.shortFingerprint)
                            .arg(preview.controller.usedTokens)
                            .arg(preview.controller.totalTokens)
                            .arg(preview.controller.truncated ? qsTr(" - trimmed to the budget") : "")
                      : ""
                color: preview.colors.textPrimary
                selectionColor: preview.colors.accent
                selectedTextColor: preview.colors.textOnAccent
                wrapMode: TextEdit.Wrap
                font.pixelSize: preview.hostWindow.dp(11)
            }

            Repeater {
                model: preview.showSnapshot ? preview.controller.sections : []

                delegate: TextEdit {
                    id: sectionRow
                    required property var modelData

                    Layout.fillWidth: true
                    readOnly: true
                    selectByMouse: true
                    text: preview.sectionText(sectionRow.modelData)
                    color: sectionRow.modelData.truncated ? preview.colors.textPrimary : preview.colors.textSecondary
                    selectionColor: preview.colors.accent
                    selectedTextColor: preview.colors.textOnAccent
                    wrapMode: TextEdit.Wrap
                    font.pixelSize: preview.hostWindow.dp(10.5)
                }
            }

            TextEdit {
                Layout.fillWidth: true
                readOnly: true
                selectByMouse: true
                text: preview.showSnapshot
                      ? qsTr("Workspace %1 - %2 sources").arg(preview.controller.workspaceFingerprint)
                            .arg(preview.controller.sourceCount)
                      : ""
                color: preview.colors.textMuted
                selectionColor: preview.colors.accent
                selectedTextColor: preview.colors.textOnAccent
                wrapMode: TextEdit.Wrap
                font.pixelSize: preview.hostWindow.dp(10)
            }

            Text {
                Layout.fillWidth: true
                Layout.topMargin: preview.hostWindow.dp(4)
                text: qsTr("Full fingerprint (select it, or use Copy):")
                color: preview.colors.textMuted
                wrapMode: Text.WordWrap
                font.pixelSize: preview.hostWindow.dp(10)
            }

            TextEdit {
                id: fingerprintField
                objectName: "chatProjectContextFingerprint"
                Layout.fillWidth: true
                readOnly: true
                selectByMouse: true
                text: preview.showSnapshot ? preview.controller.fingerprint : ""
                color: preview.colors.textPrimary
                selectionColor: preview.colors.accent
                selectedTextColor: preview.colors.textOnAccent
                wrapMode: TextEdit.WrapAnywhere
                font.pixelSize: preview.hostWindow.dp(10.5)
                Accessible.name: qsTr("Full context fingerprint")
            }

            // One button per row: the panel is narrow, and two buttons side by side would be squeezed over each other.
            ChatActionButton {
                objectName: "chatProjectContextCopyFingerprint"
                Layout.alignment: Qt.AlignLeft
                text: preview.copied ? qsTr("Copied") : qsTr("Copy fingerprint")
                hostWindow: preview.hostWindow
                colors: preview.colors
                onClicked: {
                    fingerprintField.selectAll()
                    fingerprintField.copy()
                    preview.copied = true
                    copiedTimer.restart()
                }
            }

        }

        Timer {
            id: copiedTimer
            interval: 1500
            onTriggered: preview.copied = false
        }
    }
}
