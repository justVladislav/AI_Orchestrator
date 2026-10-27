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
                               panel.workspaceBaselineArtifact.artifactId)
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
                               panel.workspaceProvenanceArtifact.artifactId)
            }
            Item { Layout.fillWidth: true }
        }
    }
}
