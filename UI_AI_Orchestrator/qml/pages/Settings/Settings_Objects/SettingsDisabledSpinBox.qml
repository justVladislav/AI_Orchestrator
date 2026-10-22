import QtQuick
import QtQuick.Layouts

// Presentational stand-in for a future real SpinBox. Always disabled — no
// backend contract exists yet to drive or persist a numeric value.
Item {
    id: control

    required property var hostWindow
    required property var colors
    property string displayText: qsTr("Not connected yet")

    implicitWidth: control.hostWindow.dp(150)
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
        anchors.fill: parent
        anchors.margins: control.hostWindow.dp(4)
        spacing: 0

        Rectangle {
            Layout.preferredWidth: control.hostWindow.dp(22)
            Layout.fillHeight: true
            color: "transparent"

            Text {
                anchors.centerIn: parent
                text: "−"
                color: control.colors.textDisabled
                font.pixelSize: control.hostWindow.dp(13)
            }
        }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: control.displayText
            color: control.colors.textDisabled
            font.pixelSize: control.hostWindow.dp(12)
            elide: Text.ElideRight
        }
        Rectangle {
            Layout.preferredWidth: control.hostWindow.dp(22)
            Layout.fillHeight: true
            color: "transparent"

            Text {
                anchors.centerIn: parent
                text: "+"
                color: control.colors.textDisabled
                font.pixelSize: control.hostWindow.dp(13)
            }
        }
    }
}

