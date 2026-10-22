import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog
    objectName: "credentialDialog"

    required property var hostWindow
    required property var colors
    property string providerId: ""
    property string providerName: ""
    property string errorText: ""

    signal credentialAccepted(string providerId, string secret)

    function openForProvider(id, name) {
        providerId = id
        providerName = name
        secretField.clear()
        errorText = ""
        open()
    }

    function submit() {
        if (secretField.text.length === 0) {
            errorText = qsTr("Credential cannot be empty")
            return
        }
        // Do not trim secrets: whitespace can be significant. The receiving
        // Qt controller forwards the value once and never retains it.
        credentialAccepted(providerId, secretField.text)
    }

    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - hostWindow.dp(32) : hostWindow.dp(470),
                    hostWindow.dp(470))
    modal: true
    title: qsTr("Configure credential")
    standardButtons: Dialog.NoButton

    onClosed: secretField.clear()

    background: Rectangle {
        radius: dialog.colors.cornerRadiusMedium
        color: dialog.colors.popupBackground
        border.width: 1
        border.color: dialog.colors.border
    }

    contentItem: ColumnLayout {
        spacing: dialog.hostWindow.dp(12)

        Text {
            Layout.fillWidth: true
            text: qsTr("Set or replace the credential for %1. The secret is sent to the OS credential store and can never be read back by this UI.").arg(dialog.providerName)
            color: dialog.colors.textSecondary
            wrapMode: Text.WordWrap
            font.pixelSize: dialog.hostWindow.dp(11)
        }

        TextField {
            id: secretField
            objectName: "credentialSecretField"
            Layout.fillWidth: true
            placeholderText: qsTr("API key or access token")
            echoMode: TextInput.Password
            passwordCharacter: "•"
            color: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(12)
            selectByMouse: true
        }

        Text {
            Layout.fillWidth: true
            visible: dialog.errorText.length > 0
            text: dialog.errorText
            color: dialog.colors.error
            wrapMode: Text.WordWrap
            font.pixelSize: dialog.hostWindow.dp(10)
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
                text: qsTr("Save credential")
                primary: true
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                onClicked: dialog.submit()
            }
        }
    }
}
