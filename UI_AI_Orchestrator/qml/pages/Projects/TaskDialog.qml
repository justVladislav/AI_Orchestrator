import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Projects_Objets"

Dialog {
    id: dialog
    required property var hostWindow
    required property var colors
    property string projectId: ""
    property var taskData: null
    signal acceptedTask(string taskId, string projectId, string title, string description,
                        string status, string priority, bool requiresAttention,
                        var dueAt, var plannedRunAt)

    width: Math.min(hostWindow.dp(620), hostWindow.width - hostWindow.dp(48))
    anchors.centerIn: parent
    modal: true
    title: taskData ? qsTr("Edit task") : qsTr("New task")
    standardButtons: Dialog.NoButton

    function openFor(project, task) {
        projectId = project
        taskData = task || null
        titleField.text = task ? task.title : ""
        descriptionField.text = task ? task.description : ""
        statusBox.currentIndex = task ? statusBox.indexOfValue(task.status) : 0
        priorityBox.currentIndex = task ? priorityBox.indexOfValue(task.priority) : 1
        attentionCheck.checked = task ? task.requiresAttention : false
        duePicker.selectedDate     = (task && task.dueAt)        ? task.dueAt        : new Date(NaN)
        plannedPicker.selectedDate = (task && task.plannedRunAt) ? task.plannedRunAt : new Date(NaN)
        open()
    }

    background: Rectangle {
        radius: dialog.colors.cornerRadiusMedium
        color: dialog.colors.popupBackground
        border.width: 1
        border.color: dialog.colors.border
    }
    contentItem: ColumnLayout {
        spacing: dialog.hostWindow.dp(12)
        TextField {
            id: titleField
            Layout.fillWidth: true
            placeholderText: qsTr("Task title")
            color: dialog.colors.textPrimary
            placeholderTextColor: dialog.colors.textMuted
            font.pixelSize: dialog.hostWindow.dp(12)
            background: Rectangle { radius: dialog.colors.cornerRadiusSmall; color: dialog.colors.inputBackground; border.width: 1; border.color: titleField.activeFocus ? dialog.colors.inputBorderFocused : dialog.colors.inputBorder }
        }
        TextArea {
            id: descriptionField
            Layout.fillWidth: true
            Layout.preferredHeight: dialog.hostWindow.dp(105)
            placeholderText: qsTr("Describe the expected result and task-specific context")
            color: dialog.colors.textPrimary
            placeholderTextColor: dialog.colors.textMuted
            wrapMode: TextEdit.Wrap
            font.pixelSize: dialog.hostWindow.dp(11)
            background: Rectangle { radius: dialog.colors.cornerRadiusSmall; color: dialog.colors.inputBackground; border.width: 1; border.color: descriptionField.activeFocus ? dialog.colors.inputBorderFocused : dialog.colors.inputBorder }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: dialog.hostWindow.dp(10)
            ProjectComboBox {
                id: statusBox
                Layout.fillWidth: true
                hostWindow: dialog.hostWindow; colors: dialog.colors
                textRole: "label"; valueRole: "value"
                model: [{label: qsTr("Backlog"),value:"backlog"},{label:qsTr("Planned"),value:"planned"},{label:qsTr("In progress"),value:"in_progress"},{label:qsTr("Blocked"),value:"blocked"},{label:qsTr("Done"),value:"done"}]
                function indexOfValue(value) { for (let i=0;i<model.length;++i) if (model[i].value===value) return i; return 0 }
            }
            ProjectComboBox {
                id: priorityBox
                Layout.fillWidth: true
                hostWindow: dialog.hostWindow; colors: dialog.colors
                textRole: "label"; valueRole: "value"
                model: [{label:qsTr("Low"),value:"low"},{label:qsTr("Normal"),value:"normal"},{label:qsTr("High"),value:"high"},{label:qsTr("Critical"),value:"critical"}]
                function indexOfValue(value) { for (let i=0;i<model.length;++i) if (model[i].value===value) return i; return 1 }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: dialog.hostWindow.dp(10)

            ColumnLayout {
                Layout.fillWidth: true
                spacing: dialog.hostWindow.dp(3)
                Text {
                    text: qsTr("Due date")
                    color: dialog.colors.textMuted
                    font.pixelSize: dialog.hostWindow.dp(9.5)
                }
                ProjectDatePicker {
                    id: duePicker
                    Layout.fillWidth: true
                    hostWindow: dialog.hostWindow
                    colors: dialog.colors
                    placeholderText: qsTr("Not set")
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: dialog.hostWindow.dp(3)
                Text {
                    text: qsTr("Planned run")
                    color: dialog.colors.textMuted
                    font.pixelSize: dialog.hostWindow.dp(9.5)
                }
                ProjectDatePicker {
                    id: plannedPicker
                    Layout.fillWidth: true
                    showTime: true
                    hostWindow: dialog.hostWindow
                    colors: dialog.colors
                    placeholderText: qsTr("Not set")
                }
            }
        }
        CheckBox {
            id: attentionCheck
            text: qsTr("Needs attention")
            palette.text: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(11)
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("Planned date is saved now; automatic scheduled execution will be added separately.")
            color: dialog.colors.textMuted
            wrapMode: Text.WordWrap
            font.pixelSize: dialog.hostWindow.dp(9)
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            ProjectActionButton { text: qsTr("Cancel"); hostWindow: dialog.hostWindow; colors: dialog.colors; onClicked: dialog.close() }
            ProjectActionButton {
                text: dialog.taskData ? qsTr("Save") : qsTr("Add task")
                primary: true; enabled: titleField.text.trim().length > 0
                hostWindow: dialog.hostWindow; colors: dialog.colors
                onClicked: {
                    dialog.acceptedTask(dialog.taskData ? dialog.taskData.taskId : "", dialog.projectId,
                        titleField.text.trim(), descriptionField.text.trim(),
                        statusBox.model[statusBox.currentIndex].value,
                        priorityBox.model[priorityBox.currentIndex].value,
                        attentionCheck.checked, duePicker.selectedDate, plannedPicker.selectedDate)
                    dialog.close()
                }
            }
        }
    }
}
