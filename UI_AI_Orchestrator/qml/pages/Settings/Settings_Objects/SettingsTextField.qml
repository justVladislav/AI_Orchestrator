import QtQuick
import QtQuick.Controls.Basic as Basic

// A single-line text input in the look of the other Settings controls. `committed` is emitted when the user presses Enter; the page
// decides what to do with the text (the Backend validates it) - this control never filters or rewrites what was typed.
Basic.TextField {
    id: control

    required property var hostWindow
    required property var colors
    property string accessibleLabel: ""

    signal committed(string text)

    implicitHeight: hostWindow.dp(34)
    leftPadding: hostWindow.dp(10)
    rightPadding: hostWindow.dp(10)
    font.pixelSize: hostWindow.dp(12)
    color: colors.textPrimary
    placeholderTextColor: colors.textDisabled
    selectionColor: colors.selection
    selectedTextColor: colors.textOnAccent
    selectByMouse: true
    verticalAlignment: TextInput.AlignVCenter
    Accessible.name: accessibleLabel
    onAccepted: committed(text)

    background: Rectangle {
        radius: control.colors.cornerRadiusSmall
        color: control.colors.inputBackground
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus
                      ? control.colors.inputBorderFocused : control.colors.inputBorder
    }
}
