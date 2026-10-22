import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

Basic.Button {
    id: control
    required property var hostWindow
    required property var colors
    property string iconText: ""
    property bool primary: false
    property bool danger: false
    implicitHeight: hostWindow.dp(38)
    implicitWidth: contentRow.implicitWidth + hostWindow.dp(24)
    opacity: enabled ? 1.0 : 0.5
    Accessible.name: text
    contentItem: RowLayout {
        id: contentRow
        spacing: control.hostWindow.dp(6)
        Text { visible: control.iconText.length > 0; text: control.iconText; color: contentRow.foreground; font.pixelSize: control.hostWindow.dp(13) }
        Text { text: control.text; color: contentRow.foreground; font.pixelSize: control.hostWindow.dp(11); font.weight: Font.Medium }
        readonly property color foreground: control.primary ? control.colors.textOnAccent
                                                              : control.danger ? control.colors.error
                                                                               : control.colors.textPrimary
    }
    background: Rectangle {
        radius: control.colors.cornerRadiusSmall
        color: control.primary ? (control.down ? control.colors.accentPressed : control.hovered ? control.colors.accentHover : control.colors.accent)
                               : control.danger ? control.colors.errorSoft
                                                : control.down ? control.colors.buttonPressed : control.hovered ? control.colors.buttonHover : control.colors.buttonBackground
        border.width: control.primary ? 0 : 1
        border.color: control.danger ? control.colors.error : control.colors.border
    }
}
