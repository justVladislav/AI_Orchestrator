pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

ComboBox {
    id: control

    required property var hostWindow
    required property var colors

    leftPadding: hostWindow.dp(12)
    rightPadding: hostWindow.dp(38)
    font.pixelSize: hostWindow.dp(13)
    hoverEnabled: true

    delegate: ItemDelegate {
        id: itemDelegate

        required property var modelData
        required property int index

        width: control.width
        height: control.hostWindow.dp(38)
        text: control.textRole.length > 0 && modelData
              ? String(modelData[control.textRole]) : String(modelData)
        highlighted: control.highlightedIndex === index
        hoverEnabled: true

        contentItem: Text {
            leftPadding: control.hostWindow.dp(8)
            rightPadding: control.hostWindow.dp(8)
            text: itemDelegate.text
            color: control.colors.textPrimary
            font.pixelSize: control.hostWindow.dp(13)
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        background: Rectangle {
            radius: control.colors.cornerRadiusSmall
            color: itemDelegate.highlighted || itemDelegate.hovered
                   ? control.colors.buttonHover
                   : "transparent"
        }
    }

    indicator: Item {
        x: control.width - width - control.hostWindow.dp(12)
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
            color: control.enabled
                   ? control.colors.textSecondary
                   : control.colors.textDisabled
        }

        Rectangle {
            x: control.hostWindow.dp(6)
            y: control.hostWindow.dp(3)
            width: control.hostWindow.dp(7)
            height: Math.max(1, control.hostWindow.dp(1))
            rotation: -45
            transformOrigin: Item.Left
            color: control.enabled
                   ? control.colors.textSecondary
                   : control.colors.textDisabled
        }
    }

    contentItem: Text {
        // ComboBox already applies its own padding to contentItem geometry.
        // Reapplying it here made otherwise fitting labels elide too early.
        leftPadding: 0
        rightPadding: 0
        text: control.displayText
        color: control.enabled
               ? control.colors.textPrimary
               : control.colors.textDisabled
        font: control.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        radius: control.colors.cornerRadiusSmall
        color: control.colors.inputBackground
        border.width: control.activeFocus || control.down ? 1 : 0
        border.color: control.colors.inputBorderFocused
    }

    popup: Popup {
        y: control.height + control.hostWindow.dp(4)
        width: control.width
        implicitHeight: Math.min(contentItem.implicitHeight
                                 + topPadding + bottomPadding,
                                 control.hostWindow.dp(260))
        padding: control.hostWindow.dp(4)

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds

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
