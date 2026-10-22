import QtQuick
import QtQuick.Layouts

// Presentational stand-in for a future real ComboBox. Always disabled — no
// backend contract exists yet to drive selection or persist a chosen value.
Item {
    id: control

    required property var hostWindow
    required property var colors
    property string displayText: qsTr("Not connected yet")

    implicitWidth: Math.max(control.hostWindow.dp(150), rowContent.implicitWidth + control.hostWindow.dp(24))
    implicitHeight: control.hostWindow.dp(30)
    opacity: 0.55

    Rectangle {
        anchors.fill: parent
        radius: control.colors.cornerRadiusSmall
        color: control.colors.inputBackground
        border.color: control.colors.inputBorder
        border.width: 1
    }

    RowLayout {
        id: rowContent
        anchors.fill: parent
        anchors.leftMargin: control.hostWindow.dp(10)
        anchors.rightMargin: control.hostWindow.dp(8)
        spacing: control.hostWindow.dp(6)

        Text {
            Layout.fillWidth: true
            text: control.displayText
            color: control.colors.textDisabled
            font.pixelSize: control.hostWindow.dp(12)
            elide: Text.ElideRight
        }
        Text {
            text: "▾"
            color: control.colors.textDisabled
            font.pixelSize: control.hostWindow.dp(10)
        }
    }
}
