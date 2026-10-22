pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Large, strictly read-only side-by-side viewer for one change's Original and
// Proposed content. The inline diff boxes are small; this gives the reviewer a
// window-sized view whose text can be selected with the mouse and copied
// (per-pane "Copy" copies the whole pane). Both panes are TextArea with
// readOnly: true - nothing here can edit or write back to a proposal.
Dialog {
    id: dialog
    objectName: "diffViewDialog"

    required property var hostWindow
    required property var colors
    property string fileLabel: ""
    property string originalText: ""
    property string proposedText: ""

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.max(hostWindow.dp(360), hostWindow.width - hostWindow.dp(48))
    height: Math.max(hostWindow.dp(320), hostWindow.height - hostWindow.dp(48))
    modal: true
    title: dialog.fileLabel.length > 0
           ? qsTr("Diff: %1").arg(dialog.fileLabel) : qsTr("Diff")
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
            elide: Text.ElideMiddle
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
            text: qsTr("Read-only. Select text with the mouse and press Ctrl+C, or use Copy to take a whole side.")
            color: dialog.colors.textMuted
            font.pixelSize: dialog.hostWindow.dp(9.5)
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: dialog.hostWindow.dp(10)

            Repeater {
                model: [
                    { "title": qsTr("Original"),
                      "emptyText": qsTr("(no original content)"),
                      "content": dialog.originalText },
                    { "title": qsTr("Proposed"),
                      "emptyText": qsTr("(no proposed content)"),
                      "content": dialog.proposedText }
                ]
                delegate: ColumnLayout {
                    id: pane
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredWidth: 1
                    spacing: dialog.hostWindow.dp(4)

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: dialog.hostWindow.dp(8)
                        Text {
                            Layout.fillWidth: true
                            text: pane.modelData.title
                            color: dialog.colors.textMuted
                            font.pixelSize: dialog.hostWindow.dp(10)
                            font.weight: Font.DemiBold
                        }
                        RunCopyButton {
                            hostWindow: dialog.hostWindow
                            colors: dialog.colors
                            label: qsTr("Copy %1").arg(pane.modelData.title.toLowerCase())
                            copyText: pane.modelData.content
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: dialog.colors.cornerRadiusSmall
                        color: dialog.colors.surface
                        border.width: 1
                        border.color: dialog.colors.border

                        Flickable {
                            id: paneFlick
                            anchors.fill: parent
                            anchors.margins: dialog.hostWindow.dp(8)
                            clip: true
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: RunScrollBar {
                                hostWindow: dialog.hostWindow
                                colors: dialog.colors
                            }

                            TextArea.flickable: TextArea {
                                readOnly: true
                                selectByMouse: true
                                persistentSelection: true
                                text: pane.modelData.content.length > 0
                                      ? pane.modelData.content : pane.modelData.emptyText
                                textFormat: TextEdit.PlainText
                                wrapMode: TextEdit.WrapAnywhere
                                color: dialog.colors.textSecondary
                                selectionColor: dialog.colors.accent
                                selectedTextColor: dialog.colors.textOnAccent
                                font.pixelSize: dialog.hostWindow.dp(10)
                                font.family: "monospace"
                                padding: 0
                                background: Item {}
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: dialog.hostWindow.dp(8)
            Item { Layout.fillWidth: true }
            RunActionButton {
                objectName: "diffViewCloseButton"
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                text: qsTr("Close")
                primary: true
                onClicked: dialog.close()
            }
        }
    }
}
