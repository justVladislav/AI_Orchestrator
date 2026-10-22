import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Button {
    id: control

    required property var hostWindow
    required property var colors

    property string iconText: ""
    property bool primary: false
    property bool danger: false

    implicitHeight: hostWindow.dp(38)
    implicitWidth: contentRow.implicitWidth + hostWindow.dp(24)
    leftPadding: hostWindow.dp(12)
    rightPadding: hostWindow.dp(12)
    hoverEnabled: true

    contentItem: RowLayout {
        id: contentRow
        spacing: control.hostWindow.dp(7)

        Text {
            visible: control.iconText.length > 0
            text: control.iconText
            color: control.primary || control.danger
                   ? control.colors.textOnAccent
                   : control.colors.textPrimary
            font.pixelSize: control.hostWindow.dp(17)
            font.weight: Font.Medium
        }

        Text {
            text: control.text
            color: control.primary || control.danger
                   ? control.colors.textOnAccent
                   : control.colors.textPrimary
            font.pixelSize: control.hostWindow.dp(13)
            font.weight: Font.Medium
        }
    }

    background: Rectangle {
        radius: control.colors.cornerRadiusSmall
        color: control.danger
               ? (control.down ? "#B91C1C" : control.hovered ? "#DC4545" : "#C93C47")
               : control.primary
                 ? (control.down ? control.colors.accentPressed
                                 : control.hovered ? control.colors.accentHover
                                                   : control.colors.accent)
                 : control.down
                   ? control.colors.buttonPressed
                   : control.hovered
                     ? control.colors.buttonHover
                     : control.colors.buttonBackground
        border.width: !control.primary && !control.danger ? 1 : 0
        border.color: control.colors.border
    }
}
