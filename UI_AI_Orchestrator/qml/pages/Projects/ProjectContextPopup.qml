import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Projects_Objets"

Popup {
    id: popup

    required property var hostWindow
    required property var colors
    property string projectId: ""
    property string originalDescription: ""

    signal saveRequested(string projectId, string description)

    function openFor(pid, description) {
        popup.projectId = pid
        popup.originalDescription = description
        editArea.text = description
        popup.open()
        editArea.forceActiveFocus()
    }

    anchors.centerIn: parent
    width: Math.min(popup.hostWindow.dp(560), parent.width - popup.hostWindow.dp(48))
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape
    padding: 0

    background: Rectangle {
        radius: popup.colors.cornerRadiusMedium
        color: popup.colors.popupBackground
        border.width: 1
        border.color: popup.colors.border

        layer.enabled: true
        layer.effect: null
    }

    contentItem: ColumnLayout {
        spacing: 0

        // Header
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: popup.hostWindow.dp(48)
            color: popup.colors.surfaceAlternative
            topLeftRadius: popup.colors.cornerRadiusMedium
            topRightRadius: popup.colors.cornerRadiusMedium

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: popup.hostWindow.dp(16)
                anchors.rightMargin: popup.hostWindow.dp(8)
                spacing: popup.hostWindow.dp(8)

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Project context")
                    color: popup.colors.textPrimary
                    font.pixelSize: popup.hostWindow.dp(14)
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }

                // Close ×
                Rectangle {
                    Layout.preferredWidth: popup.hostWindow.dp(30)
                    Layout.preferredHeight: popup.hostWindow.dp(30)
                    Layout.alignment: Qt.AlignVCenter
                    radius: popup.colors.cornerRadiusSmall
                    color: closeArea.containsMouse ? popup.colors.buttonHover : "transparent"

                    Text {
                        anchors.centerIn: parent
                        text: "×"
                        color: popup.colors.textMuted
                        font.pixelSize: popup.hostWindow.dp(18)
                    }

                    MouseArea {
                        id: closeArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: popup.close()
                    }
                }
            }
        }

        // Body
        ColumnLayout {
            Layout.fillWidth: true
            Layout.topMargin: popup.hostWindow.dp(14)
            Layout.leftMargin: popup.hostWindow.dp(16)
            Layout.rightMargin: popup.hostWindow.dp(16)
            Layout.bottomMargin: popup.hostWindow.dp(14)
            spacing: popup.hostWindow.dp(10)

            Text {
                Layout.fillWidth: true
                text: qsTr("Shared context reused by Chat, Run, and Comparison. Describe goals, tech stack, and constraints for AI agents.")
                color: popup.colors.textMuted
                wrapMode: Text.WordWrap
                font.pixelSize: popup.hostWindow.dp(10.5)
            }

            // Editor area
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: popup.hostWindow.dp(220)
                radius: popup.colors.cornerRadiusSmall
                color: popup.colors.inputBackground
                border.width: editArea.activeFocus ? 1 : 0
                border.color: popup.colors.inputBorderFocused
                clip: true

                Flickable {
                    id: editFlickable
                    anchors.fill: parent
                    anchors.margins: popup.hostWindow.dp(8)
                    contentWidth: width
                    contentHeight: Math.max(editArea.implicitHeight, height)
                    boundsBehavior: Flickable.StopAtBounds
                    TextArea.flickable: editArea

                    TextArea {
                        id: editArea
                        width: Math.max(0, editFlickable.width
                                        - (editScrollBar.visible
                                           ? popup.hostWindow.dp(12)
                                           : popup.hostWindow.dp(4)))
                        wrapMode: TextEdit.Wrap
                        color: popup.colors.textPrimary
                        placeholderText: qsTr("Describe project goals, tech stack, constraints, and shared context for AI agents.")
                        placeholderTextColor: popup.colors.textMuted
                        font.pixelSize: popup.hostWindow.dp(12)
                        selectByMouse: true
                        background: null
                    }

                    ScrollBar.vertical: ScrollBar {
                        id: editScrollBar
                        visible: editFlickable.contentHeight > editFlickable.height
                        policy: ScrollBar.AsNeeded
                        contentItem: Rectangle {
                            implicitWidth: popup.hostWindow.dp(4)
                            radius: width / 2
                            color: editScrollBar.active  ? popup.colors.textSecondary
                                 : editScrollBar.hovered ? popup.colors.textMuted
                                 :                         popup.colors.border
                        }
                    }
                }
            }

            // Footer row: counter + unsaved indicator + buttons
            RowLayout {
                Layout.fillWidth: true
                spacing: popup.hostWindow.dp(6)

                Text {
                    text: qsTr("%1 chars").arg(editArea.text.length)
                    color: popup.colors.textMuted
                    font.pixelSize: popup.hostWindow.dp(9.5)
                }

                Text {
                    visible: editArea.text !== popup.originalDescription
                    text: qsTr("· Unsaved changes")
                    color: popup.colors.warning
                    font.pixelSize: popup.hostWindow.dp(9.5)
                }

                Item { Layout.fillWidth: true }

                ProjectActionButton {
                    text: qsTr("Close")
                    hostWindow: popup.hostWindow
                    colors: popup.colors
                    onClicked: popup.close()
                }

                ProjectActionButton {
                    text: qsTr("Save")
                    primary: true
                    enabled: editArea.text !== popup.originalDescription
                    hostWindow: popup.hostWindow
                    colors: popup.colors
                    onClicked: {
                        popup.saveRequested(popup.projectId, editArea.text)
                        popup.originalDescription = editArea.text
                    }
                }
            }
        }
    }
}
