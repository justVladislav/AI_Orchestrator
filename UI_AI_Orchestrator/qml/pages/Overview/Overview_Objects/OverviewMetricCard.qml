import QtQuick
import QtQuick.Layouts

Rectangle {
    id: card

    required property var hostWindow
    required property var colors
    required property string iconText
    required property string title
    required property string value
    required property string detail
    required property color accentColor
    // True when the value is a lower bound or unavailable (Backend-reported incomplete data): the value
    // is drawn in the warning colour and italic, so an incomplete amount never reads like an exact one.
    property bool incomplete: false

    implicitHeight: hostWindow.dp(108)
    radius: colors.cornerRadiusMedium
    color: colors.surfaceAlternative
    border.width: 1
    border.color: colors.border

    RowLayout {
        anchors.fill: parent
        anchors.margins: card.hostWindow.dp(14)
        spacing: card.hostWindow.dp(12)

        Rectangle {
            Layout.preferredWidth: card.hostWindow.dp(38)
            Layout.preferredHeight: card.hostWindow.dp(38)
            Layout.alignment: Qt.AlignTop
            radius: card.colors.cornerRadiusSmall
            color: Qt.rgba(card.accentColor.r,
                           card.accentColor.g,
                           card.accentColor.b,
                           0.16)

            Text {
                anchors.centerIn: parent
                text: card.iconText
                color: card.accentColor
                font.pixelSize: card.hostWindow.dp(15)
                font.weight: Font.Bold
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: card.hostWindow.dp(2)

            Text {
                Layout.fillWidth: true
                text: card.title
                color: card.colors.textSecondary
                font.pixelSize: card.hostWindow.dp(11)
                elide: Text.ElideRight
            }

            Text {
                Layout.fillWidth: true
                text: card.value
                color: card.incomplete ? card.colors.warning : card.colors.textPrimary
                font.pixelSize: card.hostWindow.dp(21)
                font.weight: Font.DemiBold
                font.italic: card.incomplete
                elide: Text.ElideRight
            }

            Text {
                Layout.fillWidth: true
                text: card.detail
                color: card.colors.textMuted
                font.pixelSize: card.hostWindow.dp(10)
                elide: Text.ElideRight
            }
        }
    }
}
