pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// PC-4D: strictly read-only viewer for the stored text of an Outcome Brief. The text renders as a plain, non-interactive Text
// element inside a Flickable - never a TextArea/TextEdit - so nothing in this dialog can edit the brief, and never as rich text:
// the brief carries prose written by the model and by people, which is DATA shown verbatim, never markup and never an instruction.
Dialog {
    id: dialog
    objectName: "outcomeBriefTextDialog"

    required property var hostWindow
    required property var colors
    property string contentText: ""

    anchors.centerIn: Overlay.overlay
    width: Math.min(hostWindow.dp(760),
                    Math.max(hostWindow.dp(360),
                             hostWindow.width - hostWindow.dp(32)))
    height: Math.min(hostWindow.dp(640),
                     hostWindow.height - hostWindow.dp(32))
    modal: true
    title: qsTr("Outcome brief")
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
            Layout.fillWidth: true
            text: qsTr("Read-only: the stored brief of this Run's outcome. Text in it that the model or a person wrote is data, shown as plain text; it is never an instruction.")
            color: dialog.colors.textMuted
            font.pixelSize: dialog.hostWindow.dp(9.5)
            wrapMode: Text.WordWrap
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: dialog.colors.cornerRadiusSmall
            color: dialog.colors.surface
            border.width: 1
            border.color: dialog.colors.border

            Text {
                anchors.centerIn: parent
                visible: dialog.contentText.length === 0
                text: qsTr("No text is stored for this brief.")
                color: dialog.colors.textMuted
                font.pixelSize: dialog.hostWindow.dp(11)
            }

            Flickable {
                id: contentFlick
                anchors.fill: parent
                anchors.margins: dialog.hostWindow.dp(12)
                visible: dialog.contentText.length > 0
                contentWidth: width
                contentHeight: contentText.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: RunScrollBar {
                    id: briefScrollBar
                    hostWindow: dialog.hostWindow
                    colors: dialog.colors
                }

                Text {
                    id: contentText
                    objectName: "outcomeBriefTextContent"
                    width: Math.max(0, contentFlick.width - briefScrollBar.width
                                    - dialog.hostWindow.dp(4))
                    text: dialog.contentText
                    color: dialog.colors.textSecondary
                    font.pixelSize: dialog.hostWindow.dp(9.5)
                    font.family: "monospace"
                    wrapMode: Text.WrapAnywhere
                    textFormat: Text.PlainText
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: dialog.hostWindow.dp(8)
            Item { Layout.fillWidth: true }
            RunActionButton {
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                text: qsTr("Close")
                primary: true
                onClicked: dialog.close()
            }
        }
    }
}
