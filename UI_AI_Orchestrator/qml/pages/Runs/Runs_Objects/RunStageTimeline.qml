pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: timeline
    required property var hostWindow
    required property var colors
    required property var stagesModel
    spacing: hostWindow.dp(7)

    function stateColor(state) {
        if (state === "completed") return colors.success
        if (state === "running") return colors.information
        if (state === "failed") return colors.error
        if (state === "cancelled") return colors.warning
        return colors.textMuted
    }

    Repeater {
        model: timeline.stagesModel
        delegate: RowLayout {
            id: stageItem
            required property var modelData
            Layout.fillWidth: true
            Layout.preferredHeight: timeline.hostWindow.dp(42)
            spacing: timeline.hostWindow.dp(9)
            Rectangle {
                Layout.preferredWidth: timeline.hostWindow.dp(20)
                Layout.preferredHeight: timeline.hostWindow.dp(20)
                radius: width / 2
                color: stageItem.modelData.state === "pending" ? timeline.colors.buttonBackground : timeline.stateColor(stageItem.modelData.state)
                border.width: stageItem.modelData.state === "pending" ? 1 : 0
                border.color: timeline.colors.borderStrong
                Text {
                    anchors.centerIn: parent
                    text: stageItem.modelData.state === "completed" ? "✓"
                          : stageItem.modelData.state === "running" ? "•"
                          : stageItem.modelData.state === "failed" ? "!"
                          : stageItem.modelData.state === "cancelled" ? "×" : ""
                    color: timeline.colors.textOnAccent
                    font.pixelSize: timeline.hostWindow.dp(10)
                    font.weight: Font.Bold
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: timeline.hostWindow.dp(2)
                Text { Layout.fillWidth: true; text: stageItem.modelData.name; color: timeline.colors.textPrimary; font.pixelSize: timeline.hostWindow.dp(10); font.weight: stageItem.modelData.state === "running" ? Font.DemiBold : Font.Medium; elide: Text.ElideRight }
                Text { Layout.fillWidth: true; text: stageItem.modelData.detail; color: timeline.colors.textMuted; font.pixelSize: timeline.hostWindow.dp(8.5); elide: Text.ElideRight }
            }
            Text { text: stageItem.modelData.duration; color: timeline.stateColor(stageItem.modelData.state); font.pixelSize: timeline.hostWindow.dp(8.5) }
        }
    }
}
