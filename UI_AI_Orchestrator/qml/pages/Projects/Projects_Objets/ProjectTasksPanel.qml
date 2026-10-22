pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: panel
    required property var hostWindow
    required property var colors
    required property var tasksModel
    signal addRequested()
    signal editRequested(var task)
    signal cancelRequested(string taskId)
    Layout.fillWidth: true
    spacing: hostWindow.dp(8)

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: panel.hostWindow.dp(44)
        radius: panel.colors.cornerRadiusSmall
        color: panel.colors.inputBackground
        border.width: 1
        border.color: panel.colors.border

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: panel.hostWindow.dp(11)
            anchors.rightMargin: panel.hostWindow.dp(7)
            spacing: panel.hostWindow.dp(8)

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Text { text: qsTr("Project tasks"); color: panel.colors.textPrimary; font.pixelSize: panel.hostWindow.dp(13); font.weight: Font.DemiBold }
                Text { text: qsTr("Planned work and attention items"); color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(8.5) }
            }
            Rectangle {
                Layout.preferredWidth: taskCount.implicitWidth + panel.hostWindow.dp(12)
                Layout.preferredHeight: panel.hostWindow.dp(22)
                radius: height / 2
                color: panel.colors.surface
                border.width: 1
                border.color: panel.colors.border
                Text { id: taskCount; anchors.centerIn: parent; text: String(panel.tasksModel.length); color: panel.colors.textSecondary; font.pixelSize: panel.hostWindow.dp(9.5); font.weight: Font.DemiBold }
            }
            ProjectActionButton { text: qsTr("Add task"); iconText: "+"; primary: true; hostWindow: panel.hostWindow; colors: panel.colors; onClicked: panel.addRequested() }
        }
    }
    Text {
        Layout.fillWidth: true
        visible: panel.tasksModel.length === 0
        text: qsTr("No tasks yet. Add concrete work without changing the shared project context.")
        color: panel.colors.textMuted; wrapMode: Text.WordWrap; font.pixelSize: panel.hostWindow.dp(10)
    }
    Repeater {
        model: panel.tasksModel
        delegate: Rectangle {
            id: row
            required property var modelData
            Layout.fillWidth: true
            Layout.preferredHeight: content.implicitHeight + panel.hostWindow.dp(18)
            radius: panel.colors.cornerRadiusSmall
            color: panel.colors.inputBackground
            border.width: 1
            border.color: row.modelData.requiresAttention || row.modelData.status === "blocked" ? panel.colors.warning : panel.colors.border
            RowLayout {
                id: content
                anchors.fill: parent; anchors.margins: panel.hostWindow.dp(9); spacing: panel.hostWindow.dp(9)
                Rectangle { Layout.preferredWidth: panel.hostWindow.dp(4); Layout.fillHeight: true; radius: width/2; color: row.modelData.status === "done" ? panel.colors.success : row.modelData.status === "blocked" ? panel.colors.error : panel.colors.accent }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: panel.hostWindow.dp(3)
                    RowLayout {
                        Layout.fillWidth: true
                        Text { Layout.fillWidth: true; text: row.modelData.title; color: panel.colors.textPrimary; elide: Text.ElideRight; font.pixelSize: panel.hostWindow.dp(11); font.weight: Font.Medium }
                        Text { text: row.modelData.statusLabel; color: row.modelData.status === "blocked" ? panel.colors.error : panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(9) }
                    }
                    Text { Layout.fillWidth: true; visible: row.modelData.description.length > 0; text: row.modelData.description; color: panel.colors.textSecondary; elide: Text.ElideRight; font.pixelSize: panel.hostWindow.dp(9.5) }
                    Text { Layout.fillWidth: true; text: row.modelData.metaLabel; color: panel.colors.textMuted; elide: Text.ElideRight; font.pixelSize: panel.hostWindow.dp(8.5) }
                }
                ProjectActionButton { text: qsTr("Edit"); hostWindow: panel.hostWindow; colors: panel.colors; onClicked: panel.editRequested(row.modelData) }
                ProjectActionButton { text: qsTr("Cancel"); danger: true; visible: row.modelData.status !== "done" && row.modelData.status !== "cancelled"; hostWindow: panel.hostWindow; colors: panel.colors; onClicked: panel.cancelRequested(row.modelData.taskId) }
            }
        }
    }
}
