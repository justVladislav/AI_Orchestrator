pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

// AG-17.7C-3: the stored audit of a Run that was created by converting a legacy roster Run to the
// Manifest workflow. Strictly a RENDERER of `RunController.legacyConversionRecord(runId)`: the
// record is immutable, Backend-validated and read once per selected Run; nothing here edits,
// confirms or re-derives it. The panel exists only for a Run that has a record - an ordinary Run
// shows nothing - and a record that cannot be read is shown as a fault of its own (with the
// Backend's code and message), never as a partial record and never as an empty section.
Rectangle {
    id: panel
    objectName: "legacyConversionRecordPanel"

    required property var hostWindow
    required property var colors
    required property var runsController
    required property string runId

    // {state: "none" | "present" | "unreadable", ...} exactly as the Backend returned it.
    property var record: ({ state: "none" })
    readonly property bool present: panel.record.state === "present"
    readonly property bool unreadable: panel.record.state === "unreadable"
    readonly property var droppedSettings: panel.present ? (panel.record.dropped || []) : []

    // The record is immutable, so it is read when the selected Run changes - never on every
    // progress event of a running Run.
    function reload() {
        panel.record = panel.runId.length > 0 && panel.runsController
                ? panel.runsController.legacyConversionRecord(panel.runId)
                : ({ state: "none" })
    }
    onRunIdChanged: panel.reload()
    Component.onCompleted: panel.reload()

    // Presentation only: the Backend's machine values in words.
    function reasonText(reasonCode) {
        switch (reasonCode) {
        case "legacy_roster.converted_lone_workspace_lead":
            return qsTr("A lone workspace agent was re-launched as a single-assignment Manifest Run.")
        case "legacy_roster.converted_proposal_roster":
            return qsTr("A proposal roster (a planning lead and workspace workers) was re-launched as a Manifest roster.")
        default:
            return String(reasonCode)
        }
    }

    function settingLabel(setting) {
        switch (setting) {
        case "maxAttempts": return qsTr("Attempts per task")
        case "fallback": return qsTr("Provider fallback")
        case "dependencies": return qsTr("Declared dependencies")
        case "instructions": return qsTr("Instructions")
        default: return String(setting)
        }
    }

    function shortFingerprint(value) {
        const text = String(value || "")
        return text.length > 12 ? text.substring(0, 12) + "…" : text
    }

    visible: panel.present || panel.unreadable
    Layout.fillWidth: true
    Layout.preferredHeight: visible ? recordColumn.implicitHeight + panel.hostWindow.dp(20) : 0
    radius: panel.colors.cornerRadiusSmall
    color: panel.unreadable ? panel.colors.warningSoft : panel.colors.surface
    border.width: 1
    border.color: panel.unreadable ? panel.colors.warning : panel.colors.border

    ColumnLayout {
        id: recordColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: panel.hostWindow.dp(11)
        anchors.rightMargin: panel.hostWindow.dp(11)
        spacing: panel.hostWindow.dp(6)

        // ── A record that cannot be read: a fault, shown on its own ─────────────────────
        Text {
            objectName: "conversionRecordUnreadable"
            Layout.fillWidth: true
            visible: panel.unreadable
            text: qsTr("The conversion record of this Run could not be read, so none of it is shown.")
            color: panel.colors.textPrimary
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(11)
            font.weight: Font.DemiBold
        }
        Text {
            objectName: "conversionRecordFault"
            Layout.fillWidth: true
            visible: panel.unreadable
            text: panel.unreadable
                  ? qsTr("%1: %2").arg(String(panel.record.errorCode || "")).arg(String(panel.record.errorMessage || ""))
                  : ""
            textFormat: Text.PlainText
            color: panel.colors.warning
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(9.5)
        }

        // ── A record: where this Run came from and how the conversion was decided ──────
        Text {
            Layout.fillWidth: true
            visible: panel.present
            text: qsTr("Converted from a legacy Run")
            color: panel.colors.textPrimary
            font.pixelSize: panel.hostWindow.dp(13)
            font.weight: Font.DemiBold
        }
        Text {
            objectName: "conversionRecordSource"
            Layout.fillWidth: true
            visible: panel.present
            text: panel.present
                  ? (panel.record.sourceRunAvailable
                     ? qsTr("Source Run: %1 (%2)").arg(String(panel.record.sourceRunTitle || ""))
                           .arg(String(panel.record.sourceRunId || ""))
                     : qsTr("Source Run: %1 - it no longer exists; this record remains.")
                           .arg(String(panel.record.sourceRunId || "")))
                  : ""
            textFormat: Text.PlainText
            color: panel.colors.textSecondary
            wrapMode: Text.WrapAnywhere
            font.pixelSize: panel.hostWindow.dp(10)
        }
        Text {
            Layout.fillWidth: true
            visible: panel.present
            text: panel.present ? panel.reasonText(panel.record.reasonCode) : ""
            textFormat: Text.PlainText
            color: panel.colors.textSecondary
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(10)
        }
        Text {
            objectName: "conversionRecordConfirmation"
            Layout.fillWidth: true
            visible: panel.present
            // Only an explicit confirmation carries a time; an automatic Retry never claims one.
            text: !panel.present ? ""
                  : (panel.record.mode === "explicit_user"
                     ? qsTr("You confirmed this conversion on %1.")
                           .arg(Qt.formatDateTime(panel.record.confirmedAt, "yyyy-MM-dd HH:mm"))
                     : qsTr("Created by an automatic Retry: nothing was dropped, so no confirmation was needed."))
            color: panel.colors.textPrimary
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(10)
            font.weight: Font.Medium
        }

        // ── What the conversion did not carry over ─────────────────────────────────────
        Text {
            Layout.fillWidth: true
            visible: panel.present
            text: panel.droppedSettings.length > 0
                  ? qsTr("Settings that were not carried over (%1)").arg(panel.droppedSettings.length)
                  : qsTr("Nothing was dropped: every setting of the source Run was carried over.")
            color: panel.colors.textPrimary
            font.pixelSize: panel.hostWindow.dp(10.5)
            font.weight: Font.Medium
        }
        Text {
            objectName: "conversionRecordInstructionsNote"
            Layout.fillWidth: true
            visible: panel.present && Boolean(panel.record.hasDroppedInstructions)
            text: qsTr("The lead's instructions were not used by this Run. Only their length is recorded, never the text.")
            color: panel.colors.warning
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(9.5)
        }
        Repeater {
            id: droppedRepeater
            objectName: "conversionRecordDropped"
            model: panel.droppedSettings
            delegate: ColumnLayout {
                id: droppedRow
                required property var modelData
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(1)

                Text {
                    Layout.fillWidth: true
                    text: qsTr("%1 · %2: %3").arg(String(droppedRow.modelData.memberId || ""))
                          .arg(panel.settingLabel(droppedRow.modelData.setting))
                          .arg(String(droppedRow.modelData.legacyValue || ""))
                    textFormat: Text.PlainText
                    color: panel.colors.textPrimary
                    wrapMode: Text.WrapAnywhere
                    font.pixelSize: panel.hostWindow.dp(10)
                }
                Text {
                    Layout.fillWidth: true
                    text: String(droppedRow.modelData.reason || "")
                    textFormat: Text.PlainText
                    color: panel.colors.textMuted
                    wrapMode: Text.WordWrap
                    font.pixelSize: panel.hostWindow.dp(9)
                }
            }
        }

        // ── The audit identity: policy, workflow and the fingerprints it is bound to ─────
        Text {
            objectName: "conversionRecordAudit"
            Layout.fillWidth: true
            visible: panel.present
            text: panel.present
                  ? qsTr("Policy %1 · from workflow %2 · source snapshot %3 · target roster %4 · confirmation %5")
                        .arg(String(panel.record.policyId || ""))
                        .arg(String(panel.record.sourceWorkflowId || ""))
                        .arg(panel.shortFingerprint(panel.record.sourceSnapshotFingerprint))
                        .arg(panel.shortFingerprint(panel.record.targetRosterFingerprint))
                        .arg(panel.shortFingerprint(panel.record.confirmationFingerprint))
                  : ""
            textFormat: Text.PlainText
            color: panel.colors.textMuted
            wrapMode: Text.WrapAnywhere
            font.pixelSize: panel.hostWindow.dp(8.5)
        }
    }
}
