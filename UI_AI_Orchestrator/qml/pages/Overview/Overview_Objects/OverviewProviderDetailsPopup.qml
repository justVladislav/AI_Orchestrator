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
                height: Math.max(hostWindow.dp(72), modelLayout.implicitHeight + 2 * hostWindow.dp(10))
                radius: colors.cornerRadiusSmall
                color: colors.surfaceAlternative
                border.width: 1
                border.color: colors.border
                ColumnLayout {
                    id: modelLayout
                    anchors.fill: parent
                    anchors.margins: hostWindow.dp(10)
                    spacing: hostWindow.dp(4)
                    RowLayout {
                        Layout.fillWidth: true
                        Text { objectName: "modelNameLabel"; Layout.fillWidth: true; text: modelRow.modelData.name; color: colors.textPrimary; font.pixelSize: hostWindow.dp(11); font.weight: Font.Medium; elide: Text.ElideRight }
                        Text {
                            id: modelCostLabel
                            objectName: "modelCostLabel"
                            // A long incomplete-estimate sentence wraps (up to two lines) inside 60% of the row instead of
                            // squeezing the model name to nothing or leaving the row.
                            Layout.maximumWidth: modelRow.width * 0.6
                            // advanceWidth, not the bounding width: the text needs its full advance, or an exact amount loses its last digit.
                            Layout.preferredWidth: Math.min(Math.ceil(modelCostMetrics.advanceWidth) + 2, Layout.maximumWidth)
                            text: modelRow.modelData.costLabel
                            color: modelRow.modelData.priceIncomplete ? colors.warning : colors.textPrimary
                            horizontalAlignment: Text.AlignRight
                            wrapMode: Text.WordWrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                            font.pixelSize: hostWindow.dp(10)
                            font.weight: Font.DemiBold
                            font.italic: modelRow.modelData.priceIncomplete
                            TextMetrics { id: modelCostMetrics; font: modelCostLabel.font; text: modelCostLabel.text }
                        }
                    }
                    // Ledger executions and memory compactions stay apart; the latency belongs to the executions only.
                    Text {
                        objectName: "modelStatsLabel"
                        Layout.fillWidth: true
                        text: {
                            const data = modelRow.modelData
                            let line = qsTr("%1 executions").arg(data.executionCount)
                            if (Number(data.compactionCount || 0) > 0)
                                line += "  ·  " + qsTr("%1 compactions").arg(data.compactionCount)
                            line += "  ·  " + qsTr("%1 in / %2 out").arg(data.inputLabel).arg(data.outputLabel)
                            if (Number(data.executionCount) > 0)
                                line += "  ·  " + qsTr("%1 ms avg").arg(data.averageLatencyMs)
                            return line
                        }
                        color: colors.textSecondary
                        font.pixelSize: hostWindow.dp(9)
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
        Text {
            Layout.fillWidth: true
            visible: !popup.providerData.models || popup.providerData.models.length === 0
            text: qsTr("No executions or memory compactions recorded for this provider.")
            color: colors.textMuted
            horizontalAlignment: Text.AlignHCenter
        }
    }
}
