import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Rectangle {
    id: panel

    required property var hostWindow
    required property var colors
    // Qt Adapter composition root - only ProjectMemoryPanel reads from it
    // directly (ProjectMemoryController's data is fetched live, not
    // pre-aggregated into a page-level snapshot like tasksModel below).
    required property var backend
    property var projectData: null
    property var tasksModel: []

    signal continueRequested(string projectId)
    signal orchestrationRequested(string projectId)
    signal removeRequested()
    signal restoreRequested()
    signal addTaskRequested()
    signal editTaskRequested(var task)
    signal cancelTaskRequested(string taskId)
    signal editContextRequested()
    signal clearMemoryRequested(string projectId)
    // path is a plain local filesystem path, already stripped of file://.
    signal pathChangeRequested(string projectId, string path)

    // Demo rows and backend rows do not expose every optional field yet.
    function projectString(fieldName, fallbackValue) {
        if (!projectData)
            return fallbackValue

        const value = projectData[fieldName]
        return typeof value === "string" && value.length > 0
               ? value
               : fallbackValue
    }

    radius: colors.cornerRadiusMedium
    color: colors.surfaceAlternative
    border.width: 1
    border.color: colors.border

    Flickable {
        anchors.fill: parent
        anchors.margins: panel.hostWindow.dp(18)
        contentWidth: width
        contentHeight: detailsLayout.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ScrollBar.vertical: ScrollBar { id: detailsScrollBar }

        ColumnLayout {
            id: detailsLayout
            width: Math.max(0, parent.width - detailsScrollBar.width
                            - panel.hostWindow.dp(4))
            spacing: panel.hostWindow.dp(14)

            RowLayout {
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(10)

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: panel.hostWindow.dp(3)

                    Text {
                        Layout.fillWidth: true
                        text: panel.projectData ? panel.projectData.name : qsTr("Select a project")
                        color: panel.colors.textPrimary
                        elide: Text.ElideRight
                        font.pixelSize: panel.hostWindow.dp(20)
                        font.weight: Font.DemiBold
                    }

                    Text {
                        Layout.fillWidth: true
                        text: panel.projectData ? panel.projectData.description : qsTr("Project details will appear here.")
                        color: panel.colors.textSecondary
                        wrapMode: Text.WordWrap
                        maximumLineCount: 3
                        elide: Text.ElideRight
                        font.pixelSize: panel.hostWindow.dp(12)
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: panel.projectData !== null
                        text: qsTr("View / Edit context")
                        color: panel.colors.accent
                        font.pixelSize: panel.hostWindow.dp(10)
                        font.underline: true

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: panel.editContextRequested()
                        }
                    }
                }

                Rectangle {
                    visible: panel.projectData !== null
                    Layout.preferredWidth: statusText.implicitWidth + panel.hostWindow.dp(18)
                    Layout.preferredHeight: panel.hostWindow.dp(26)
                    radius: height / 2
                    color: panel.projectData && panel.projectData.status === "active"
                           ? Qt.rgba(0.27, 0.83, 0.60, 0.16)
                           : Qt.rgba(0.96, 0.73, 0.30, 0.16)

                    Text {
                        id: statusText
                        anchors.centerIn: parent
                        text: !panel.projectData
                              ? ""
                              : panel.projectData.status === "active"
                                ? qsTr("Active")
                                : panel.projectData.status === "archived"
                                  ? qsTr("Archived")
                                  : qsTr("Unavailable")
                        color: panel.projectData && panel.projectData.status === "active"
                               ? panel.colors.success
                               : panel.colors.warning
                        font.pixelSize: panel.hostWindow.dp(10)
                        font.weight: Font.DemiBold
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: projectInfo.implicitHeight + panel.hostWindow.dp(22)
                radius: panel.colors.cornerRadiusSmall
                color: panel.colors.inputBackground
                border.width: 1
                border.color: panel.colors.border

                GridLayout {
                    id: projectInfo
                    anchors.fill: parent
                    anchors.margins: panel.hostWindow.dp(11)
                    columns: 2
                    columnSpacing: panel.hostWindow.dp(18)
                    rowSpacing: panel.hostWindow.dp(8)

                    Text {
                        text: qsTr("Path")
                        color: panel.colors.textMuted
                        font.pixelSize: panel.hostWindow.dp(10)
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: panel.hostWindow.dp(6)

                        Text {
                            Layout.fillWidth: true
                            text: panel.projectData ? panel.projectData.path : "—"
                            color: panel.colors.textSecondary
                            elide: Text.ElideMiddle
                            font.pixelSize: panel.hostWindow.dp(10)
                        }

                        Text {
                            visible: panel.projectData !== null
                            text: qsTr("Edit")
                            color: pathEditArea.containsMouse
                                   ? panel.colors.textPrimary : panel.colors.accent
                            font.pixelSize: panel.hostWindow.dp(9.5)
                            font.underline: true

                            MouseArea {
                                id: pathEditArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: pathFolderDialog.open()
                            }
                        }
                    }

                    FolderDialog {
                        id: pathFolderDialog
                        title: qsTr("Select new project path")

                        onAccepted: {
                            // selectedFolder is a file:// URL; the backend
                            // expects a plain local path.
                            let path = pathFolderDialog.selectedFolder.toString()
                            if (path.startsWith("file://"))
                                path = path.substring(7)
                            // A Windows path yields "file:///C:/Users/..." ->
                            // after stripping "file://" that leaves
                            // "/C:/Users/...": strip that extra leading slash
                            // only when a drive letter follows. A real POSIX
                            // path (e.g. "/Users/...") must keep its own
                            // leading slash.
                            if (/^\/[A-Za-z]:\//.test(path))
                                path = path.substring(1)
                            path = decodeURIComponent(path)
                            if (panel.projectData)
                                panel.pathChangeRequested(panel.projectData.projectId, path)
                        }
                    }

                    Text {
                        text: qsTr("Workflow")
                        color: panel.colors.textMuted
                        font.pixelSize: panel.hostWindow.dp(10)
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Mock sequential pipeline")
                        color: panel.colors.textPrimary
                        font.pixelSize: panel.hostWindow.dp(11)
                    }

                    Text {
                        text: qsTr("Created")
                        color: panel.colors.textMuted
                        font.pixelSize: panel.hostWindow.dp(10)
                    }
                    Text {
                        Layout.fillWidth: true
                        text: panel.projectString("createdLabel", "—")
                        color: panel.colors.textPrimary
                        font.pixelSize: panel.hostWindow.dp(11)
                    }

                    Text {
                        text: qsTr("Updated")
                        color: panel.colors.textMuted
                        font.pixelSize: panel.hostWindow.dp(10)
                    }
                    Text {
                        Layout.fillWidth: true
                        text: panel.projectString("updatedLabel", "—")
                        color: panel.colors.textPrimary
                        font.pixelSize: panel.hostWindow.dp(11)
                    }
                }
            }

            ProjectTasksPanel {
                hostWindow: panel.hostWindow
                colors: panel.colors
                tasksModel: panel.tasksModel
                visible: panel.projectData !== null
                onAddRequested: panel.addTaskRequested()
                onEditRequested: task => panel.editTaskRequested(task)
                onCancelRequested: taskId => panel.cancelTaskRequested(taskId)
            }

            ProjectSetupPanel {
                hostWindow: panel.hostWindow
                colors: panel.colors
                backend: panel.backend
                projectId: panel.projectData ? panel.projectData.projectId : ""
                projectPath: panel.projectData ? panel.projectData.path : ""
                visible: panel.projectData !== null
            }

            ProjectMemoryPanel {
                hostWindow: panel.hostWindow
                colors: panel.colors
                backend: panel.backend
                projectId: panel.projectData ? panel.projectData.projectId : ""
                visible: panel.projectData !== null
                onClearMemoryRequested: {
                    if (panel.projectData)
                        panel.clearMemoryRequested(panel.projectData.projectId)
                }
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("Last run")
                color: panel.colors.textPrimary
                font.pixelSize: panel.hostWindow.dp(13)
                font.weight: Font.DemiBold
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: recentContent.implicitHeight + panel.hostWindow.dp(22)
                radius: panel.colors.cornerRadiusSmall
                color: panel.colors.inputBackground
                border.width: 1
                border.color: panel.colors.border

                ColumnLayout {
                    id: recentContent
                    anchors.fill: parent
                    anchors.margins: panel.hostWindow.dp(11)
                    spacing: panel.hostWindow.dp(9)

                    Text {
                        Layout.fillWidth: true
                        text: panel.projectString("lastRunTitle",
                                                  qsTr("No runs started yet"))
                        color: panel.colors.textPrimary
                        wrapMode: Text.WordWrap
                        font.pixelSize: panel.hostWindow.dp(11)
                    }

                    Text {
                        Layout.fillWidth: true
                        text: panel.projectString("lastRunStatusLabel", "").length > 0
                              ? qsTr("Status: %1").arg(panel.projectString("lastRunStatusLabel", ""))
                              : ""
                        color: panel.colors.textMuted
                        font.pixelSize: panel.hostWindow.dp(10)
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("Runs")
                color: panel.colors.textPrimary
                font.pixelSize: panel.hostWindow.dp(13)
                font.weight: Font.DemiBold
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(8)

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: panel.hostWindow.dp(70)
                    radius: panel.colors.cornerRadiusSmall
                    color: panel.colors.inputBackground
                    border.width: 1
                    border.color: panel.colors.border

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: panel.hostWindow.dp(10)
                        spacing: panel.hostWindow.dp(3)

                        Text {
                            text: panel.projectData
                                  ? qsTr("%1 running").arg(panel.projectData.runsRunning)
                                  : qsTr("Running")
                            color: panel.colors.information
                            font.pixelSize: panel.hostWindow.dp(10)
                            font.weight: Font.DemiBold
                        }

                        Text {
                            Layout.fillWidth: true
                            text: panel.projectData ? qsTr("%1 total runs").arg(panel.projectData.runsTotal) : "—"
                            color: panel.colors.textPrimary
                            elide: Text.ElideRight
                            font.pixelSize: panel.hostWindow.dp(11)
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: panel.hostWindow.dp(70)
                    radius: panel.colors.cornerRadiusSmall
                    color: panel.colors.inputBackground
                    border.width: 1
                    border.color: panel.colors.border

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: panel.hostWindow.dp(10)
                        spacing: panel.hostWindow.dp(3)

                        Text {
                            text: panel.projectData
                                  ? qsTr("%1 completed").arg(panel.projectData.runsCompleted)
                                  : qsTr("Completed")
                            color: panel.colors.success
                            font.pixelSize: panel.hostWindow.dp(10)
                            font.weight: Font.DemiBold
                        }

                        Text {
                            Layout.fillWidth: true
                            text: panel.projectData ? qsTr("%1 failed").arg(panel.projectData.runsFailed) : "—"
                            color: panel.colors.textPrimary
                            elide: Text.ElideRight
                            font.pixelSize: panel.hostWindow.dp(11)
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: panel.hostWindow.dp(2)
                spacing: panel.hostWindow.dp(8)

                ProjectActionButton {
                    text: qsTr("Continue")
                    primary: true
                    enabled: panel.projectData !== null
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    onClicked: panel.continueRequested(panel.projectData.projectId)
                }

                ProjectActionButton {
                    text: qsTr("New run")
                    enabled: panel.projectData !== null
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Configure agents and start orchestration for this project.")
                    onClicked: panel.orchestrationRequested(panel.projectData.projectId)
                }

                Item { Layout.fillWidth: true }

                ProjectActionButton {
                    text: qsTr("Restore")
                    visible: panel.projectData !== null && panel.projectData.status === "archived"
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    onClicked: panel.restoreRequested()
                }

                ProjectActionButton {
                    text: qsTr("Archive")
                    danger: true
                    visible: panel.projectData === null || panel.projectData.status === "active"
                    enabled: panel.projectData !== null && panel.projectData.status === "active"
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    onClicked: panel.removeRequested()
                }
            }
        }
    }
}
