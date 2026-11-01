pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: panel

    required property var hostWindow
    required property var colors
    required property var runData
    required property var workspaceBaselineArtifact
    required property var workspaceProvenanceArtifact
    property var diagnostics: ({})
    readonly property var recorded: panel.runData && panel.diagnostics
        && panel.diagnostics.runId === panel.runData.runId ? panel.diagnostics : ({})
    readonly property string recordedRunId: panel.recorded.runId || ""
    readonly property var baselineFacts: panel.recorded.baseline || ({})
    readonly property var comparison: panel.recorded.comparison || ({})
    property bool rulesExpanded: false
    onRecordedRunIdChanged: panel.rulesExpanded = false

    function comparisonText() {
        if (panel.comparison.state === "unavailable")
            return qsTranslate("RunDetailsPanel", "The comparison could not be read. Open the report for details; no result is inferred.")
        const labels = {
            "unchanged": qsTranslate("RunDetailsPanel", "Recorded comparison: unchanged."),
            "manual_or_external_drift": qsTranslate("RunDetailsPanel", "Recorded comparison: manual or external workspace changes."),
            "applied_proposal_set": qsTranslate("RunDetailsPanel", "Recorded comparison matches the applied proposal set."),
            "applied_with_manual_or_external_drift": qsTranslate("RunDetailsPanel", "Recorded comparison: applied proposals plus manual or external workspace changes."),
            "not_verifiable": qsTranslate("RunDetailsPanel", "The workspace comparison could not be completed.")
        }
        return labels[panel.comparison.classification] || ""
    }

    signal artifactRequested(string runId, string artifactId)

    objectName: "workspaceProvenanceCard"
    Layout.fillWidth: true
    Layout.preferredHeight: workspaceProvenanceColumn.implicitHeight
                            + panel.hostWindow.dp(20)
    visible: panel.runData !== null
             && panel.runData.workflowId === "execution-manifest-v1"
    radius: panel.colors.cornerRadiusSmall
    color: panel.colors.surface
    border.width: 1
    border.color: panel.colors.information

    ColumnLayout {
        id: workspaceProvenanceColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: panel.hostWindow.dp(10)
        anchors.rightMargin: panel.hostWindow.dp(10)
        spacing: panel.hostWindow.dp(6)

        Text {
            Layout.fillWidth: true
            text: qsTranslate("RunDetailsPanel", "Workspace provenance")
            color: panel.colors.textPrimary
            font.pixelSize: panel.hostWindow.dp(10)
            font.weight: Font.DemiBold
        }
        Text {
            objectName: "workspaceRecordedOutputPaths"
            Layout.fillWidth: true
            text: panel.baselineFacts.state === "recorded"
                  ? ((panel.baselineFacts.outputPaths || []).length > 0
                     ? qsTranslate("RunDetailsPanel", "Output folders recorded for this Run:\n%1")
                         .arg(panel.baselineFacts.outputPaths.join("\n"))
                     : qsTranslate("RunDetailsPanel", "No output-folder exclusions were recorded in this baseline."))
                  : (panel.baselineFacts.state === "unavailable"
                     ? qsTranslate("RunDetailsPanel", "Recorded output folders are unavailable (%1). No empty scope is inferred.")
                         .arg(panel.baselineFacts.errorCode || "unknown")
                     : qsTranslate("RunDetailsPanel", "Recorded output folders have not been loaded or are not available for this Run."))
            textFormat: Text.PlainText
            color: panel.colors.textSecondary
            font.pixelSize: panel.hostWindow.dp(9)
            wrapMode: Text.Wrap
        }
        Text {
            objectName: "workspaceFrozenScopeNotice"
            Layout.fillWidth: true
            text: qsTranslate("RunDetailsPanel", "Output-folder exclusions are frozen for each Run. Approving or revoking a folder in Projects > Output folders affects new Manifest Runs; it does not change this Run or its continuation/handoff area. Recorded baseline rules are historical; static safety defaults may evolve.")
            color: panel.colors.textSecondary
            font.pixelSize: panel.hostWindow.dp(9)
            wrapMode: Text.WordWrap
        }
        ToolButton {
            objectName: "workspaceRecordedRulesToggle"
            visible: panel.baselineFacts.state === "recorded"
            text: panel.rulesExpanded
                  ? qsTranslate("RunDetailsPanel", "Hide recorded baseline rules")
                  : qsTranslate("RunDetailsPanel", "Show recorded baseline rules")
            onClicked: panel.rulesExpanded = !panel.rulesExpanded
        }
        Text {
            objectName: "workspaceRecordedRules"
            Layout.fillWidth: true
            visible: panel.rulesExpanded && panel.baselineFacts.state === "recorded"
            text: (panel.baselineFacts.exclusions || []).join("\n")
            textFormat: Text.PlainText
            color: panel.colors.textSecondary
            font.pixelSize: panel.hostWindow.dp(9)
            wrapMode: Text.Wrap
        }
        Text {
            objectName: "workspaceRecordedComparison"
            Layout.fillWidth: true
            visible: text.length > 0
            text: panel.comparisonText()
            color: panel.colors.textPrimary
            font.pixelSize: panel.hostWindow.dp(9)
            wrapMode: Text.WordWrap
        }
        Text {
            objectName: "workspaceComparisonDetails"
            Layout.fillWidth: true
            text: [panel.comparison.errorCode || "", panel.comparison.details || ""]
                .filter(value => value.length > 0).join("\n")
            visible: text.length > 0
            textFormat: Text.PlainText
            color: panel.colors.textSecondary
            font.pixelSize: panel.hostWindow.dp(9)
            wrapMode: Text.Wrap
        }
        Text {
            objectName: "workspaceProvenanceNextAction"
            Layout.fillWidth: true
            visible: panel.comparison.classification === "not_verifiable"
                     || panel.comparison.classification === "manual_or_external_drift"
                     || panel.comparison.classification === "applied_with_manual_or_external_drift"
            text: panel.comparison.errorCode === "workspace_baseline.file_limit"
                  ? qsTranslate("RunDetailsPanel", "Review the reported path, size and limit in View provenance. If the folder is genuinely disposable build output, review it in Projects > Output folders before starting a new Run. Do not hide source folders to bypass this failure.")
                  : qsTranslate("RunDetailsPanel", "Open View provenance to review the changed paths or failure before retrying. Check edits made by you, your IDE or other tools. A report does not identify which tool made a change, and changing today's approvals will not repair this Run's recorded area.")
            color: panel.colors.textSecondary
            font.pixelSize: panel.hostWindow.dp(9)
            wrapMode: Text.WordWrap
        }
        Text {
            objectName: "workspaceProvenanceSummary"
            Layout.fillWidth: true
            text: panel.workspaceBaselineArtifact === null
                  ? qsTranslate("RunDetailsPanel", "Workspace baseline is not available for this Run. No provenance result is inferred.")
                  : (panel.workspaceProvenanceArtifact === null
                     ? qsTranslate("RunDetailsPanel", "The pre-provider baseline is recorded. Post-run comparison is not available or not run.")
                     : qsTranslate("RunDetailsPanel", "The pre-provider baseline and latest post-run or post-apply comparison are recorded. Open them for the exact fingerprint, exclusions, proposal set, and manual/external drift classification."))
            color: panel.colors.textSecondary
            font.pixelSize: panel.hostWindow.dp(9)
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(6)
            RunActionButton {
                objectName: "viewWorkspaceBaselineButton"
                visible: panel.workspaceBaselineArtifact !== null
                hostWindow: panel.hostWindow
                colors: panel.colors
                text: qsTranslate("RunDetailsPanel", "View baseline")
                iconText: "↗"
                onClicked: panel.artifactRequested(
                               panel.runData.runId,
                               panel.baselineFacts.artifactId || panel.workspaceBaselineArtifact.artifactId)
            }
            RunActionButton {
                objectName: "viewWorkspaceProvenanceButton"
                visible: panel.workspaceProvenanceArtifact !== null
                hostWindow: panel.hostWindow
                colors: panel.colors
                text: qsTranslate("RunDetailsPanel", "View provenance")
                iconText: "↗"
                primary: true
                onClicked: panel.artifactRequested(
                               panel.runData.runId,
                               panel.comparison.artifactId || panel.workspaceProvenanceArtifact.artifactId)
            }
            Item { Layout.fillWidth: true }
        }
    }
}
