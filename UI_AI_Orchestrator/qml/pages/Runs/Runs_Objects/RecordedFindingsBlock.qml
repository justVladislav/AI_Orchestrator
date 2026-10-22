pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

// The recorded findings of a failed verification, shown ABOVE both repair panels (the paid `Prepare paid verification repair` in
// ManifestPanel and the free `Create repair workspace` in ManualHandoffPanel), before any repair workspace exists. Every value comes from
// ManifestVerificationManualHandoffController.recordedFindings (the Backend's own classification); this file classifies nothing, hides
// nothing and gates nothing: Create and Reverify stay available whatever the verdict says. The stored findings are a bounded sample, so the
// list is never presented as complete.
Rectangle {
    id: block
    objectName: "recordedFindingsBlock"

    required property var hostWindow
    required property var colors
    required property var controller

    readonly property var recorded: block.controller ? block.controller.recordedFindings : ({})
    readonly property var rows: block.recorded.findings || []
    readonly property string verdict: block.recorded.verdict || "unknown"
    readonly property int maximumInlineFindingsPerGroup: 3

    function isOutside(row) {
        return row.authority === "not_editable" || row.authority === "outside_all_create_modify_targets"
    }

    function isMayEdit(row) {
        return row.authority === "editable" || row.authority === "unknown_until_created"
    }

    // Plain lists of the Backend's own display lines; the split is by the Backend's severity and authority strings only.
    function linesWhere(severity, wanted) {
        const lines = []
        for (let i = 0; i < block.rows.length; ++i) {
            const row = block.rows[i]
            if (row.severity !== severity) continue
            const outside = block.isOutside(row)
            const mayEdit = block.isMayEdit(row)
            if (wanted === "outside" && !outside) continue
            if (wanted === "mayEdit" && !mayEdit) continue
            if (wanted === "unknown" && (outside || mayEdit)) continue
            lines.push(row.text)
        }
        return lines
    }

    function groups() {
        return [
            { "id": "blockingOutside", "tone": "error",
              "title": qsTr("Blocking, in files a repair workspace will certainly not be able to edit"),
              "items": block.linesWhere("blocking", "outside") },
            { "id": "blockingMayEdit", "tone": "error",
              "title": block.controller && block.controller.hasHandoff
                       ? qsTr("Blocking, in files you may edit") : qsTr("Blocking, in files a repair workspace may be able to edit"),
              "items": block.linesWhere("blocking", "mayEdit") },
            { "id": "blockingUnknown", "tone": "warning",
              "title": qsTr("Blocking, not tied to a file this handoff can be compared with"),
              "items": block.linesWhere("blocking", "unknown") },
            { "id": "advisory", "tone": "muted",
              "title": qsTr("Advisory (do not block)"),
              "items": block.linesWhere("advisory", "all") }
        ]
    }

    function preview(items) {
        return items.slice(0, block.maximumInlineFindingsPerGroup)
    }

    function showAllFindings() {
        findingsDialog.heading = qsTr("recorded failed verification")
        findingsDialog.recordedFindingsMode = true
        findingsDialog.scopeSummary = block.controller && block.controller.hasHandoff
                ? qsTr("The repair workspace's frozen editable files: %1")
                    .arg((block.controller.allowedRelativePaths || []).join(", "))
                : qsTr("The exact editable files are not known until a repair workspace is created.")
        findingsDialog.editablePaths = []
        findingsDialog.editableFindings = block.linesWhere("blocking", "mayEdit")
        findingsDialog.outsideScopeFindings = block.linesWhere("blocking", "outside")
        findingsDialog.newActionableFindings = []
        findingsDialog.newOutsideFindings = []
        findingsDialog.generalFindings = block.linesWhere("blocking", "unknown")
        findingsDialog.advisoryFindings = block.linesWhere("advisory", "all")
        findingsDialog.baselineDebtFindings = []
        findingsDialog.open()
    }

    function toneColor(tone) {
        return tone === "error" ? block.colors.error : tone === "warning" ? block.colors.warning : block.colors.textSecondary
    }

    visible: block.controller !== null && block.controller.isApplicable && block.recorded.hasFindings === true
    Layout.fillWidth: true
    Layout.preferredHeight: column.implicitHeight + block.hostWindow.dp(24)
    radius: block.colors.cornerRadiusMedium
    color: block.colors.surface
    border.width: 1
    border.color: block.verdict === "blocked_outside_scope" ? block.colors.error : block.colors.border

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: block.hostWindow.dp(12)
        anchors.rightMargin: block.hostWindow.dp(12)
        spacing: block.hostWindow.dp(6)

        Text {
            Layout.fillWidth: true
            text: qsTr("Recorded findings from the failed verification")
            color: block.colors.textPrimary
            font.pixelSize: block.hostWindow.dp(13)
            font.weight: Font.DemiBold
        }

        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Only a bounded sample of the findings is stored, so this list may be incomplete. It does not decide anything: "
                       + "Create and Reverify stay available and the full verification has the last word.")
            color: block.colors.textSecondary
            font.pixelSize: block.hostWindow.dp(10.5)
        }

        Text {
            objectName: "recordedFindingsAttemptsHint"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: block.controller && block.controller.hasHandoff && block.controller.attempts.length > 0
            text: qsTr("These are the findings of the failed verification the repair workspace started from. What each later "
                       + "verification attempt found is counted per attempt in the Manual verification handoff below.")
            color: block.colors.textSecondary
            font.pixelSize: block.hostWindow.dp(10.5)
        }

        // The verdict: plain Backend text. Information only; never a gate.
        Text {
            objectName: "recordedFindingsVerdict"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: (block.recorded.verdictMessage || "").length > 0
            text: block.recorded.verdictMessage || ""
            color: block.verdict === "blocked_outside_scope" ? block.colors.error
                   : block.verdict === "unknown" ? block.colors.warning : block.colors.textSecondary
            font.pixelSize: block.hostWindow.dp(11.5)
            font.weight: block.verdict === "blocked_outside_scope" ? Font.DemiBold : Font.Normal
        }

        // The ways out when a blocker is proven outside this handoff's reach. Nothing here widens the editable list.
        Text {
            objectName: "recordedFindingsExits"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: block.verdict === "blocked_outside_scope"
            text: block.controller && block.controller.hasHandoff
                  ? qsTr("What you can do: fix the file(s) above in the project yourself and start a new Run (this repair workspace then "
                         + "no longer matches the project and has to be discarded), or discard the workspace. A repair workspace never "
                         + "widens the list of files it may edit.")
                  : qsTr("What you can do: fix the file(s) above in the project yourself and start a new Run. Creating this repair "
                         + "workspace cannot clear them and never widens the list of files it may edit.")
            color: block.colors.textPrimary
            font.pixelSize: block.hostWindow.dp(11)
        }

        Text {
            objectName: "recordedFindingsClassificationError"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: (block.recorded.classificationErrorCode || "").length > 0
            text: (block.recorded.classificationErrorCode || "") + ": " + (block.recorded.classificationErrorMessage || "")
            color: block.colors.warning
            font.pixelSize: block.hostWindow.dp(10.5)
        }

        Repeater {
            model: block.groups()
            delegate: ColumnLayout {
                id: group
                required property var modelData
                objectName: "recordedFindingsGroup_" + group.modelData.id
                Layout.fillWidth: true
                visible: group.modelData.items.length > 0
                spacing: block.hostWindow.dp(3)

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("%1 (%2)").arg(group.modelData.title).arg(group.modelData.items.length)
                    color: block.toneColor(group.modelData.tone)
                    font.pixelSize: block.hostWindow.dp(11)
                    font.weight: Font.DemiBold
                }
                Repeater {
                    model: block.preview(group.modelData.items)
                    delegate: Text {
                        required property string modelData
                        Layout.fillWidth: true
                        wrapMode: Text.WrapAnywhere
                        textFormat: Text.PlainText
                        text: modelData
                        color: block.colors.textPrimary
                        font.pixelSize: block.hostWindow.dp(10.5)
                    }
                }
                Text {
                    Layout.fillWidth: true
                    visible: group.modelData.items.length > block.maximumInlineFindingsPerGroup
                    text: qsTr("%1 more in the findings window")
                        .arg(group.modelData.items.length - block.maximumInlineFindingsPerGroup)
                    color: block.colors.textMuted
                    font.pixelSize: block.hostWindow.dp(10)
                }
            }
        }

        RunActionButton {
            objectName: "recordedFindingsShowAllButton"
            hostWindow: block.hostWindow
            colors: block.colors
            text: qsTr("Show recorded findings in a window")
            onClicked: block.showAllFindings()
        }

        Text {
            objectName: "recordedFindingsDebtCount"
            Layout.fillWidth: true
            visible: (block.recorded.debtCount || 0) > 0
            text: qsTr("%1 older warning(s) that were already there and do not block are not listed.").arg(block.recorded.debtCount)
            color: block.colors.textMuted
            font.pixelSize: block.hostWindow.dp(10)
        }
    }

    ManualHandoffFindingsDialog {
        id: findingsDialog
        objectName: "recordedFindingsDialog"
        hostWindow: block.hostWindow
        colors: block.colors
    }
}
