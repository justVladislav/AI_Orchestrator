pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

ComboBox {
    id: control

    required property var hostWindow
    required property var colors

    property var itemsModel: null
    property string itemIdRole: "itemId"
    property string itemLabelRole: "itemLabel"
    property string currentItemId: ""
    property string placeholderText: ""

    signal itemSelected(string itemId)

    model: itemsModel
    textRole: itemLabelRole
    valueRole: itemIdRole
    hoverEnabled: true
    leftPadding: hostWindow.dp(10)
    rightPadding: hostWindow.dp(28)
    font.pixelSize: hostWindow.dp(12.5)

    function syncSelection() {
        if (currentItemId.length === 0) {
            currentIndex = -1
            return
        }

        const matchingIndex = indexOfValue(currentItemId)
        currentIndex = matchingIndex
    }

    Component.onCompleted: Qt.callLater(syncSelection)
    onCurrentItemIdChanged: syncSelection()
    onModelChanged: Qt.callLater(syncSelection)
    onCountChanged: Qt.callLater(syncSelection)

    onActivated: {
        const selectedValue = currentValue
        control.itemSelected(selectedValue === undefined || selectedValue === null
                             ? "" : String(selectedValue))
    }

    delegate: ItemDelegate {
        id: itemDelegate

        required property int index

        width: control.width
        height: control.hostWindow.dp(36)
        leftPadding: control.hostWindow.dp(10)
        rightPadding: control.hostWindow.dp(10)
        highlighted: control.highlightedIndex === index
        hoverEnabled: true

        contentItem: Text {
            text: control.textAt(itemDelegate.index)
            color: control.colors.textPrimary
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
            font.pixelSize: control.hostWindow.dp(12.5)
        }

        background: Rectangle {
            radius: control.colors.cornerRadiusSmall
            color: itemDelegate.highlighted || itemDelegate.hovered
                   ? control.colors.buttonHover
                   : "transparent"
        }
    }

    indicator: Item {
        x: control.width - width - control.hostWindow.dp(10)
        y: (control.height - height) / 2
        width: control.hostWindow.dp(14)
        height: control.hostWindow.dp(10)

        Rectangle {
            x: control.hostWindow.dp(1)
            y: control.hostWindow.dp(3)
            width: control.hostWindow.dp(7)
            height: Math.max(1, control.hostWindow.dp(1))
            rotation: 45
            transformOrigin: Item.Right
            color: control.enabled ? control.colors.textSecondary : control.colors.textDisabled
        }

        Rectangle {
            x: control.hostWindow.dp(6)
            y: control.hostWindow.dp(3)
            width: control.hostWindow.dp(7)
            height: Math.max(1, control.hostWindow.dp(1))
            rotation: -45
            transformOrigin: Item.Left
            color: control.enabled ? control.colors.textSecondary : control.colors.textDisabled
        }
    }

    contentItem: Text {
        leftPadding: 0
        rightPadding: 0
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        color: control.enabled ? control.colors.textPrimary : control.colors.textDisabled
        font: control.font
        text: control.currentIndex >= 0 ? control.currentText : control.placeholderText
    }

    background: Rectangle {
        radius: control.colors.cornerRadiusSmall
        color: control.colors.inputBackground
        border.width: control.activeFocus || control.down ? 1 : 0
        border.color: control.colors.inputBorderFocused
    }

    popup: Popup {
        y: control.height + control.hostWindow.dp(4)
        width: Math.max(control.width, control.hostWindow.dp(200))
        implicitHeight: Math.min(contentItem.implicitHeight + topPadding + bottomPadding,
                                 control.hostWindow.dp(260))
        padding: control.hostWindow.dp(4)

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds

            ScrollBar.vertical: ScrollBar {
                id: popupScrollBar
                policy: ScrollBar.AsNeeded
                contentItem: Rectangle {
                    implicitWidth: control.hostWindow.dp(4)
                    radius: width / 2
                    color: popupScrollBar.pressed ? control.colors.accent : control.colors.textMuted
                    opacity: popupScrollBar.active ? 0.72 : 0.28
                }
                background: Item { }
            }
        }

        background: Rectangle {
            radius: control.colors.cornerRadiusSmall
            color: control.colors.popupBackground
            border.width: 1
            border.color: control.colors.border
        }
    }
}
