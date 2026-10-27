pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: panel

    required property var hostWindow
    required property var colors
    required property var runData
    required property var plannerReportArtifact

    signal artifactRequested(string runId, string artifactId)

    objectName: "plannerOutcomeCard"
    Layout.fillWidth: true
    Layout.preferredHeight: plannerOutcomeColumn.implicitHeight
                            + panel.hostWindow.dp(20)
    visible: panel.plannerReportArtifact !== null
    radius: panel.colors.cornerRadiusSmall
    color: panel.colors.surface
    border.width: 1
    border.color: panel.plannerReportArtifact
                  && panel.plannerReportArtifact.name
                     === "manifest-planning-no-changes.md"
                  ? panel.colors.success : panel.colors.warning

    ColumnLayout {
        id: plannerOutcomeColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: panel.hostWindow.dp(10)
        anchors.rightMargin: panel.hostWindow.dp(10)
        spacing: panel.hostWindow.dp(6)

        Text {
            Layout.fillWidth: true
            text: panel.plannerReportArtifact
                  && panel.plannerReportArtifact.name
                     === "manifest-planning-no-changes.md"
                  ? qsTranslate("RunDetailsPanel", "Planner: current task already satisfied")
                  : qsTranslate("RunDetailsPanel", "Planner diagnostics")
            color: panel.colors.textPrimary
            font.pixelSize: panel.hostWindow.dp(10)
            font.weight: Font.DemiBold
        }
        Text {
            Layout.fillWidth: true
            text: panel.plannerReportArtifact
                  && panel.plannerReportArtifact.name
                     === "manifest-planning-no-changes.md"
                  ? qsTranslate("RunDetailsPanel", "Execution was intentionally skipped. Review the evidence tied to inspected paths and any recommended next step; no next stage was launched automatically.")
                  : qsTranslate("RunDetailsPanel", "Review the exact primary/final rejection codes and safe Backend validation findings.")
            color: panel.colors.textSecondary
            font.pixelSize: panel.hostWindow.dp(9)
            wrapMode: Text.WordWrap
        }
        RunActionButton {
            objectName: "viewPlannerReportButton"
            hostWindow: panel.hostWindow
            colors: panel.colors
            text: qsTranslate("RunDetailsPanel", "View planner report")
            iconText: "↗"
            primary: true
            onClicked: panel.artifactRequested(
                           panel.runData.runId,
                           panel.plannerReportArtifact.artifactId)
        }
    }
}
