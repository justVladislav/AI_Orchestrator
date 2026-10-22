import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog

    required property var hostWindow
    required property var colors

    property bool editMode: false
    property string providerId: ""

    signal providerAccepted(bool editMode,
                            string providerId,
                            string name,
                            string providerType,
                            string baseUrl,
                            string credential)

    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - hostWindow.dp(32) : hostWindow.dp(540),
                    hostWindow.dp(540))
    modal: true
    standardButtons: Dialog.NoButton
    font.pixelSize: hostWindow.dp(12)

    function openForCreate() {
        editMode = false
        providerId = ""
        nameField.clear()
        typeCombo.currentIndex = 0
        baseUrlField.clear()
        credentialField.clear()
        open()
    }

    function openForEdit(providerData) {
        if (!providerData)
            return
        editMode = true
        providerId = providerData.providerId
        nameField.text = providerData.name
        const typeIndex = typeCombo.model.indexOf(providerData.type)
        typeCombo.currentIndex = Math.max(0, typeIndex)
        baseUrlField.text = providerData.baseUrl
        credentialField.clear()
        open()
    }

    onOpened: nameField.forceActiveFocus()

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
            text: dialog.editMode ? qsTr("Edit provider") : qsTr("Add provider")
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
        spacing: dialog.hostWindow.dp(11)

        Text {
            Layout.fillWidth: true
            text: qsTr("Configure persisted connection metadata. Secure credentials will be configured separately in a later stage.")
            color: dialog.colors.textSecondary
            wrapMode: Text.WordWrap
            font.pixelSize: dialog.hostWindow.dp(11)
        }

        Text {
            text: qsTr("Display name")
            color: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(11)
            font.weight: Font.Medium
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: dialog.hostWindow.dp(38)
            radius: dialog.colors.cornerRadiusSmall
            color: dialog.colors.inputBackground
            border.width: nameField.activeFocus ? 1 : 0
            border.color: dialog.colors.inputBorderFocused
            Text { anchors.fill: parent; anchors.leftMargin: dialog.hostWindow.dp(11); text: qsTr("Provider name"); visible: nameField.text.length === 0 && !nameField.activeFocus; color: dialog.colors.textMuted; font.pixelSize: dialog.hostWindow.dp(12); verticalAlignment: Text.AlignVCenter }
            TextInput { id: nameField; anchors.fill: parent; anchors.leftMargin: dialog.hostWindow.dp(11); anchors.rightMargin: dialog.hostWindow.dp(11); color: dialog.colors.textPrimary; font.pixelSize: dialog.hostWindow.dp(12); verticalAlignment: TextInput.AlignVCenter; selectByMouse: true }
        }

        Text {
            text: qsTr("Provider type")
            color: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(11)
            font.weight: Font.Medium
        }

        ProviderComboBox {
            id: typeCombo
            Layout.fillWidth: true
            Layout.preferredHeight: dialog.hostWindow.dp(38)
            hostWindow: dialog.hostWindow
            colors: dialog.colors
            // Values map one-to-one to the provider kinds validated by the
            // current Backend catalog.
            model: ["OpenAI", "OpenAI-compatible", "Anthropic", qsTr("Local runtime")]
        }

        Text {
            text: qsTr("Base URL")
            color: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(11)
            font.weight: Font.Medium
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: dialog.hostWindow.dp(38)
            radius: dialog.colors.cornerRadiusSmall
            color: dialog.colors.inputBackground
            border.width: baseUrlField.activeFocus ? 1 : 0
            border.color: dialog.colors.inputBorderFocused
            Text { anchors.fill: parent; anchors.leftMargin: dialog.hostWindow.dp(11); text: "https://api.example.com/v1"; visible: baseUrlField.text.length === 0 && !baseUrlField.activeFocus; color: dialog.colors.textMuted; font.pixelSize: dialog.hostWindow.dp(12); verticalAlignment: Text.AlignVCenter }
            TextInput { id: baseUrlField; anchors.fill: parent; anchors.leftMargin: dialog.hostWindow.dp(11); anchors.rightMargin: dialog.hostWindow.dp(11); color: dialog.colors.textPrimary; font.pixelSize: dialog.hostWindow.dp(12); verticalAlignment: TextInput.AlignVCenter; selectByMouse: true }
        }

        Text {
            text: dialog.editMode ? qsTr("Replace credential (optional)") : qsTr("Credential")
            visible: false
            color: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(11)
            font.weight: Font.Medium
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: dialog.hostWindow.dp(38)
            visible: false
            radius: dialog.colors.cornerRadiusSmall
            color: dialog.colors.inputBackground
            border.width: credentialField.activeFocus ? 1 : 0
            border.color: dialog.colors.inputBorderFocused
            Text { anchors.fill: parent; anchors.leftMargin: dialog.hostWindow.dp(11); text: dialog.editMode ? qsTr("Leave empty to keep the current credential") : qsTr("Enter API key or token"); visible: credentialField.text.length === 0 && !credentialField.activeFocus; color: dialog.colors.textMuted; font.pixelSize: dialog.hostWindow.dp(11); verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
            TextInput { id: credentialField; enabled: false; anchors.fill: parent; anchors.leftMargin: dialog.hostWindow.dp(11); anchors.rightMargin: dialog.hostWindow.dp(11); echoMode: TextInput.Password; color: dialog.colors.textPrimary; font.pixelSize: dialog.hostWindow.dp(12); verticalAlignment: TextInput.AlignVCenter; selectByMouse: true }
        }

        Text {
            Layout.fillWidth: true
            text: qsTr("Credentials are disabled until a secure OS credential-store adapter is available. Secrets are never stored in the catalog database.")
            color: dialog.colors.warning
            wrapMode: Text.WordWrap
            font.pixelSize: dialog.hostWindow.dp(9)
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: dialog.hostWindow.dp(4)
            spacing: dialog.hostWindow.dp(8)

            Item { Layout.fillWidth: true }

            ProviderActionButton {
                text: qsTr("Cancel")
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                onClicked: dialog.reject()
            }

            ProviderActionButton {
                text: dialog.editMode ? qsTr("Save changes") : qsTr("Add provider")
                primary: true
                enabled: nameField.text.trim().length > 0
                         && baseUrlField.text.trim().length > 0
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                onClicked: {
                    dialog.providerAccepted(dialog.editMode,
                                            dialog.providerId,
                                            nameField.text.trim(),
                                            String(typeCombo.currentText),
                                            baseUrlField.text.trim(),
                                            "")
                    credentialField.clear()
                    dialog.accept()
                }
            }
        }
    }
}
