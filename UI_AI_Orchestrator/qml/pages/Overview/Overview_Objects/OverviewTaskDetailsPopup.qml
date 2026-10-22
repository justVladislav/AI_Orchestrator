import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

Basic.Popup {
    id: popup
    required property var hostWindow
    required property var colors
    property var taskData: ({})

    modal: false
    focus: true
    closePolicy: Basic.Popup.CloseOnEscape | Basic.Popup.CloseOnPressOutside
    width: Math.min(hostWindow.dp(480), parent ? parent.width - hostWindow.dp(32) : hostWindow.dp(480))
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
            Text { Layout.fillWidth: true; text: popup.taskData.title || qsTr("Task"); color: colors.textPrimary; font.pixelSize: hostWindow.dp(15); font.weight: Font.DemiBold; wrapMode: Text.WordWrap }
            Basic.Button { text: qsTr("Close"); onClicked: popup.close() }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: colors.separator }
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: hostWindow.dp(14)
            rowSpacing: hostWindow.dp(6)
            Text { text: qsTr("Status"); color: colors.textMuted; font.pixelSize: hostWindow.dp(9) }
            Text { text: popup.taskData.status || qsTr("Unknown"); color: colors.textPrimary; font.pixelSize: hostWindow.dp(10) }
            Text { text: qsTr("Priority"); color: colors.textMuted; font.pixelSize: hostWindow.dp(9) }
            Text { text: popup.taskData.priority || qsTr("Normal"); color: colors.textPrimary; font.pixelSize: hostWindow.dp(10) }
            Text { text: qsTr("Due"); color: colors.textMuted; font.pixelSize: hostWindow.dp(9) }
            Text { text: popup.taskData.dueLabel || qsTr("Not scheduled"); color: colors.textPrimary; font.pixelSize: hostWindow.dp(10) }
        }
        Text {
            Layout.fillWidth: true
            text: popup.taskData.description || qsTr("No task description.")
            color: colors.textSecondary
            font.pixelSize: hostWindow.dp(10)
            wrapMode: Text.WordWrap
        }
    }
}
