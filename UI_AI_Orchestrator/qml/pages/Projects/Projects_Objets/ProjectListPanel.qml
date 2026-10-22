pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: panel

    required property var hostWindow
    required property var colors
    required property var projectsModel

    property string selectedProjectId: ""
    property string searchText: ""
    property string statusFilter: "all"

    signal selectionRequested(string projectId)

    // Resolves the ListView's visual currentIndex from the stable ID so
    // keyboard navigation still works without selection depending on position.
    function indexForId(projectId) {
        if (!projectsModel || typeof projectsModel.get !== "function")
            return -1

        for (let i = 0; i < projectsModel.count; ++i) {
            if (projectsModel.get(i).projectId === projectId)
                return i
        }
        return -1
    }

    radius: colors.cornerRadiusMedium
    color: colors.surfaceAlternative
    border.width: 1
    border.color: colors.border
    clip: true

    function matchesProject(projectName, projectPath, projectStatus) {
        const query = searchText.trim().toLowerCase()
        const matchesText = query.length === 0
                            || projectName.toLowerCase().includes(query)
                            || projectPath.toLowerCase().includes(query)
        const matchesStatus = statusFilter === "all"
                              || projectStatus === statusFilter

        return matchesText && matchesStatus
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: panel.hostWindow.dp(36)
            color: "transparent"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: panel.hostWindow.dp(14)
                anchors.rightMargin: panel.hostWindow.dp(14) + projectListScrollBar.width
                spacing: panel.hostWindow.dp(8)

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Project")
                    color: panel.colors.textMuted
                    font.pixelSize: panel.hostWindow.dp(11)
                    font.weight: Font.DemiBold
                }

                Text {
                    Layout.preferredWidth: panel.hostWindow.dp(76)
                    text: qsTr("Runs")
                    color: panel.colors.textMuted
                    horizontalAlignment: Text.AlignHCenter
                    font.pixelSize: panel.hostWindow.dp(11)
                    font.weight: Font.DemiBold
                }

                Text {
                    Layout.preferredWidth: panel.hostWindow.dp(88)
                    text: qsTr("Updated")
                    color: panel.colors.textMuted
                    horizontalAlignment: Text.AlignRight
                    font.pixelSize: panel.hostWindow.dp(11)
                    font.weight: Font.DemiBold
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: panel.colors.separator
        }

        ListView {
            id: projectList
            objectName: "projectListView"
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: panel.projectsModel
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            currentIndex: panel.indexForId(panel.selectedProjectId)

            ScrollBar.vertical: ScrollBar { id: projectListScrollBar }

            delegate: Item {
                id: row

                required property int index
                required property string projectId
                required property string name
                required property string path
                required property string status
                required property int activeTasks
                required property string lastActivity

                readonly property bool matches: panel.matchesProject(name, path, status)
                // Identity-based selection: survives list reordering/insertion,
                // unlike comparing against a positional index.
                readonly property bool selected: projectId === panel.selectedProjectId

                width: projectList.width
                height: matches ? panel.hostWindow.dp(62) : 0
                visible: matches

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: panel.hostWindow.dp(4)
                    radius: panel.colors.cornerRadiusSmall
                    color: row.selected
                           ? panel.colors.accentSoft
                           : rowMouse.containsMouse
                             ? panel.colors.buttonHover
                             : "transparent"
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: panel.hostWindow.dp(14)
                    anchors.rightMargin: panel.hostWindow.dp(14) + projectListScrollBar.width
                    spacing: panel.hostWindow.dp(8)

                    Rectangle {
                        Layout.preferredWidth: panel.hostWindow.dp(8)
                        Layout.preferredHeight: panel.hostWindow.dp(8)
                        radius: width / 2
                        color: row.status === "active"
                               ? panel.colors.success
                               : row.status === "unavailable"
                                 ? panel.colors.error
                                 : panel.colors.textMuted
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: panel.hostWindow.dp(2)

                        Text {
                            Layout.fillWidth: true
                            text: row.name
                            color: panel.colors.textPrimary
                            elide: Text.ElideRight
                            font.pixelSize: panel.hostWindow.dp(13)
                            font.weight: row.selected ? Font.DemiBold : Font.Medium
                        }

                        Text {
                            Layout.fillWidth: true
                            text: row.path
                            color: panel.colors.textMuted
                            elide: Text.ElideMiddle
                            font.pixelSize: panel.hostWindow.dp(10)
                        }
                    }

                    Text {
                        Layout.preferredWidth: panel.hostWindow.dp(76)
                        text: String(row.activeTasks)
                        color: row.activeTasks > 0
                               ? panel.colors.information
                               : panel.colors.textMuted
                        horizontalAlignment: Text.AlignHCenter
                        font.pixelSize: panel.hostWindow.dp(12)
                        font.weight: Font.DemiBold
                    }

                    Text {
                        Layout.preferredWidth: panel.hostWindow.dp(88)
                        text: row.lastActivity
                        color: panel.colors.textSecondary
                        horizontalAlignment: Text.AlignRight
                        elide: Text.ElideRight
                        font.pixelSize: panel.hostWindow.dp(10)
                    }
                }

                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: panel.selectionRequested(row.projectId)
                    onDoubleClicked: panel.selectionRequested(row.projectId)
                }
            }

            Text {
                anchors.centerIn: parent
                visible: projectList.count === 0
                text: qsTr("No projects yet")
                color: panel.colors.textMuted
                font.pixelSize: panel.hostWindow.dp(13)
            }
        }

        Rectangle {
            objectName: "projectStatusLegend"
            Layout.fillWidth: true
            Layout.preferredHeight: panel.hostWindow.dp(36)
            Layout.leftMargin: panel.hostWindow.dp(8)
            Layout.rightMargin: panel.hostWindow.dp(8)
            Layout.bottomMargin: panel.hostWindow.dp(8)
            radius: panel.colors.cornerRadiusSmall
            color: panel.colors.surface
            border.width: 1
            border.color: panel.colors.border

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: panel.hostWindow.dp(10)
                anchors.rightMargin: panel.hostWindow.dp(10)
                spacing: panel.hostWindow.dp(14)

                Text {
                    text: qsTr("Status")
                    color: panel.colors.textMuted
                    font.pixelSize: panel.hostWindow.dp(9.5)
                    font.weight: Font.DemiBold
                }

                Repeater {
                    model: [
                        { label: qsTr("Active"), color: panel.colors.success },
                        { label: qsTr("Archived"), color: panel.colors.textMuted },
                        { label: qsTr("Unavailable"), color: panel.colors.error }
                    ]

                    RowLayout {
                        required property var modelData
                        spacing: panel.hostWindow.dp(5)

                        Rectangle {
                            Layout.preferredWidth: panel.hostWindow.dp(7)
                            Layout.preferredHeight: panel.hostWindow.dp(7)
                            radius: width / 2
                            color: modelData.color
                        }

                        Text {
                            text: modelData.label
                            color: panel.colors.textSecondary
                            font.pixelSize: panel.hostWindow.dp(9.5)
                        }
                    }
                }

                Item { Layout.fillWidth: true }
            }
        }
    }
}
