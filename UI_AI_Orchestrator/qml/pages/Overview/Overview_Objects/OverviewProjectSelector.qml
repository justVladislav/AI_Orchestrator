pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic

Basic.ComboBox {
    id: control

    required property var hostWindow
    required property var colors
    required property var projectsModel

    signal projectSelected(int index)

    model: projectsModel
    textRole: "name"
    implicitHeight: hostWindow.dp(40)
    leftPadding: hostWindow.dp(12)
    rightPadding: hostWindow.dp(34)
    font.pixelSize: hostWindow.dp(12)
    Accessible.name: qsTr("Dashboard project")
    onActivated: index => projectSelected(index)

    contentItem: Text {
        leftPadding: 0
        rightPadding: 0
        text: control.currentIndex >= 0 && control.currentIndex < control.projectsModel.length
              ? control.projectsModel[control.currentIndex].name : qsTr("Select project")
        color: control.colors.textPrimary
        font: control.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    indicator: Text {
        x: control.width - width - control.hostWindow.dp(12)
        anchors.verticalCenter: parent.verticalCenter
        text: "⌄"
        color: control.colors.textSecondary
        font.pixelSize: control.hostWindow.dp(15)
    }
    background: Rectangle {
        radius: control.colors.cornerRadiusSmall
        color: control.colors.inputBackground
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? control.colors.inputBorderFocused : control.colors.inputBorder
    }
    delegate: Basic.ItemDelegate {
        id: optionItem
        required property int index
        width: control.width
        text: control.projectsModel[index].name
        highlighted: control.highlightedIndex === index
        font.pixelSize: control.hostWindow.dp(12)
        contentItem: Text {
            text: optionItem.text
            color: control.colors.textPrimary
            font: optionItem.font
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle { color: optionItem.highlighted ? control.colors.accentSoft : control.colors.popupBackground }
    }
    popup: Basic.Popup {
        y: control.height + control.hostWindow.dp(4)
        width: control.width
        implicitHeight: Math.min(contentItem.implicitHeight + control.hostWindow.dp(8), control.hostWindow.dp(260))
        padding: control.hostWindow.dp(4)
        contentItem: ListView {
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            clip: true
            ScrollIndicator.vertical: ScrollIndicator { }
        }
        background: Rectangle {
            radius: control.colors.cornerRadiusSmall
            color: control.colors.popupBackground
            border.width: 1
            border.color: control.colors.border
        }
    }
}
