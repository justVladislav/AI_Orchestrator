pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic

// Selects a provider + model pair together as a single choice, since a
// conversation is always bound to exactly one of each at a time. Backed by
// providersModel (providerId, providerName, modelName, available,
// statusText). modelId is the persistent backend id; modelName is display-only.
Basic.ComboBox {
    id: control

    required property var hostWindow
    required property var colors
    required property var providersModel

    property string currentProviderId: ""
    property string currentModelId: ""

    signal providerSelected(string providerId, string modelId)

    model: providersModel
    hoverEnabled: true
    leftPadding: hostWindow.dp(10)
    rightPadding: hostWindow.dp(30)
    font.pixelSize: hostWindow.dp(12.5)

    Accessible.name: qsTr("Provider and model")
    ToolTip.visible: hovered && !popup.visible
    ToolTip.delay: 600
    ToolTip.text: currentIndex >= 0
                  ? providersModel.get(currentIndex).statusText
                  : qsTr("Choose a provider")

    function syncSelection() {
        for (let i = 0; i < providersModel.count; ++i) {
            const row = providersModel.get(i)

            if (row.providerId === control.currentProviderId
                    && row.modelId === control.currentModelId) {
                control.currentIndex = i
                return
            }
        }

        control.currentIndex = -1
    }

    Component.onCompleted: syncSelection()
    onCurrentProviderIdChanged: syncSelection()
    onCurrentModelIdChanged: syncSelection()

    onActivated: index => {
        const row = providersModel.get(index)
        control.providerSelected(row.providerId, row.modelId)
    }

    delegate: Basic.ItemDelegate {
        id: itemDelegate

        required property int index
        required property string providerId
        required property string providerName
        required property string modelName
        required property string modelId
        required property bool available
        required property string statusText

        width: control.width
        height: control.hostWindow.dp(46)
        leftPadding: control.hostWindow.dp(10)
        rightPadding: control.hostWindow.dp(10)
        enabled: available
        highlighted: control.highlightedIndex === index
        hoverEnabled: true

        Accessible.name: providerName + " " + modelName
                        + (available ? "" : " " + qsTr("unavailable"))

        contentItem: Row {
            spacing: control.hostWindow.dp(8)

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: control.hostWindow.dp(6)
                height: width
                radius: width / 2
                color: itemDelegate.available
                       ? control.colors.success
                       : control.colors.textDisabled
            }

            Column {
                width: control.width - control.hostWindow.dp(34)
                spacing: control.hostWindow.dp(1)

                Text {
                    width: parent.width
                    // Model first: with long provider names and a narrow
                    // control, the model is the part the user must see.
                    text: itemDelegate.modelName + " · " + itemDelegate.providerName
                    color: itemDelegate.available
                           ? control.colors.textPrimary
                           : control.colors.textDisabled
                    elide: Text.ElideRight
                    font.pixelSize: control.hostWindow.dp(12.5)
                }

                Text {
                    width: parent.width
                    text: itemDelegate.statusText
                    color: control.colors.textMuted
                    elide: Text.ElideRight
                    font.pixelSize: control.hostWindow.dp(10)
                }
            }
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
            color: control.colors.textSecondary
        }

        Rectangle {
            x: control.hostWindow.dp(6)
            y: control.hostWindow.dp(3)
            width: control.hostWindow.dp(7)
            height: Math.max(1, control.hostWindow.dp(1))
            rotation: -45
            transformOrigin: Item.Left
            color: control.colors.textSecondary
        }
    }

    contentItem: Text {
        leftPadding: 0
        rightPadding: 0
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        color: control.colors.textPrimary
        font: control.font
        // Below ~190dp there is not enough room for "Model · Provider" without
        // eliding the model name itself away — drop the provider suffix
        // entirely so the model (the part users actually need to read) always
        // renders in full instead of getting truncated.
        text: {
            if (control.currentIndex < 0)
                return qsTr("Select provider")
            const row = control.providersModel.get(control.currentIndex)
            return control.width >= control.hostWindow.dp(190)
                   ? row.modelName + " · " + row.providerName
                   : row.modelName
        }
    }

    background: Rectangle {
        radius: control.colors.cornerRadiusSmall
        color: control.colors.inputBackground
        border.width: control.activeFocus || control.down ? 1 : 0
        border.color: control.colors.inputBorderFocused
    }

    popup: Basic.Popup {
        y: control.height + control.hostWindow.dp(4)
        width: Math.max(control.width, control.hostWindow.dp(220))
        implicitHeight: Math.min(contentItem.implicitHeight + topPadding + bottomPadding,
                                 control.hostWindow.dp(280))
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
