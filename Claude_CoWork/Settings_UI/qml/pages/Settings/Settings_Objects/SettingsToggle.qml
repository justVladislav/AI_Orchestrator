import QtQuick
import QtQuick.Layouts

// A settings row with label+description on the left and a toggle on the
// right. The track/thumb is always visible so the control reads as a real,
// recognizable toggle. When connected is false, it renders at reduced
// opacity, is non-interactive (enabled: false), and an optional
// notConnectedLabel appears beside it — the toggle itself never disappears.
ColumnLayout {
    id: toggle

    required property var hostWindow
    required property var colors
    required property string label

    property string description: ""
    property bool checked: false
    property bool connected: false
    property bool isLast: false
    property string notConnectedLabel: qsTr("Not connected yet")

    signal toggled(bool value)

    spacing: 0

    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: toggle.hostWindow.dp(12)
        Layout.bottomMargin: toggle.hostWindow.dp(12)
        spacing: toggle.hostWindow.dp(12)

        ColumnLayout {
            Layout.fillWidth: true
            spacing: toggle.hostWindow.dp(2)

            Text {
                Layout.fillWidth: true
                text: toggle.label
                color: toggle.colors.textPrimary
                font.pixelSize: toggle.hostWindow.dp(13)
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                visible: toggle.description.length > 0
                text: toggle.description
                color: toggle.colors.textSecondary
                font.pixelSize: toggle.hostWindow.dp(12)
                wrapMode: Text.WordWrap
            }
        }

        Text {
            visible: !toggle.connected && toggle.notConnectedLabel.length > 0
            text: toggle.notConnectedLabel
            color: toggle.colors.textDisabled
            font.pixelSize: toggle.hostWindow.dp(11)
            font.italic: true
        }

        Rectangle {
            id: track
            enabled: toggle.connected
            opacity: toggle.connected ? 1.0 : 0.45
            implicitWidth: toggle.hostWindow.dp(40)
            implicitHeight: toggle.hostWindow.dp(22)
            radius: height / 2
            color: toggle.checked ? toggle.colors.accent : toggle.colors.border

            Behavior on color { ColorAnimation { duration: 150 } }

            Rectangle {
                // Thumb is anchored within track, not Layout-managed — plain
                // width/height is correct here.
                width: toggle.hostWindow.dp(16)
                height: width
                radius: width / 2
                color: "#FFFFFF"
                x: toggle.checked
                   ? parent.width - width - toggle.hostWindow.dp(3)
                   : toggle.hostWindow.dp(3)
                anchors.verticalCenter: parent.verticalCenter

                Behavior on x { NumberAnimation { duration: 150; easing.type: Easing.InOutQuad } }
            }

            MouseArea {
                anchors.fill: parent
                enabled: toggle.connected
                hoverEnabled: toggle.connected
                cursorShape: toggle.connected ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: if (toggle.connected) toggle.toggled(!toggle.checked)
            }
        }
    }

    Rectangle {
        Layout.fillWidth: true
        visible: !toggle.isLast
        implicitHeight: 1
        color: toggle.colors.separator
    }
}
