import QtQuick
import QtQuick.Controls.Basic as Basic

Basic.SpinBox {
    id: control

    required property var hostWindow
    required property var colors
    property string suffix: ""
    property string accessibleLabel: ""

    signal valueSelected(int selectedValue)

    implicitWidth: hostWindow.dp(150)
    implicitHeight: hostWindow.dp(34)
    editable: true
    font.pixelSize: hostWindow.dp(12)
    Accessible.name: accessibleLabel
    textFromValue: function(number, locale) {
        return Number(number).toLocaleString(locale, 'f', 0) + control.suffix
    }
    valueFromText: function(text, locale) {
        const normalized = String(text).replace(control.suffix, "").trim()
        return Number.fromLocaleString(locale, normalized)
    }
    onValueModified: valueSelected(value)

    contentItem: TextInput {
        z: 2
        text: control.displayText
        color: control.colors.textPrimary
        selectionColor: control.colors.selection
        selectedTextColor: control.colors.textOnAccent
        horizontalAlignment: Qt.AlignHCenter
        verticalAlignment: Qt.AlignVCenter
        readOnly: !control.editable
        validator: control.validator
        inputMethodHints: Qt.ImhFormattedNumbersOnly
        font: control.font
    }

    up.indicator: Rectangle {
        x: control.mirrored ? 0 : parent.width - width
        implicitWidth: control.hostWindow.dp(28)
        implicitHeight: parent.height / 2
        color: control.up.pressed ? control.colors.buttonPressed
              : control.up.hovered ? control.colors.buttonHover : "transparent"
        Text {
            anchors.centerIn: parent
            text: "+"
            color: control.colors.textSecondary
            font.pixelSize: control.hostWindow.dp(12)
        }
    }
    down.indicator: Rectangle {
        x: control.mirrored ? 0 : parent.width - width
        y: parent.height / 2
        implicitWidth: control.hostWindow.dp(28)
        implicitHeight: parent.height / 2
        color: control.down.pressed ? control.colors.buttonPressed
              : control.down.hovered ? control.colors.buttonHover : "transparent"
        Text {
            anchors.centerIn: parent
            text: "−"
            color: control.colors.textSecondary
            font.pixelSize: control.hostWindow.dp(12)
        }
    }
    background: Rectangle {
        radius: control.colors.cornerRadiusSmall
        color: control.colors.inputBackground
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus
                      ? control.colors.inputBorderFocused : control.colors.inputBorder
    }
}
