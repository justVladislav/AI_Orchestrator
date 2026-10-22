import QtQuick

// Reusable action button. `actionEnabled: false` (the default) renders a
// dimmed, non-interactive control for actions with no backend contract yet.
// Named "actionEnabled" rather than "enabled" to avoid shadowing the base
// QQuickItem.enabled property. `danger: true` applies the destructive-action
// color treatment.
Item {
    id: control

    required property var hostWindow
    required property var colors
    property string text: ""
    property bool actionEnabled: false
    property bool danger: false

    signal clicked()

    implicitWidth: label.implicitWidth + control.hostWindow.dp(24)
    implicitHeight: control.hostWindow.dp(32)
    opacity: control.actionEnabled ? 1.0 : 0.5

    Rectangle {
        anchors.fill: parent
        radius: control.colors.cornerRadiusSmall
        color: control.danger
               ? (area.pressed
                  ? Qt.rgba(control.colors.error.r, control.colors.error.g, control.colors.error.b, 0.28)
                  : area.containsMouse
                    ? Qt.rgba(control.colors.error.r, control.colors.error.g, control.colors.error.b, 0.16)
                    : control.colors.errorSoft)
               : (area.pressed
                  ? control.colors.buttonPressed
                  : area.containsMouse
                    ? control.colors.buttonHover
                    : control.colors.buttonBackground)
    }

    Text {
        id: label
        anchors.centerIn: parent
        text: control.text
        color: control.danger ? control.colors.error : control.colors.textPrimary
        font.pixelSize: control.hostWindow.dp(13)
        font.weight: control.danger ? Font.Medium : Font.Normal
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: control.actionEnabled
        enabled: control.actionEnabled
        cursorShape: control.actionEnabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: if (control.actionEnabled) control.clicked()
    }
}
