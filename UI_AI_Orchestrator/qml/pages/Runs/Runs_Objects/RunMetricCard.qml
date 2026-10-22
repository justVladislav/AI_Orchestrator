import QtQuick
import QtQuick.Layouts

Rectangle {
    id: card
    required property var hostWindow
    required property var colors
    required property string title
    required property string value
    required property string detail
    required property color accentColor
    implicitHeight: hostWindow.dp(88)
    radius: colors.cornerRadiusMedium
    color: colors.surfaceAlternative
    border.width: 1
    border.color: colors.border
    RowLayout {
        anchors.fill: parent
        anchors.margins: card.hostWindow.dp(14)
        spacing: card.hostWindow.dp(10)
        Rectangle { Layout.preferredWidth: card.hostWindow.dp(4); Layout.fillHeight: true; radius: width / 2; color: card.accentColor }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: card.hostWindow.dp(2)
            Text { text: card.title; color: card.colors.textMuted; font.pixelSize: card.hostWindow.dp(9.5) }
            Text { text: card.value; color: card.colors.textPrimary; font.pixelSize: card.hostWindow.dp(19); font.weight: Font.DemiBold }
            Text { Layout.fillWidth: true; text: card.detail; color: card.colors.textSecondary; font.pixelSize: card.hostWindow.dp(9); elide: Text.ElideRight }
        }
    }
}
