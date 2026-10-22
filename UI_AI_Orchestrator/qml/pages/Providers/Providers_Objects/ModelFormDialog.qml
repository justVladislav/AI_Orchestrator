import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog

    required property var hostWindow
    required property var colors

    property string providerId: ""
    property bool editMode: false
    // Internal DB model id (stable primary key) - only meaningful/used in
    // editMode, never shown in the form itself (the "Model ID" field below
    // is the provider's own apiModelId, a separate editable string).
    property string editingModelId: ""

    signal modelAccepted(string providerId,
                         string modelId,
                         string name,
                         string contextLabel,
                         string inputPrice,
                         string outputPrice)
    signal modelEditAccepted(string modelId,
                             string apiModelId,
                             string name,
                             string contextLabel,
                             string maxOutputTokens,
                             string inputPrice,
                             string outputPrice)

    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - hostWindow.dp(32) : hostWindow.dp(500),
                    hostWindow.dp(500))
    modal: true
    standardButtons: Dialog.NoButton

    function decimalPrice(micros) {
        let value = (Number(micros || 0) / 1000000).toFixed(6)
        value = value.replace(/0+$/, "").replace(/\.$/, "")
        return value.length > 0 ? value : "0"
    }

    function openForProvider(targetProviderId) {
        editMode = false
        editingModelId = ""
        providerId = targetProviderId
        modelIdField.clear()
        nameField.clear()
        contextField.text = "128K"
        maxOutputField.text = "4096"
        inputPriceField.text = "0.00"
        outputPriceField.text = "0.00"
        open()
    }

    function openForEdit(modelData) {
        editMode = true
        editingModelId = modelData.modelId
        providerId = modelData.providerId
        modelIdField.text = modelData.apiModelId
        nameField.text = modelData.name
        contextField.text = String(modelData.contextWindowTokens || 0)
        maxOutputField.text = String(modelData.maxOutputTokens || 0)
        inputPriceField.text = decimalPrice(modelData.inputPriceMicros)
        outputPriceField.text = decimalPrice(modelData.outputPriceMicros)
        open()
    }

    onOpened: modelIdField.forceActiveFocus()

    background: Rectangle {
        radius: dialog.colors.cornerRadiusMedium
        color: dialog.colors.popupBackground
        border.width: 1
        border.color: dialog.colors.border
    }

    header: Rectangle {
        implicitHeight: dialog.hostWindow.dp(50)
        color: dialog.colors.surfaceAlternative

        Text {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: dialog.hostWindow.dp(18)
            anchors.rightMargin: dialog.hostWindow.dp(18)
            text: dialog.editMode ? qsTr("Edit model") : qsTr("Add model")
            color: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(15)
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: dialog.colors.separator
        }
    }

    contentItem: ColumnLayout {
        spacing: dialog.hostWindow.dp(10)

        Text { text: qsTr("Model ID"); color: dialog.colors.textPrimary; font.pixelSize: dialog.hostWindow.dp(11); font.weight: Font.Medium }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: dialog.hostWindow.dp(36)
            radius: dialog.colors.cornerRadiusSmall
            color: dialog.colors.inputBackground
            border.width: modelIdField.activeFocus ? 1 : 0
            border.color: dialog.colors.inputBorderFocused
            Text { anchors.fill: parent; anchors.leftMargin: dialog.hostWindow.dp(11); text: "model-id"; visible: modelIdField.text.length === 0 && !modelIdField.activeFocus; color: dialog.colors.textMuted; font.pixelSize: dialog.hostWindow.dp(11); verticalAlignment: Text.AlignVCenter }
            TextInput { id: modelIdField; anchors.fill: parent; anchors.leftMargin: dialog.hostWindow.dp(11); anchors.rightMargin: dialog.hostWindow.dp(11); color: dialog.colors.textPrimary; font.pixelSize: dialog.hostWindow.dp(11); verticalAlignment: TextInput.AlignVCenter; selectByMouse: true }
        }

        Text { text: qsTr("Display name"); color: dialog.colors.textPrimary; font.pixelSize: dialog.hostWindow.dp(11); font.weight: Font.Medium }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: dialog.hostWindow.dp(36)
            radius: dialog.colors.cornerRadiusSmall
            color: dialog.colors.inputBackground
            border.width: nameField.activeFocus ? 1 : 0
            border.color: dialog.colors.inputBorderFocused
            Text { anchors.fill: parent; anchors.leftMargin: dialog.hostWindow.dp(11); text: qsTr("Model name"); visible: nameField.text.length === 0 && !nameField.activeFocus; color: dialog.colors.textMuted; font.pixelSize: dialog.hostWindow.dp(11); verticalAlignment: Text.AlignVCenter }
            TextInput { id: nameField; anchors.fill: parent; anchors.leftMargin: dialog.hostWindow.dp(11); anchors.rightMargin: dialog.hostWindow.dp(11); color: dialog.colors.textPrimary; font.pixelSize: dialog.hostWindow.dp(11); verticalAlignment: TextInput.AlignVCenter; selectByMouse: true }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: dialog.hostWindow.dp(10)

            ColumnLayout {
                Layout.fillWidth: true
                spacing: dialog.hostWindow.dp(5)
                Text { text: qsTr("Context window"); color: dialog.colors.textPrimary; font.pixelSize: dialog.hostWindow.dp(11); font.weight: Font.Medium }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: dialog.hostWindow.dp(36)
                    radius: dialog.colors.cornerRadiusSmall
                    color: dialog.colors.inputBackground
                    border.width: contextField.activeFocus ? 1 : 0
                    border.color: dialog.colors.inputBorderFocused
                    TextInput { id: contextField; anchors.fill: parent; anchors.leftMargin: dialog.hostWindow.dp(11); anchors.rightMargin: dialog.hostWindow.dp(11); color: dialog.colors.textPrimary; font.pixelSize: dialog.hostWindow.dp(11); verticalAlignment: TextInput.AlignVCenter; selectByMouse: true }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: dialog.hostWindow.dp(5)
                Text { text: qsTr("Max output"); color: dialog.colors.textPrimary; font.pixelSize: dialog.hostWindow.dp(11); font.weight: Font.Medium }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: dialog.hostWindow.dp(36)
                    radius: dialog.colors.cornerRadiusSmall
                    color: dialog.colors.inputBackground
                    border.width: maxOutputField.activeFocus ? 1 : 0
                    border.color: dialog.colors.inputBorderFocused
                    TextInput { id: maxOutputField; anchors.fill: parent; anchors.leftMargin: dialog.hostWindow.dp(11); anchors.rightMargin: dialog.hostWindow.dp(11); color: dialog.colors.textPrimary; font.pixelSize: dialog.hostWindow.dp(11); verticalAlignment: TextInput.AlignVCenter; selectByMouse: true; validator: IntValidator { bottom: 1 } }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: dialog.hostWindow.dp(5)
                Text { text: qsTr("Input / 1M"); color: dialog.colors.textPrimary; font.pixelSize: dialog.hostWindow.dp(11); font.weight: Font.Medium }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: dialog.hostWindow.dp(36)
                    radius: dialog.colors.cornerRadiusSmall
                    color: dialog.colors.inputBackground
                    border.width: inputPriceField.activeFocus ? 1 : 0
                    border.color: dialog.colors.inputBorderFocused

                    TextInput {
                        id: inputPriceField
                        anchors.fill: parent
                        anchors.leftMargin: dialog.hostWindow.dp(11)
                        anchors.rightMargin: dialog.hostWindow.dp(11)
                        color: dialog.colors.textPrimary
                        font.pixelSize: dialog.hostWindow.dp(11)
                        verticalAlignment: TextInput.AlignVCenter
                        selectByMouse: true
                        validator: DoubleValidator { bottom: 0 }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: dialog.hostWindow.dp(5)
                Text { text: qsTr("Output / 1M"); color: dialog.colors.textPrimary; font.pixelSize: dialog.hostWindow.dp(11); font.weight: Font.Medium }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: dialog.hostWindow.dp(36)
                    radius: dialog.colors.cornerRadiusSmall
                    color: dialog.colors.inputBackground
                    border.width: outputPriceField.activeFocus ? 1 : 0
                    border.color: dialog.colors.inputBorderFocused

                    TextInput {
                        id: outputPriceField
                        anchors.fill: parent
                        anchors.leftMargin: dialog.hostWindow.dp(11)
                        anchors.rightMargin: dialog.hostWindow.dp(11)
                        color: dialog.colors.textPrimary
                        font.pixelSize: dialog.hostWindow.dp(11)
                        verticalAlignment: TextInput.AlignVCenter
                        selectByMouse: true
                        validator: DoubleValidator { bottom: 0 }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: dialog.hostWindow.dp(4)

            Item { Layout.fillWidth: true }

            ProviderActionButton {
                text: qsTr("Cancel")
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                onClicked: dialog.reject()
            }

            ProviderActionButton {
                text: dialog.editMode ? qsTr("Save changes") : qsTr("Add model")
                primary: true
                enabled: modelIdField.text.trim().length > 0
                         && nameField.text.trim().length > 0
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                onClicked: {
                    if (dialog.editMode) {
                        dialog.modelEditAccepted(dialog.editingModelId,
                                                  modelIdField.text.trim(),
                                                  nameField.text.trim(),
                                                  contextField.text.trim(),
                                                  maxOutputField.text.trim(),
                                                  inputPriceField.text.trim(),
                                                  outputPriceField.text.trim())
                    } else {
                        dialog.modelAccepted(dialog.providerId,
                                             modelIdField.text.trim(),
                                             nameField.text.trim(),
                                             contextField.text.trim(),
                                             inputPriceField.text.trim(),
                                             outputPriceField.text.trim())
                    }
                    dialog.accept()
                }
            }
        }
    }
}
