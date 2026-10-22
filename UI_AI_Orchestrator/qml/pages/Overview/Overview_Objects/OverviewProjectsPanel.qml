pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

Rectangle {
    id: panel

    required property var hostWindow
    required property var colors
    required property var projectsModel
    property int selectedIndex: -1

    signal projectSelected(int index)

    implicitHeight: hostWindow.dp(312)
    radius: colors.cornerRadiusMedium
    color: colors.surfaceAlternative
    border.width: 1
    border.color: colors.border

    function statusColor(status) {
        if (status === "active") return colors.success
        if (status === "paused") return colors.warning
        return colors.textMuted
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: panel.hostWindow.dp(16)
        spacing: panel.hostWindow.dp(8)

        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(2)
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Projects")
                    color: panel.colors.textPrimary
                    font.pixelSize: panel.hostWindow.dp(15)
                    font.weight: Font.DemiBold
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Select a project to update the dashboard")
                    color: panel.colors.textMuted
                    font.pixelSize: panel.hostWindow.dp(10)
                    elide: Text.ElideRight
                }
            }
            Text {
                text: qsTr("%1 total").arg(panel.projectsModel.length)
                color: panel.colors.textSecondary
                font.pixelSize: panel.hostWindow.dp(10)
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: panel.colors.separator }

        Repeater {
            model: panel.projectsModel
            delegate: Basic.AbstractButton {
                id: projectItem
                required property int index
                required property var modelData
                Layout.fillWidth: true
                Layout.preferredHeight: panel.hostWindow.dp(48)
                hoverEnabled: true
                Accessible.name: qsTr("Show %1 overview").arg(modelData.name)
                onClicked: panel.projectSelected(index)

                background: Rectangle {
                    radius: panel.colors.cornerRadiusSmall
                    color: projectItem.index === panel.selectedIndex
                           ? panel.colors.accentSoft
                           : projectItem.hovered ? panel.colors.buttonHover : "transparent"
                }

                contentItem: RowLayout {
                    spacing: panel.hostWindow.dp(10)
                    Rectangle {
                        Layout.preferredWidth: panel.hostWindow.dp(8)
                        Layout.preferredHeight: panel.hostWindow.dp(8)
                        radius: width / 2
                        color: panel.statusColor(projectItem.modelData.status)
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: panel.hostWindow.dp(3)
                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                Layout.fillWidth: true
                                text: projectItem.modelData.name
                                color: panel.colors.textPrimary
                                font.pixelSize: panel.hostWindow.dp(11)
                                font.weight: Font.Medium
                                elide: Text.ElideRight
                            }
                            Text {
                                text: projectItem.modelData.updated
                                color: panel.colors.textMuted
                                font.pixelSize: panel.hostWindow.dp(9)
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: panel.hostWindow.dp(10)
                            Text {
                                Layout.preferredWidth: panel.hostWindow.dp(112)
                                text: projectItem.modelData.taskLabel
                                color: panel.colors.textSecondary
                                font.pixelSize: panel.hostWindow.dp(9)
                                elide: Text.ElideRight
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: panel.hostWindow.dp(5)
                                radius: height / 2
                                color: panel.colors.buttonBackground
                                Rectangle {
                                    width: parent.width * Math.max(0, Math.min(1, projectItem.modelData.progress))
                                    height: parent.height
                                    radius: height / 2
                                    color: panel.statusColor(projectItem.modelData.status)
                                }
                            }
                            Text {
                                Layout.preferredWidth: panel.hostWindow.dp(34)
                                text: Math.round(projectItem.modelData.progress * 100) + "%"
                                color: panel.colors.textMuted
                                horizontalAlignment: Text.AlignRight
                                font.pixelSize: panel.hostWindow.dp(9)
                            }
                        }
                    }
                }
            }
        }
        Item { Layout.fillHeight: true }
    }
}
