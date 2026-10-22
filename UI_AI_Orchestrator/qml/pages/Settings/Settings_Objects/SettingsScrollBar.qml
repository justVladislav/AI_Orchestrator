import QtQuick
import QtQuick.Controls.Basic as Basic

Basic.ScrollBar {
    id: control

    required property var hostWindow
    required property var colors

    policy: Basic.ScrollBar.AsNeeded
    interactive: true
    hoverEnabled: true
    padding: hostWindow.dp(2)
    implicitWidth: hostWindow.dp(9)
    minimumSize: 0.08

    contentItem: Rectangle {
        implicitWidth: control.hostWindow.dp(4)
        radius: width / 2
        color: control.pressed
               ? control.colors.accent
               : control.hovered
                 ? control.colors.borderStrong
                 : control.colors.textMuted
        opacity: control.active || control.hovered ? 0.72 : 0.30
    }

    background: Rectangle {
        color: "transparent"
    }
}

