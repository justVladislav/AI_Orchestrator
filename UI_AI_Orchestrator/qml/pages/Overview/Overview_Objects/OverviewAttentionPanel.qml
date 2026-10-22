pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

Rectangle {
    id: panel

    required property var hostWindow
    required property var colors
    required property string projectName
    required property var attentionModel
    signal itemRequested(var itemData)

    implicitHeight: hostWindow.dp(312)
    radius: colors.cornerRadiusMedium
    color: colors.surfaceAlternative
    border.width: 1
    border.color: colors.border

    function severityColor(severity) {
        if (severity === "error") return colors.error
        if (severity === "warning") return colors.warning
        return colors.information
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: panel.hostWindow.dp(16)
        spacing: panel.hostWindow.dp(10)

        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(2)
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Needs attention")
                    color: panel.colors.textPrimary
                    font.pixelSize: panel.hostWindow.dp(15)
                    font.weight: Font.DemiBold
                }
                Text {
                    Layout.fillWidth: true
                    text: panel.projectName
                    color: panel.colors.textMuted
                    font.pixelSize: panel.hostWindow.dp(10)
                    elide: Text.ElideRight
                }
            }
            Rectangle {
                Layout.preferredWidth: countText.implicitWidth + panel.hostWindow.dp(18)
                Layout.preferredHeight: panel.hostWindow.dp(26)
                radius: height / 2
                color: panel.attentionModel.length > 0 ? panel.colors.warningSoft : panel.colors.successSoft
                Text {
                    id: countText
                    anchors.centerIn: parent
                    text: panel.attentionModel.length > 0
                          ? qsTr("%1 tasks").arg(panel.attentionModel.length) : qsTr("All clear")
                    color: panel.attentionModel.length > 0 ? panel.colors.warning : panel.colors.success
                    font.pixelSize: panel.hostWindow.dp(9)
                    font.weight: Font.DemiBold
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: panel.colors.separator }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignHCenter
            visible: panel.attentionModel.length === 0
            spacing: panel.hostWindow.dp(8)
            Item { Layout.fillHeight: true }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: "✓"
                color: panel.colors.success
                font.pixelSize: panel.hostWindow.dp(28)
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("No action required")
                color: panel.colors.textPrimary
                font.pixelSize: panel.hostWindow.dp(12)
                font.weight: Font.Medium
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("No tasks are blocked, overdue or marked for attention.")
                color: panel.colors.textMuted
                font.pixelSize: panel.hostWindow.dp(9)
            }
            Item { Layout.fillHeight: true }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(4)
            visible: panel.attentionModel.length > 0

            Repeater {
              model: panel.attentionModel
              delegate: Basic.AbstractButton {
                id: attentionItem
                required property var modelData
                Layout.fillWidth: true
                Layout.preferredHeight: panel.hostWindow.dp(48)
                hoverEnabled: true
                onClicked: panel.itemRequested(modelData)

                background: Rectangle {
                    radius: panel.colors.cornerRadiusSmall
                    color: attentionItem.hovered ? panel.colors.buttonHover : "transparent"
                }

                contentItem: RowLayout {
                    spacing: panel.hostWindow.dp(9)
                    Rectangle {
                        Layout.preferredWidth: panel.hostWindow.dp(4)
                        Layout.fillHeight: true
                        radius: width / 2
                        color: panel.severityColor(attentionItem.modelData.severity)
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: panel.hostWindow.dp(2)
                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                Layout.fillWidth: true
                                text: attentionItem.modelData.title
                                color: panel.colors.textPrimary
                                font.pixelSize: panel.hostWindow.dp(10.5)
                                font.weight: Font.Medium
                                elide: Text.ElideRight
                            }
                            Text {
                                text: attentionItem.modelData.time
                                color: panel.colors.textMuted
                                font.pixelSize: panel.hostWindow.dp(8.5)
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: attentionItem.modelData.detail
                            color: panel.colors.textSecondary
                            font.pixelSize: panel.hostWindow.dp(9)
                            elide: Text.ElideRight
                        }
                    }
                }
              }
            }
        }
        Item { Layout.fillHeight: true }
    }
}
