import QtQuick
import QtQuick.Controls
import QtQuick.Window

AbstractButton {
    id: control

    enum ControlType {
        Minimize,
        Maximize,
        Close
    }

    required property var hostWindow
    required property var colors
    required property int controlType

    readonly property bool isCloseButton: controlType === WindowControlButton.Close
    readonly property bool isMaximized: hostWindow.visibility === Window.Maximized

    implicitWidth: hostWindow.dp(46)
    implicitHeight: hostWindow.dp(34)
    hoverEnabled: true

    Accessible.name: controlType === WindowControlButton.Minimize
                     ? qsTr("Minimize")
                     : controlType === WindowControlButton.Maximize
                       ? (isMaximized ? qsTr("Restore") : qsTr("Maximize"))
                       : qsTr("Close")

    ToolTip.visible: hovered
    ToolTip.delay: 700
    ToolTip.text: Accessible.name

    background: Rectangle {
        color: control.isCloseButton && control.hovered
               ? (control.down ? "#B91C1C" : "#E5484D")
               : control.down
                 ? control.colors.buttonPressed
                 : control.hovered
                   ? control.colors.buttonHover
                   : "transparent"
    }

    contentItem: Item {
        id: iconContainer

        implicitWidth: control.hostWindow.dp(16)
        implicitHeight: control.hostWindow.dp(16)

        readonly property color iconColor: control.isCloseButton && control.hovered
                                            ? "#FFFFFF"
                                            : control.colors.textSecondary

        Rectangle {
            visible: control.controlType === WindowControlButton.Minimize
            anchors.centerIn: parent
            width: control.hostWindow.dp(12)
            height: Math.max(1, control.hostWindow.dp(1))
            color: parent.iconColor
        }

        Item {
            visible: control.controlType === WindowControlButton.Maximize
                     && !control.isMaximized
            anchors.centerIn: parent
            width: control.hostWindow.dp(11)
            height: width

            Rectangle {
                anchors.fill: parent
                color: "transparent"
                border.width: Math.max(1, control.hostWindow.dp(1))
                border.color: iconContainer.iconColor
            }
        }

        Item {
            visible: control.controlType === WindowControlButton.Maximize
                     && control.isMaximized
            anchors.centerIn: parent
            width: control.hostWindow.dp(12)
            height: width

            Rectangle {
                x: control.hostWindow.dp(3)
                y: 0
                width: control.hostWindow.dp(9)
                height: control.hostWindow.dp(9)
                color: "transparent"
                border.width: Math.max(1, control.hostWindow.dp(1))
                border.color: iconContainer.iconColor
            }

            Rectangle {
                x: 0
                y: control.hostWindow.dp(3)
                width: control.hostWindow.dp(9)
                height: control.hostWindow.dp(9)
                color: control.colors.surface
                border.width: Math.max(1, control.hostWindow.dp(1))
                border.color: iconContainer.iconColor
            }
        }

        Item {
            visible: control.controlType === WindowControlButton.Close
            anchors.centerIn: parent
            width: control.hostWindow.dp(14)
            height: width

            Rectangle {
                anchors.centerIn: parent
                width: control.hostWindow.dp(15)
                height: Math.max(1, control.hostWindow.dp(1))
                rotation: 45
                color: iconContainer.iconColor
            }

            Rectangle {
                anchors.centerIn: parent
                width: control.hostWindow.dp(15)
                height: Math.max(1, control.hostWindow.dp(1))
                rotation: -45
                color: iconContainer.iconColor
            }
        }
    }
}
