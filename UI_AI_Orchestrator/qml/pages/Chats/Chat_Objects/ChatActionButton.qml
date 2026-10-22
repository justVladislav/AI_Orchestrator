import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

// Text action button used across the Chats page (New chat, Send, Stop,
// Clear context, dialog confirm/cancel...). Local to Chats so that the
// danger variant can be built entirely from AppTheme tokens (colors.error /
// colors.errorSoft) instead of hardcoded hex values.
Basic.Button {
    id: control

    required property var hostWindow
    required property var colors

    property string iconGlyph: ""
    property bool primary: false
    property bool danger: false

    implicitHeight: hostWindow.dp(36)
    implicitWidth: contentRow.implicitWidth + hostWindow.dp(22)
    leftPadding: hostWindow.dp(12)
    rightPadding: hostWindow.dp(12)
    hoverEnabled: true
    opacity: enabled ? 1.0 : 0.55

    Accessible.name: control.text

    contentItem: RowLayout {
        id: contentRow
        spacing: control.hostWindow.dp(6)

        Text {
            visible: control.iconGlyph.length > 0
            text: control.iconGlyph
            color: contentRow.textColor
            font.pixelSize: control.hostWindow.dp(13)
        }

        Text {
            text: control.text
            color: contentRow.textColor
            font.pixelSize: control.hostWindow.dp(12.5)
            font.weight: Font.Medium
        }

        readonly property color textColor: control.primary
                                           ? control.colors.textOnAccent
                                           : control.danger
                                             ? (control.hovered || control.down
                                                ? control.colors.textOnAccent
                                                : control.colors.error)
                                             : control.colors.textPrimary
    }

    background: Rectangle {
        radius: control.colors.cornerRadiusSmall
        color: control.primary
               ? (control.down ? control.colors.accentPressed
                               : control.hovered ? control.colors.accentHover
                                                 : control.colors.accent)
               : control.danger
                 ? (control.down || control.hovered
                    ? control.colors.error
                    : control.colors.errorSoft)
                 : control.down
                   ? control.colors.buttonPressed
                   : control.hovered
                     ? control.colors.buttonHover
                     : control.colors.buttonBackground
        border.width: control.primary
                      ? 0
                      : control.danger
                        ? (control.hovered || control.down ? 0 : 1)
                        : 1
        border.color: control.danger ? control.colors.error : control.colors.border
    }
}
