pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: panel

    required property var hostWindow
    required property var colors
    required property var runData
    required property var manifestVerificationArtifact

    signal artifactRequested(string runId, string artifactId)

    objectName: "manifestVerificationCard"
    Layout.fillWidth: true
    Layout.preferredHeight: verificationColumn.implicitHeight
                            + panel.hostWindow.dp(20)
    visible: panel.runData !== null
             && panel.runData.workflowId === "execution-manifest-v1"
    radius: panel.colors.cornerRadiusSmall
    color: panel.colors.surface
    border.width: 1
    // The card is intentionally never green from artifact
    // presence alone. Exact statuses live in the integrity-
    // checked record and unknown/not-run must stay neutral.
    border.color: panel.colors.information

    ColumnLayout {
        id: verificationColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: panel.hostWindow.dp(10)
        anchors.rightMargin: panel.hostWindow.dp(10)
        spacing: panel.hostWindow.dp(6)

        Text {
            Layout.fillWidth: true
            text: qsTranslate("RunDetailsPanel", "Manifest verification")
            color: panel.colors.textPrimary
            font.pixelSize: panel.hostWindow.dp(10)
            font.weight: Font.DemiBold
        }
        Text {
            objectName: "manifestVerificationSummary"
            Layout.fillWidth: true
            text: panel.manifestVerificationArtifact !== null
                  ? qsTranslate("RunDetailsPanel", "Recorded separately: proposal safety/schema validation, combined build/lint/test verification, and acceptance-criteria verification. Open the record for exact statuses and evidence.")
                  : qsTranslate("RunDetailsPanel", "Verification not available or not run. This state is not treated as passed; older Runs are not assigned a result retroactively.")
            color: panel.colors.textSecondary
            font.pixelSize: panel.hostWindow.dp(9)
            wrapMode: Text.WordWrap
        }
        RunActionButton {
            objectName: "viewManifestVerificationButton"
            visible: panel.manifestVerificationArtifact !== null
            hostWindow: panel.hostWindow
            colors: panel.colors
            text: qsTranslate("RunDetailsPanel", "View verification record")
            iconText: "↗"
            primary: true
            onClicked: panel.artifactRequested(
                           panel.runData.runId,
                           panel.manifestVerificationArtifact.artifactId)
        }
    }
}
