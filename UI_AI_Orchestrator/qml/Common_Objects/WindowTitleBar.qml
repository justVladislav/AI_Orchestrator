import QtQuick
import QtQuick.Layouts
import QtQuick.Window

Rectangle {
    id: titleBar

    required property var hostWindow
    required property var colors
    property string titleText: ""

    readonly property bool isMaximized: hostWindow.visibility === Window.Maximized

    height: colors.titleBarHeight
    color: colors.surface
    border.width: 0

    function toggleMaximized() {
        if (isMaximized)
            hostWindow.showNormal()
        else
            hostWindow.showMaximized()
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            RowLayout {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: titleBar.hostWindow.dp(12)
                spacing: titleBar.hostWindow.dp(9)

                Rectangle {
                    Layout.preferredWidth: titleBar.hostWindow.dp(24)
                    Layout.preferredHeight: titleBar.hostWindow.dp(24)
                    radius: titleBar.colors.cornerRadiusSmall / 2
                    color: titleBar.colors.accent

                    Text {
                        anchors.centerIn: parent
                        text: "AI"
                        color: titleBar.colors.textOnAccent
                        font.pixelSize: titleBar.hostWindow.dp(9)
                        font.weight: Font.Bold
                    }
                }

                Text {
                    Layout.maximumWidth: titleBar.hostWindow.dp(280)
                    text: titleBar.titleText
                    color: titleBar.colors.textPrimary
                    elide: Text.ElideRight
                    font.pixelSize: titleBar.hostWindow.dp(13)
                    font.weight: Font.Medium
                }
            }

            DragHandler {
                target: null
                acceptedButtons: Qt.LeftButton
                onActiveChanged: {
                    if (active && !titleBar.isMaximized)
                        titleBar.hostWindow.startSystemMove()
                }
            }

            TapHandler {
                acceptedButtons: Qt.LeftButton
                onDoubleTapped: titleBar.toggleMaximized()
            }
        }

        WindowControlButton {
            Layout.fillHeight: true
            hostWindow: titleBar.hostWindow
            colors: titleBar.colors
            controlType: WindowControlButton.Minimize
            onClicked: titleBar.hostWindow.showMinimized()
        }

        WindowControlButton {
            Layout.fillHeight: true
            hostWindow: titleBar.hostWindow
            colors: titleBar.colors
            controlType: WindowControlButton.Maximize
            onClicked: titleBar.toggleMaximized()
        }

        WindowControlButton {
            Layout.fillHeight: true
            hostWindow: titleBar.hostWindow
            colors: titleBar.colors
            controlType: WindowControlButton.Close
            onClicked: titleBar.hostWindow.close()
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: titleBar.colors.separator
    }
}
