pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic as Basic

Basic.ComboBox {
    id: control

    required property var hostWindow
    required property var colors
    required property var projectsModel
    property string currentProjectId: ""
    signal projectSelected(string projectId)

    model: projectsModel
    textRole: "projectName"
    valueRole: "projectId"
    hoverEnabled: true
    leftPadding: hostWindow.dp(10)
    rightPadding: hostWindow.dp(28)
    font.pixelSize: hostWindow.dp(12)

    function syncSelection() {
        for (let i = 0; i < projectsModel.count; ++i) {
            if (projectsModel.get(i).projectId === currentProjectId) {
                currentIndex = i
                return
            }
        }
        currentIndex = 0
    }

    function currentProjectName() {
        if (!projectsModel || currentIndex < 0 || currentIndex >= projectsModel.count)
            return qsTr("Select project")

        const project = projectsModel.get(currentIndex)
        return project ? project.projectName : qsTr("Select project")
    }

    Component.onCompleted: syncSelection()
    onCurrentProjectIdChanged: syncSelection()
    onActivated: index => projectSelected(projectsModel.get(index).projectId)

    delegate: Basic.ItemDelegate {
        id: projectDelegate
        required property int index
        required property string projectId
        required property string projectName
        width: control.width
        height: control.hostWindow.dp(38)
        text: projectName
        highlighted: control.highlightedIndex === index
        font.pixelSize: control.hostWindow.dp(12)
        contentItem: Text {
            text: projectDelegate.text
            color: control.colors.textPrimary
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: control.colors.cornerRadiusSmall
            color: projectDelegate.highlighted || projectDelegate.hovered ? control.colors.buttonHover : "transparent"
        }
    }

    contentItem: Text {
        leftPadding: 0
        rightPadding: 0
        text: control.currentProjectName()
        color: control.colors.textPrimary
        font: control.font
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        radius: control.colors.cornerRadiusSmall
        color: control.colors.inputBackground
        border.width: control.activeFocus || control.down ? 1 : 0
        border.color: control.colors.inputBorderFocused
    }

    popup: Basic.Popup {
        y: control.height + control.hostWindow.dp(4)
        width: control.width
        implicitHeight: Math.min(contentItem.implicitHeight + topPadding + bottomPadding, control.hostWindow.dp(260))
        padding: control.hostWindow.dp(4)
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
        }
        background: Rectangle {
            radius: control.colors.cornerRadiusSmall
            color: control.colors.popupBackground
            border.width: 1
            border.color: control.colors.border
        }
    }
}
