import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

AbstractButton {
    id: control

    required property var hostWindow
    required property var colors

    property string iconText: ""
    property bool primary: false
    property bool danger: false

    implicitHeight: hostWindow.dp(36)
    implicitWidth: contentRow.implicitWidth + hostWindow.dp(22)
    leftPadding: hostWindow.dp(11)
    rightPadding: hostWindow.dp(11)
    hoverEnabled: true

    contentItem: RowLayout {
        id: contentRow
        spacing: control.hostWindow.dp(6)

        Text {
            visible: control.iconText.length > 0
            text: control.iconText
            color: control.primary || control.danger
                   ? control.colors.textOnAccent
                   : control.enabled
                     ? control.colors.textPrimary
                     : control.colors.textDisabled
            font.pixelSize: control.hostWindow.dp(15)
            font.weight: Font.DemiBold
        }

        Text {
            text: control.text
            color: control.primary || control.danger
                   ? control.colors.textOnAccent
                   : control.enabled
                     ? control.colors.textPrimary
                     : control.colors.textDisabled
            font.pixelSize: control.hostWindow.dp(11)
            font.weight: Font.Medium
        }
    }

    background: Rectangle {
        radius: control.colors.cornerRadiusSmall
        opacity: control.enabled ? 1.0 : 0.55
        color: control.danger
               ? (control.down
                  ? Qt.darker(control.colors.error, 1.18)
                  : control.hovered
                    ? Qt.lighter(control.colors.error, 1.08)
                    : control.colors.error)
               : control.primary
                 ? (control.down
                    ? control.colors.accentPressed
                    : control.hovered
                      ? control.colors.accentHover
                      : control.colors.accent)
                 : control.down
                   ? control.colors.buttonPressed
                   : control.hovered
                     ? control.colors.buttonHover
                     : control.colors.buttonBackground
        border.width: !control.primary && !control.danger ? 1 : 0
        border.color: control.activeFocus
                      ? control.colors.focusRing
                      : control.colors.border
    }
}
