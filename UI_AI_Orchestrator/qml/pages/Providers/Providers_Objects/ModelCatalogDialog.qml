pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog

    required property var hostWindow
    required property var colors

    property string providerId: ""
    property string providerName: ""
    property var availableModels: []
    property var selectedModelIds: []

    signal modelsAccepted(string providerId, var models)

    function openForProvider(targetProviderId, targetProviderName, models) {
        providerId = targetProviderId
        providerName = targetProviderName
        availableModels = models || []
        selectedModelIds = []
        open()
    }

    function containsSelection(apiModelId) {
        return selectedModelIds.indexOf(apiModelId) >= 0
    }

    function setSelected(apiModelId, selected) {
        const next = selectedModelIds.slice()
        const index = next.indexOf(apiModelId)
        if (selected && index < 0)
            next.push(apiModelId)
        else if (!selected && index >= 0)
            next.splice(index, 1)
        selectedModelIds = next
    }

    function selectedModels() {
        return availableModels.filter(function(model) {
            return containsSelection(String(model.apiModelId))
        })
    }

    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - hostWindow.dp(32) : hostWindow.dp(620), hostWindow.dp(620))
    height: Math.min(parent ? parent.height - hostWindow.dp(32) : hostWindow.dp(620), hostWindow.dp(620))
    modal: true
    title: qsTr("Add models from catalog")
    standardButtons: Dialog.NoButton

    background: Rectangle {
        radius: dialog.colors.cornerRadiusMedium
        color: dialog.colors.popupBackground
        border.width: 1
        border.color: dialog.colors.borderStrong
    }

    header: Rectangle {
        implicitHeight: dialog.hostWindow.dp(52)
        color: dialog.colors.surfaceAlternative
        border.width: 1
        border.color: dialog.colors.border
        Text {
            anchors.fill: parent
            anchors.leftMargin: dialog.hostWindow.dp(16)
            verticalAlignment: Text.AlignVCenter
            text: qsTr("Models for %1").arg(dialog.providerName)
            color: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(14)
            font.weight: Font.DemiBold
        }
    }

    contentItem: ColumnLayout {
        spacing: dialog.hostWindow.dp(12)

        Text {
            Layout.fillWidth: true
            text: qsTr("Select models that should be available for execution profiles, runs, chats and comparisons.")
            color: dialog.colors.textSecondary
            font.pixelSize: dialog.hostWindow.dp(11)
            wrapMode: Text.WordWrap
        }

        Text {
            Layout.fillWidth: true
            text: qsTr("Pricing is intentionally not prefilled and will be synchronized separately.")
            color: dialog.colors.textMuted
            font.pixelSize: dialog.hostWindow.dp(9)
            wrapMode: Text.WordWrap
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: dialog.colors.cornerRadiusSmall
            color: dialog.colors.inputBackground
            border.width: 1
            border.color: dialog.colors.border
            clip: true

            ListView {
                id: catalogList
                anchors.fill: parent
                anchors.margins: dialog.hostWindow.dp(6)
                model: dialog.availableModels
                spacing: dialog.hostWindow.dp(4)
                clip: true

                delegate: Rectangle {
                    id: modelRow
                    required property var modelData

                    width: ListView.view.width
                    height: dialog.hostWindow.dp(58)
                    radius: dialog.colors.cornerRadiusSmall
                    color: modelCheck.checked ? dialog.colors.accentSoft : "transparent"

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: dialog.hostWindow.dp(8)
                        spacing: dialog.hostWindow.dp(10)

                        CheckBox {
                            id: modelCheck
                            checked: dialog.containsSelection(String(modelRow.modelData.apiModelId))
                            onToggled: dialog.setSelected(
                                String(modelRow.modelData.apiModelId), checked)
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: dialog.hostWindow.dp(2)
                            Text {
                                Layout.fillWidth: true
                                text: String(modelRow.modelData.displayName)
                                color: dialog.colors.textPrimary
                                font.pixelSize: dialog.hostWindow.dp(11)
                                font.weight: Font.Medium
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                text: String(modelRow.modelData.apiModelId)
                                      + qsTr(" · context %1K").arg(
                                            Number(modelRow.modelData.contextWindowTokens) / 1000)
                                color: dialog.colors.textMuted
                                font.pixelSize: dialog.hostWindow.dp(9)
                                elide: Text.ElideRight
                            }
                        }
                    }
                }

                Text {
                    anchors.centerIn: parent
                    visible: catalogList.count === 0
                    text: qsTr("All catalog models are already configured")
                    color: dialog.colors.textMuted
                    font.pixelSize: dialog.hostWindow.dp(11)
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }

            ProviderActionButton {
                text: qsTr("Cancel")
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                onClicked: dialog.reject()
            }

            ProviderActionButton {
                text: qsTr("Add selected (%1)").arg(dialog.selectedModelIds.length)
                primary: true
                enabled: dialog.selectedModelIds.length > 0
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                onClicked: {
                    dialog.modelsAccepted(dialog.providerId, dialog.selectedModels())
                    dialog.accept()
                }
            }
        }
    }
}
