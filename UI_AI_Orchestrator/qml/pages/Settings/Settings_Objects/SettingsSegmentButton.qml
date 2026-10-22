import QtQuick

Rectangle {
    id: control

    required property var hostWindow
    required property var colors
    required property string text
    property bool selected: false

    signal clicked()

    implicitWidth: label.implicitWidth + hostWindow.dp(18)
    implicitHeight: hostWindow.dp(30)
    radius: colors.cornerRadiusSmall
    color: selected ? colors.accentSoft
                    : mouse.containsMouse ? colors.buttonHover : colors.buttonBackground
    border.width: selected ? 2 : 1
    border.color: selected ? colors.accent : colors.border

    Text {
        id: label
        anchors.centerIn: parent
        text: control.text
        color: control.selected ? control.colors.accent : control.colors.textSecondary
        font.pixelSize: control.hostWindow.dp(12)
        font.weight: control.selected ? Font.DemiBold : Font.Normal
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: control.clicked()
    }
}
