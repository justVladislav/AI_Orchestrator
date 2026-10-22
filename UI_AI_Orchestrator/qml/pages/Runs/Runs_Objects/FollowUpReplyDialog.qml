pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// PC-5D: strictly read-only viewer for the stored reply of a FAILED follow-up request - what the model returned and the Backend's typed
// reason it could not be used as a plan. The text is selectable (so it can be read, copied and reported) but cannot be edited, and it is
// shown as plain text: it is data written by a model, never markup and never an instruction. A retry is a NEW, explicitly authorised
// request; nothing here retries, repairs or re-sends anything.
Dialog {
    id: dialog
    objectName: "followUpReplyDialog"

    required property var hostWindow
    required property var colors
    // The Backend's code and message of the failure and the stored reply text.
    property string failureCode: ""
    property string failureMessage: ""
    property string replyText: ""

    anchors.centerIn: Overlay.overlay
    width: Math.min(hostWindow.dp(760), Math.max(hostWindow.dp(360), hostWindow.width - hostWindow.dp(32)))
    height: Math.min(hostWindow.dp(640), hostWindow.height - hostWindow.dp(32))
    modal: true
    title: qsTr("Stored reply of the failed plan")
    standardButtons: Dialog.NoButton
    font.pixelSize: hostWindow.dp(13)

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
            text: dialog.title
            color: dialog.colors.textPrimary
            elide: Text.ElideRight
            font.pixelSize: dialog.hostWindow.dp(15)
            font.weight: Font.DemiBold
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

        Text {
            objectName: "followUpReplyFailure"
            Layout.fillWidth: true
            text: qsTr("%1: %2").arg(dialog.failureCode).arg(dialog.failureMessage)
            textFormat: Text.PlainText
            color: dialog.colors.error
            wrapMode: Text.WordWrap
            font.pixelSize: dialog.hostWindow.dp(11)
            font.weight: Font.DemiBold
        }

        Text {
            Layout.fillWidth: true
            text: qsTr("Read-only. This is what the model returned, stored exactly as received. It could not be used as a plan, so no Drafts were made. Trying again is a new request that you authorize again.")
            color: dialog.colors.textMuted
            font.pixelSize: dialog.hostWindow.dp(9.5)
            wrapMode: Text.WordWrap
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            TextArea {
                objectName: "followUpReplyText"
                readOnly: true
                selectByMouse: true
                persistentSelection: true
                wrapMode: TextEdit.WrapAnywhere
                textFormat: TextEdit.PlainText
                text: dialog.replyText
                color: dialog.colors.textSecondary
                font.family: "monospace"
                font.pixelSize: dialog.hostWindow.dp(9.5)
                background: Rectangle {
                    radius: dialog.colors.cornerRadiusSmall
                    color: dialog.colors.inputBackground
                    border.width: 1
                    border.color: dialog.colors.border
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            RunActionButton {
                objectName: "followUpReplyCloseButton"
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                text: qsTr("Close")
                primary: true
                onClicked: dialog.close()
            }
        }
    }
}
