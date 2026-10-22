import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

Basic.Popup {
    id: popup
    required property var hostWindow
    required property var colors
    property var providerData: ({ name: "", models: [] })

    modal: false
    focus: true
    closePolicy: Basic.Popup.CloseOnEscape | Basic.Popup.CloseOnPressOutside
    width: Math.min(hostWindow.dp(560), parent ? parent.width - hostWindow.dp(32) : hostWindow.dp(560))
    height: Math.min(hostWindow.dp(430), parent ? parent.height - hostWindow.dp(32) : hostWindow.dp(430))
    padding: hostWindow.dp(18)

    background: Rectangle {
        radius: colors.cornerRadiusMedium
        color: colors.surfaceRaised
        border.width: 1
        border.color: colors.borderStrong
    }

    contentItem: ColumnLayout {
        spacing: hostWindow.dp(10)
        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                Layout.fillWidth: true
                spacing: hostWindow.dp(2)
                Text { text: popup.providerData.name; color: colors.textPrimary; font.pixelSize: hostWindow.dp(16); font.weight: Font.DemiBold }
                Text { text: qsTr("Usage by model"); color: colors.textMuted; font.pixelSize: hostWindow.dp(10) }
            }
            Basic.Button {
                text: qsTr("Close")
                onClicked: popup.close()
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: colors.separator }
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: hostWindow.dp(6)
            model: popup.providerData.models || []
            delegate: Rectangle {
                id: modelRow
                required property var modelData
                width: ListView.view.width
                height: hostWindow.dp(72)
                radius: colors.cornerRadiusSmall
                color: colors.surfaceAlternative
                border.width: 1
                border.color: colors.border
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: hostWindow.dp(10)
                    spacing: hostWindow.dp(4)
                    RowLayout {
                        Layout.fillWidth: true
                        Text { Layout.fillWidth: true; text: modelRow.modelData.name; color: colors.textPrimary; font.pixelSize: hostWindow.dp(11); font.weight: Font.Medium; elide: Text.ElideRight }
                        Text { objectName: "modelCostLabel"; text: modelRow.modelData.costLabel; color: colors.textPrimary; font.pixelSize: hostWindow.dp(10); font.weight: Font.DemiBold; font.italic: modelRow.modelData.priceIncomplete }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("%1 calls  ·  %2 in / %3 out  ·  %4 ms avg")
                              .arg(modelRow.modelData.executionCount)
                              .arg(modelRow.modelData.inputLabel)
                              .arg(modelRow.modelData.outputLabel)
                              .arg(modelRow.modelData.averageLatencyMs)
                        color: colors.textSecondary
                        font.pixelSize: hostWindow.dp(9)
                        elide: Text.ElideRight
                    }
                }
            }
        }
        Text {
            Layout.fillWidth: true
            visible: !popup.providerData.models || popup.providerData.models.length === 0
            text: qsTr("No model executions recorded for this provider.")
            color: colors.textMuted
            horizontalAlignment: Text.AlignHCenter
        }
    }
}
