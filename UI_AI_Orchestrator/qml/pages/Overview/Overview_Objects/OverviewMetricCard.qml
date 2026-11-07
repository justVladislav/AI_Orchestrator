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
    // True when the value is an incomplete estimate or unavailable (Backend-reported incomplete data): the value
    // is drawn in the warning colour and italic, so an incomplete amount never reads like an exact one.
    property bool incomplete: false

    // The card is at least the usual height and grows with its text: an incomplete-estimate value wraps onto more lines
    // instead of being elided, so the recorded amount and the incompleteness mark stay readable at any width.
    implicitHeight: Math.max(hostWindow.dp(108), textColumn.implicitHeight + 2 * hostWindow.dp(14))
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
            id: textColumn
            Layout.fillWidth: true
            // A small preferred width: the column takes what the card gives it and a long wrapped value never widens the
            // card (or its grid column) beyond its siblings.
            Layout.preferredWidth: card.hostWindow.dp(60)
            spacing: card.hostWindow.dp(2)

            Text {
                Layout.fillWidth: true
                text: card.title
                color: card.colors.textSecondary
                font.pixelSize: card.hostWindow.dp(11)
                elide: Text.ElideRight
            }

            Text {
                objectName: "metricValue"
                Layout.fillWidth: true
                text: card.value
                color: card.incomplete ? card.colors.warning : card.colors.textPrimary
                // An incomplete value is a sentence ("Incomplete estimate: $0.25 recorded, total unknown"), not a single
                // figure: smaller type and wrapped over as many lines as it needs - never cut, the card grows instead.
                font.pixelSize: card.incomplete ? card.hostWindow.dp(14) : card.hostWindow.dp(21)
                font.weight: Font.DemiBold
                font.italic: card.incomplete
                wrapMode: card.incomplete ? Text.WordWrap : Text.NoWrap
                elide: card.incomplete ? Text.ElideNone : Text.ElideRight
            }

            Text {
                objectName: "metricDetail"
                Layout.fillWidth: true
                text: card.detail
                color: card.colors.textMuted
                font.pixelSize: card.hostWindow.dp(10)
                elide: Text.ElideRight
            }
        }
    }
}
