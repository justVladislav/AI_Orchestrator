import QtQuick
import QtQuick.Layouts

Item {
    id: page

    required property var hostWindow
    required property var colors
    required property string pageTitle
    required property string pageDescription


    property string statusText: ""

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: page.colors.contentMargin
        spacing: page.hostWindow.dp(8)

        Text {
            Layout.fillWidth: true
            text: page.pageTitle
            color: page.colors.textPrimary
            font.pixelSize: page.hostWindow.dp(28)
            font.weight: Font.DemiBold
        }

        Text {
            Layout.fillWidth: true
            text: page.pageDescription
            color: page.colors.textSecondary
            font.pixelSize: page.hostWindow.dp(14)
            wrapMode: Text.WordWrap
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: page.hostWindow.dp(16)

            Text {
                anchors.centerIn: parent
                visible: page.statusText.length > 0
                text: page.statusText
                color: page.colors.textSecondary
                font.pixelSize: page.hostWindow.dp(16)
                font.weight: Font.Medium
            }
        }
    }
}
