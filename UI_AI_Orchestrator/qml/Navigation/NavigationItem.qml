import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

AbstractButton {
    id: control

    required property var hostWindow
    required property var colors
    required property string sectionId
    required property string labelText
    // Text glyph fallback, shown only when the section has no PNG icon.
    property string iconText: ""
    // Navigation icons are named after PageHost section ids.
    property url iconSource: "qrc:/assets/navigation/" + sectionId + ".png"
    property bool selected: false

    height: hostWindow.dp(44)
    hoverEnabled: true

    Accessible.name: labelText

    contentItem: RowLayout {
        anchors.fill: parent
        anchors.leftMargin: control.hostWindow.dp(13)
        anchors.rightMargin: control.hostWindow.dp(13)
        spacing: control.hostWindow.dp(12)

        Item {
            Layout.preferredWidth: control.hostWindow.dp(28)
            Layout.fillHeight: true

            Image {
                id: iconImage
                anchors.centerIn: parent
                width: control.hostWindow.dp(25)
                height: control.hostWindow.dp(25)
                source: control.iconSource
                // Scale the cached texture rather than reload it on every resize.
                sourceSize: Qt.size(256, 256)
                fillMode: Image.PreserveAspectFit
                smooth: true
                mipmap: true
                opacity: control.selected ? 1.0 : 0.72
            }

            Text {
                anchors.centerIn: parent
                visible: iconImage.status !== Image.Ready
                text: control.iconText
                color: control.selected
                       ? control.colors.accent
                       : control.colors.textSecondary
                font.pixelSize: control.hostWindow.dp(19)
                font.weight: Font.DemiBold
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.fillHeight: true
            text: control.labelText
            color: control.selected
                   ? control.colors.textPrimary
                   : control.enabled
                     ? control.colors.textSecondary
                     : control.colors.textDisabled
            font.pixelSize: control.hostWindow.dp(14)
            font.weight: control.selected ? Font.DemiBold : Font.Medium
            verticalAlignment: Text.AlignVCenter
        }
    }

    background: Rectangle {
        radius: control.colors.cornerRadiusSmall
        color: control.selected
               ? control.colors.accentSoft
               : control.down
                 ? control.colors.buttonPressed
                 : control.hovered
                   ? control.colors.buttonHover
                   : "transparent"
        border.width: control.visualFocus ? 1 : 0
        border.color: control.colors.focusRing

        Rectangle {
            visible: control.selected
            anchors.left: parent.left
            anchors.leftMargin: control.hostWindow.dp(3)
            anchors.verticalCenter: parent.verticalCenter
            width: control.hostWindow.dp(3)
            height: control.hostWindow.dp(22)
            radius: width / 2
            color: control.colors.accent
        }
    }
}
