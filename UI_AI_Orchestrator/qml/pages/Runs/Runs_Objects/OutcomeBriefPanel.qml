pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

// PC-4D: the read-only OUTCOME of a Run - whether what the Run produced has been resolved - exactly as the Backend's Outcome Brief
// says. Strictly a RENDERER of `backend.outcomeBrief.brief` (OutcomeBriefController): this file never classifies a state, counts a
// decision, reads a fingerprint out of the text or decides what may happen next, and it has no Apply, Retry or Compose action - the
// only controls open the stored text and close it again. Text that the model or a person wrote (a rejection reason, the Run's own
// summary) reaches the user only inside the stored brief text, as plain text in a read-only dialog, labelled there as untrusted data.
//
// The block exists only for a terminal outcome Run: an ordinary Run shows nothing. A brief that has not been written yet says so,
// and a brief that cannot be read is shown as a fault of its own (with the Backend's code and message), never as a stale brief
// and never as an empty section.
Rectangle {
    id: panel
    objectName: "outcomeBriefPanel"

    required property var hostWindow
    required property var colors
    // backend.outcomeBrief. Null-safe: without it the block simply stays hidden.
    property var controller: null

    readonly property var brief: panel.controller ? panel.controller.brief : ({ availability: "not_applicable" })
    readonly property string availability: String(panel.brief.availability || "not_applicable")
    readonly property bool present: panel.availability === "present"
    readonly property bool notRecorded: panel.availability === "not_recorded"
    readonly property bool unreadable: panel.availability === "unreadable"
    readonly property string briefState: panel.present ? String(panel.brief.state || "") : ""
    readonly property var counts: panel.present ? (panel.brief.counts || ({})) : ({})
    readonly property var reasons: panel.present ? (panel.brief.reasons || []) : []

    // -- Presentation only: the Backend's machine values in words ------------------------------------------------------------
    function stateTitle(state) {
        switch (state) {
        case "resolved_no_change": return qsTr("Resolved - nothing to review")
        case "awaiting_review": return qsTr("Awaiting review")
        case "awaiting_apply": return qsTr("Awaiting apply")
        case "resolved_applied": return qsTr("Resolved - applied")
        case "resolved_rejected": return qsTr("Resolved - rejected")
        case "resolved_mixed": return qsTr("Resolved - partly applied")
        case "attention_required": return qsTr("Needs attention")
        case "awaiting_manual_resolution": return qsTr("Awaiting manual resolution")
        case "resolved_manually_applied": return qsTr("Resolved - applied manually")
        case "superseded_by_continuation": return qsTr("Taken over by a continuation Run")
        case "recovery_only": return qsTr("Recovery context only")
        default: return String(state)
        }
    }

    function stateExplanation(state) {
        switch (state) {
        case "resolved_no_change": return qsTr("The Run finished and proposed no change that needs a decision.")
        case "awaiting_review": return qsTr("At least one proposal has no decision from a person yet.")
        case "awaiting_apply": return qsTr("At least one proposal is approved but has not been applied yet.")
        case "resolved_applied": return qsTr("Every proposal of this Run was applied.")
        case "resolved_rejected": return qsTr("Every proposal of this Run was rejected by a person.")
        case "resolved_mixed": return qsTr("Every proposal is final: some were applied and some were rejected by a person.")
        case "attention_required": return qsTr("The recorded facts of this Run cannot be settled automatically. See the reasons below.")
        case "awaiting_manual_resolution": return qsTr("A manual verification handoff exists for this failed Run and is not fully applied yet.")
        case "resolved_manually_applied": return qsTr("The manual handoff of this failed Run was fully applied.")
        case "superseded_by_continuation": return qsTr("A paid continuation Run took this failed Run over. That Run has its own outcome.")
        case "recovery_only": return qsTr("This Run did not complete, and nothing else owns its outcome.")
        default: return ""
        }
    }

    function reasonText(code) {
        switch (code) {
        case "source_ledger_missing": return qsTr("The Work Ledger entry of this Run is missing. It is never rebuilt from other data.")
        case "inconsistent_source_facts": return qsTr("The stored facts about this Run disagree with each other.")
        case "inconsistent_resolution": return qsTr("A review decision contradicts the state of a proposal (for example a rejection after the proposal was applied).")
        case "apply_failed": return qsTr("Applying a proposal failed. The workspace may be partly changed and should be inspected.")
        default: return String(code)
        }
    }

    // Only the counts that are not zero, in a fixed order; the Backend's numbers, untouched.
    function countsText(counts) {
        const labels = [
            ["members", qsTr("Proposals")],
            ["applied", qsTr("applied")],
            ["humanRejected", qsTr("rejected by a person")],
            ["approved", qsTr("approved, not applied")],
            ["undecided", qsTr("awaiting a decision")],
            ["applyFailed", qsTr("apply failed")],
            ["noChanges", qsTr("no changes needed")],
            ["validatorRejected", qsTr("blocked by the validator")],
            ["superseded", qsTr("replaced")],
            ["unsettled", qsTr("unfinished")],
            ["inconsistent", qsTr("inconsistent")]
        ]
        const parts = []
        for (let index = 0; index < labels.length; ++index) {
            const value = Number(counts[labels[index][0]] || 0)
            if (value > 0) parts.push(labels[index][0] === "members"
                                      ? labels[index][1] + ": " + value
                                      : value + " " + labels[index][1])
        }
        return parts.length > 0 ? parts.join(" | ") : qsTr("No proposals")
    }

    function shortFingerprint(value) {
        const text = String(value || "")
        return text.length > 12 ? text.substring(0, 12) + "..." : text
    }

    function stateColor(state) {
        if (state === "attention_required") return panel.colors.error
        if (state.indexOf("resolved_") === 0) return panel.colors.success
        if (state === "awaiting_review" || state === "awaiting_apply" || state === "awaiting_manual_resolution")
            return panel.colors.warning
        return panel.colors.textMuted
    }

    visible: panel.availability !== "not_applicable"
    Layout.fillWidth: true
    Layout.preferredHeight: visible ? outcomeColumn.implicitHeight + panel.hostWindow.dp(20) : 0
    radius: panel.colors.cornerRadiusSmall
    color: panel.unreadable ? panel.colors.warningSoft
           : panel.briefState === "attention_required" ? panel.colors.errorSoft : panel.colors.surface
    border.width: 1
    border.color: panel.unreadable ? panel.colors.warning
                  : panel.briefState === "attention_required" ? panel.colors.error : panel.colors.border

    ColumnLayout {
        id: outcomeColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: panel.hostWindow.dp(11)
        anchors.rightMargin: panel.hostWindow.dp(11)
        spacing: panel.hostWindow.dp(6)

        RowLayout {
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(8)
            Text {
                text: qsTr("Outcome")
                color: panel.colors.textPrimary
                font.pixelSize: panel.hostWindow.dp(13)
                font.weight: Font.DemiBold
            }
            Text {
                objectName: "outcomeBriefState"
                Layout.fillWidth: true
                visible: panel.present
                text: panel.present ? panel.stateTitle(panel.briefState) : ""
                color: panel.present ? panel.stateColor(panel.briefState) : panel.colors.textMuted
                elide: Text.ElideRight
                font.pixelSize: panel.hostWindow.dp(11.5)
                font.weight: Font.DemiBold
            }
            Item { Layout.fillWidth: true; visible: !panel.present }
        }

        // -- A brief that is not there yet ------------------------------------------------------------
        Text {
            objectName: "outcomeBriefNotRecorded"
            Layout.fillWidth: true
            visible: panel.notRecorded
            text: qsTr("No outcome brief has been recorded for this Run yet. It is written when the outcome is reconciled: after the Run finishes, after a proposal is reviewed or applied, and when the application starts.")
            color: panel.colors.textSecondary
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(10)
        }

        // -- A brief that cannot be read: a fault, shown on its own -------------------------------------------------------
        Text {
            objectName: "outcomeBriefUnreadable"
            Layout.fillWidth: true
            visible: panel.unreadable
            text: qsTr("The outcome brief of this Run could not be read, so none of it is shown.")
            color: panel.colors.textPrimary
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(11)
            font.weight: Font.DemiBold
        }
        Text {
            objectName: "outcomeBriefFault"
            Layout.fillWidth: true
            visible: panel.unreadable
            text: panel.unreadable
                  ? qsTr("%1: %2").arg(String(panel.brief.errorCode || "")).arg(String(panel.brief.errorMessage || ""))
                  : ""
            textFormat: Text.PlainText
            color: panel.colors.warning
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(9.5)
        }

        // -- The brief ------------------------------------------------------------
        Text {
            objectName: "outcomeBriefExplanation"
            Layout.fillWidth: true
            visible: panel.present
            text: panel.present ? panel.stateExplanation(panel.briefState) : ""
            color: panel.colors.textSecondary
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(10)
        }
        Text {
            objectName: "outcomeBriefCounts"
            Layout.fillWidth: true
            visible: panel.present
            text: panel.present ? panel.countsText(panel.counts) : ""
            color: panel.colors.textPrimary
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(10)
            font.weight: Font.Medium
        }
        Repeater {
            id: reasonsRepeater
            objectName: "outcomeBriefReasons"
            model: panel.reasons
            delegate: Text {
                id: reasonRow
                required property var modelData
                Layout.fillWidth: true
                text: "- " + panel.reasonText(String(reasonRow.modelData))
                color: panel.colors.error
                wrapMode: Text.WordWrap
                font.pixelSize: panel.hostWindow.dp(10)
            }
        }
        Text {
            objectName: "outcomeBriefRecovery"
            Layout.fillWidth: true
            visible: panel.present && (Boolean(panel.brief.recoveryEligible) || Boolean(panel.brief.requiresInspection))
            text: !panel.present ? ""
                  : (Boolean(panel.brief.requiresInspection)
                     ? qsTr("This Run was interrupted: inspect its workspace before relying on any result. Nothing recovers it automatically.")
                     : qsTr("Recovery is possible for this Run."))
            color: panel.brief && panel.brief.requiresInspection ? panel.colors.warning : panel.colors.textSecondary
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(9.5)
        }
        Text {
            objectName: "outcomeBriefRelations"
            Layout.fillWidth: true
            readonly property string childRunId: panel.present ? String(panel.brief.continuationChildRunId || "") : ""
            readonly property string sourceRunId: panel.present ? String(panel.brief.continuationSourceRunId || "") : ""
            readonly property string handoffId: panel.present ? String(panel.brief.manualHandoffId || "") : ""
            visible: childRunId.length > 0 || sourceRunId.length > 0 || handoffId.length > 0
            text: {
                const lines = []
                if (childRunId.length > 0) lines.push(qsTr("Taken over by continuation Run %1").arg(childRunId))
                if (sourceRunId.length > 0) lines.push(qsTr("Continuation of Run %1").arg(sourceRunId))
                if (handoffId.length > 0) lines.push(qsTr("Manual handoff %1").arg(handoffId))
                return lines.join("\n")
            }
            textFormat: Text.PlainText
            color: panel.colors.textSecondary
            wrapMode: Text.WrapAnywhere
            font.pixelSize: panel.hostWindow.dp(9.5)
        }

        RowLayout {
            Layout.fillWidth: true
            visible: panel.present
            spacing: panel.hostWindow.dp(8)
            Text {
                objectName: "outcomeBriefFooter"
                Layout.fillWidth: true
                text: panel.present
                      ? qsTr("Revision %1 | recorded %2 | source %3 | content %4")
                            .arg(Number(panel.brief.revision || 0))
                            .arg(Qt.formatDateTime(panel.brief.recordedAt, "yyyy-MM-dd HH:mm"))
                            .arg(panel.shortFingerprint(panel.brief.sourceStateFingerprint))
                            .arg(panel.shortFingerprint(panel.brief.contentFingerprint))
                      : ""
                textFormat: Text.PlainText
                color: panel.colors.textMuted
                wrapMode: Text.WrapAnywhere
                font.pixelSize: panel.hostWindow.dp(8.5)
            }
            RunActionButton {
                objectName: "outcomeBriefViewText"
                visible: panel.present && String(panel.brief.renderedText || "").length > 0
                hostWindow: panel.hostWindow
                colors: panel.colors
                text: qsTr("View brief text")
                onClicked: textDialog.open()
            }
        }

        Text {
            Layout.fillWidth: true
            visible: panel.present
            text: qsTr("Read-only. This summary changes only when the Run's proposals are reviewed, applied or recovered; nothing here starts, retries or applies anything.")
            color: panel.colors.textMuted
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(8.5)
        }
    }

    // A direct child of the root Rectangle (not of a Layout), like the dialogs of the neighbouring panels.
    OutcomeBriefTextDialog {
        id: textDialog
        hostWindow: panel.hostWindow
        colors: panel.colors
        contentText: panel.present ? String(panel.brief.renderedText || "") : ""
    }
}
