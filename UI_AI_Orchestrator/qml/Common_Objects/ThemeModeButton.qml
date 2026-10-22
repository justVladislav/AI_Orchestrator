import QtQuick
import QtQuick.Controls

AbstractButton {
    id: control

    required property var hostWindow
    required property var colors
    required property string modeName
    required property url iconSource
    required property string accessibleLabel

    implicitWidth: hostWindow.dp(40)
    implicitHeight: hostWindow.dp(40)
    checkable: true

    Accessible.name: accessibleLabel
    ToolTip.visible: hovered
    ToolTip.text: accessibleLabel
    ToolTip.delay: 500

    contentItem: Item {
        clip: true

        Image {
            anchors.fill: parent
            anchors.margins: control.hostWindow.dp(7)
            source: control.iconSource
            sourceSize.width: 64
            sourceSize.height: 64
            fillMode: Image.PreserveAspectFit
            smooth: true
            mipmap: true
            opacity: control.enabled ? 1.0 : 0.45
        }
    }

    background: Rectangle {
        radius: control.colors.cornerRadiusSmall
        color: control.checked
               ? control.colors.accentSoft
               : control.down
                 ? control.colors.buttonPressed
                 : control.hovered
                   ? control.colors.buttonHover
                   : "transparent"
        border.width: control.checked || control.visualFocus ? 1 : 0
        border.color: control.visualFocus
                      ? control.colors.focusRing
                      : control.colors.accent
    }
}
