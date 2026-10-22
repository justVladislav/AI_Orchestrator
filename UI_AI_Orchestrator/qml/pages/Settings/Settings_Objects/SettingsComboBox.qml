pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic as Basic

Basic.ComboBox {
    id: control

    required property var hostWindow
    required property var colors
    required property var options
    property string selectedValue: ""
    property string accessibleLabel: ""

    signal valueSelected(string value)

    model: options
    implicitWidth: hostWindow.dp(190)
    implicitHeight: hostWindow.dp(34)
    leftPadding: hostWindow.dp(10)
    rightPadding: hostWindow.dp(28)
    font.pixelSize: hostWindow.dp(12)
    Accessible.name: accessibleLabel

    function syncIndex() {
        for (let index = 0; index < options.length; ++index) {
            if (String(options[index].value) === selectedValue) {
                currentIndex = index
                return
            }
        }
        currentIndex = options.length > 0 ? 0 : -1
    }

    Component.onCompleted: syncIndex()
    onSelectedValueChanged: syncIndex()
    onOptionsChanged: syncIndex()
    onActivated: index => {
        if (index >= 0 && index < options.length)
            valueSelected(String(options[index].value))
    }

    contentItem: Text {
        leftPadding: 0
        rightPadding: 0
        text: control.currentIndex >= 0 && control.currentIndex < control.options.length
              ? control.options[control.currentIndex].label : ""
        color: control.colors.textPrimary
        font: control.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Text {
        x: control.width - width - control.hostWindow.dp(9)
        anchors.verticalCenter: parent.verticalCenter
        text: "⌄"
        color: control.colors.textSecondary
        font.pixelSize: control.hostWindow.dp(13)
    }

    background: Rectangle {
        radius: control.colors.cornerRadiusSmall
        color: control.colors.inputBackground
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus
                      ? control.colors.inputBorderFocused : control.colors.inputBorder
    }

    delegate: Basic.ItemDelegate {
        id: optionItem
        required property int index
        width: control.width
        text: control.options[index].label
        highlighted: control.highlightedIndex === index
        font.pixelSize: control.hostWindow.dp(12)

        contentItem: Text {
            text: optionItem.text
            color: control.colors.textPrimary
            font: optionItem.font
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            color: optionItem.highlighted
                   ? control.colors.accentSoft : control.colors.popupBackground
        }
    }

    popup: Basic.Popup {
        y: control.height + control.hostWindow.dp(4)
        width: control.width
        padding: control.hostWindow.dp(4)
        implicitHeight: Math.min(contentItem.implicitHeight + control.hostWindow.dp(8),
                                 control.hostWindow.dp(260))
        contentItem: ListView {
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            clip: true
            Basic.ScrollIndicator.vertical: Basic.ScrollIndicator { }
        }
        background: Rectangle {
            radius: control.colors.cornerRadiusSmall
            color: control.colors.popupBackground
            border.width: 1
            border.color: control.colors.border
        }
    }
}
