import QtQuick
import QtQuick.Layouts

// A settings row: label+description on the left, a control slot on the right.
// Place one control item as a child; it goes into the right-side slot.
ColumnLayout {
    id: row

    required property var hostWindow
    required property var colors
    required property string label

    property string description: ""
    property bool isLast: false

    default property alias controlSlot: controlHolder.data

    spacing: 0

    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: row.hostWindow.dp(12)
        Layout.bottomMargin: row.hostWindow.dp(12)
        spacing: row.hostWindow.dp(12)

        ColumnLayout {
            Layout.fillWidth: true
            spacing: row.hostWindow.dp(2)

            Text {
                Layout.fillWidth: true
                text: row.label
                color: row.colors.textPrimary
                font.pixelSize: row.hostWindow.dp(13)
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                visible: row.description.length > 0
                text: row.description
                color: row.colors.textSecondary
                font.pixelSize: row.hostWindow.dp(12)
                wrapMode: Text.WordWrap
            }
        }

        Item {
            id: controlHolder
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: childrenRect.width
            implicitHeight: Math.max(childrenRect.height, 1)
        }
    }

    Rectangle {
        Layout.fillWidth: true
        visible: !row.isLast
        implicitHeight: 1
        color: row.colors.separator
    }
}
