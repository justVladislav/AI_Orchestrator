pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

Rectangle {
    id: panel

    required property var hostWindow
    required property var colors
    required property var profilesModel

    signal createRequested()
    signal editRequested(var profileData)
    signal defaultRequested(string profileId)

    Layout.fillWidth: true
    implicitHeight: profilesColumn.implicitHeight + hostWindow.dp(28)
    radius: colors.cornerRadiusMedium
    color: colors.surface
    border.width: 1
    border.color: colors.border

    ColumnLayout {
        id: profilesColumn
        anchors.fill: parent
        anchors.margins: panel.hostWindow.dp(14)
        spacing: panel.hostWindow.dp(10)

        RowLayout {
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(10)

            ColumnLayout {
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(2)
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Execution profiles")
                    color: panel.colors.textPrimary
                    font.pixelSize: panel.hostWindow.dp(15)
                    font.weight: Font.DemiBold
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Assign a provider, model and timeout to every production stage.")
                    color: panel.colors.textMuted
                    font.pixelSize: panel.hostWindow.dp(10)
                    wrapMode: Text.WordWrap
                }
            }

            ProviderActionButton {
                text: qsTr("New profile")
                iconText: "+"
                primary: true
                hostWindow: panel.hostWindow
                colors: panel.colors
                onClicked: panel.createRequested()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: panel.colors.separator
        }

        Repeater {
            model: panel.profilesModel

            delegate: Rectangle {
                id: profileRow
                required property string profileId
                required property string name
                required property string workflowId
                required property bool profileEnabled
                required property bool isDefault
                required property var assignments
                required property var providerBudget

                Layout.fillWidth: true
                Layout.preferredHeight: rowLayout.implicitHeight + panel.hostWindow.dp(20)
                radius: panel.colors.cornerRadiusSmall
                color: panel.colors.inputBackground
                border.width: profileRow.isDefault ? 1 : 0
                border.color: panel.colors.accent

                RowLayout {
                    id: rowLayout
                    anchors.fill: parent
                    anchors.margins: panel.hostWindow.dp(10)
                    spacing: panel.hostWindow.dp(10)

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: panel.hostWindow.dp(2)
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: panel.hostWindow.dp(7)
                            Text {
                                Layout.fillWidth: true
                                text: profileRow.name
                                color: panel.colors.textPrimary
                                font.pixelSize: panel.hostWindow.dp(11)
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            Text {
                                visible: profileRow.isDefault
                                text: qsTr("Default")
                                color: panel.colors.accent
                                font.pixelSize: panel.hostWindow.dp(9)
                                font.weight: Font.DemiBold
                            }
                            Text {
                                visible: !profileRow.profileEnabled
                                text: qsTr("Disabled")
                                color: panel.colors.warning
                                font.pixelSize: panel.hostWindow.dp(9)
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("%1 · %2 assignments")
                                .arg(profileRow.workflowId)
                                .arg(profileRow.assignments.length)
                            color: panel.colors.textMuted
                            font.pixelSize: panel.hostWindow.dp(9)
                            elide: Text.ElideRight
                        }
                    }

                    ProviderActionButton {
                        text: qsTr("Make default")
                        visible: !profileRow.isDefault && profileRow.profileEnabled
                        hostWindow: panel.hostWindow
                        colors: panel.colors
                        onClicked: panel.defaultRequested(profileRow.profileId)
                    }

                    ProviderActionButton {
                        text: qsTr("Edit")
                        hostWindow: panel.hostWindow
                        colors: panel.colors
                        onClicked: panel.editRequested({
                            profileId: profileRow.profileId,
                            name: profileRow.name,
                            workflowId: profileRow.workflowId,
                            enabled: profileRow.profileEnabled,
                            isDefault: profileRow.isDefault,
                            assignments: profileRow.assignments,
                            providerBudget: profileRow.providerBudget
                        })
                    }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.preferredHeight: panel.profilesModel.length === 0
                                    ? panel.hostWindow.dp(54) : 0
            visible: panel.profilesModel.length === 0
            text: qsTr("No execution profiles yet. Add providers and models first, then create a profile.")
            color: panel.colors.textMuted
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(10)
        }
    }
}
