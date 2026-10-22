import QtQuick
import QtQuick.Layouts

// Card-style section container. Children are placed inside the card's inner
// ColumnLayout via the default alias. Layout.* properties on children are
// managed by that inner ColumnLayout.
ColumnLayout {
    id: section

    required property var hostWindow
    required property var colors
    required property string sectionTitle

    default property alias contentData: innerLayout.data

    spacing: section.hostWindow.dp(6)

    Text {
        Layout.fillWidth: true
        text: section.sectionTitle.toUpperCase()
        color: section.colors.textMuted
        font.pixelSize: section.hostWindow.dp(11)
        font.weight: Font.DemiBold
        font.letterSpacing: 0.5
        topPadding: section.hostWindow.dp(4)
    }

    Rectangle {
        Layout.fillWidth: true
        // innerLayout drives height; rows supply their own top/bottom margins.
        implicitHeight: innerLayout.implicitHeight
        color: section.colors.surface
        border.color: section.colors.border
        border.width: 1
        radius: section.colors.cornerRadiusMedium
        clip: true

        ColumnLayout {
            id: innerLayout
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: section.hostWindow.dp(16)
            anchors.rightMargin: section.hostWindow.dp(16)
            spacing: 0
        }
    }
}

