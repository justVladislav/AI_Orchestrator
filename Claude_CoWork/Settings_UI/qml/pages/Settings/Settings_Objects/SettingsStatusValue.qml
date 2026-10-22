import QtQuick
import QtQuick.Layouts

// Reusable status indicator: a dot plus a text label. Used by Diagnostics
// rows. dotColor defaults to a neutral "unknown" tone — callers do not
// pass success/error tones today because no status contract exists yet.
RowLayout {
    id: status

    required property var hostWindow
    required property var colors
    property string text: qsTr("Not connected yet")
    property color dotColor: status.colors.textDisabled

    spacing: status.hostWindow.dp(6)

    Rectangle {
        Layout.preferredWidth: status.hostWindow.dp(7)
        Layout.preferredHeight: status.hostWindow.dp(7)
        Layout.alignment: Qt.AlignVCenter
        radius: width / 2
        color: status.dotColor
    }
    Text {
        Layout.alignment: Qt.AlignVCenter
        text: status.text
        color: status.colors.textDisabled
        font.pixelSize: status.hostWindow.dp(12)
    }
}
