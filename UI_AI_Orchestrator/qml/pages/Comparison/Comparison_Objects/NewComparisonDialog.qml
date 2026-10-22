pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog
    objectName: "newComparisonDialog"

    required property var hostWindow
    required property var colors
    property string projectId: ""
    property var modelChoices: []
    property int selectedModelCount: 0
    property var selectedModels: ({})

    signal submitted(var request)

    implicitWidth: Math.min(
            hostWindow.dp(620),
            Overlay.overlay ? Overlay.overlay.width - hostWindow.dp(40) : hostWindow.dp(620)
        )
    height: implicitHeight

    modal: true
    focus: true
    padding: hostWindow.dp(20)
    title: qsTr("New comparison")
    standardButtons: Dialog.NoButton
    font.pixelSize: hostWindow.dp(13)
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    // Grouped view: [{providerId, providerName, models:[{choiceIndex, modelId, modelName}]}]
    // Built from the flat modelChoices array; providers appear in catalog order.
    readonly property var groupedChoices: {
        const groups = {}
        const order = []
        for (let i = 0; i < dialog.modelChoices.length; ++i) {
            const c = dialog.modelChoices[i]
            if (!groups[c.providerId]) {
                groups[c.providerId] = { providerName: c.providerName, models: [] }
                order.push(c.providerId)
            }
            groups[c.providerId].models.push({
                choiceIndex: i,
                modelId: c.modelId,
                modelName: c.modelName
            })
        }
        return order.map(function(pid) {
            return {
                providerId: pid,
                providerName: groups[pid].providerName,
                models: groups[pid].models
            }
        })
    }

    onOpened: {
        titleField.clear()
        taskField.clear()
        titleField.forceActiveFocus()
        dialog.selectDefaults()
    }

    function modelSelected(choiceIndex) {
        return selectedModels[choiceIndex] === true
    }

    function setModelSelected(choiceIndex, selected) {
        const next = Object.assign({}, selectedModels)
        next[choiceIndex] = selected
        selectedModels = next
        recalculateSelectedCount()
    }

    function recalculateSelectedCount() {
        let count = 0
        for (let i = 0; i < dialog.modelChoices.length; ++i) {
            if (modelSelected(i))
                ++count
        }
        selectedModelCount = count
    }

    function selectDefaults() {
        const next = ({})
        for (let i = 0; i < dialog.modelChoices.length; ++i)
            next[i] = Boolean(dialog.modelChoices[i].isDefault)
        dialog.selectedModels = next
        dialog.recalculateSelectedCount()
    }

    function clearSelection() {
        const next = ({})
        for (let i = 0; i < dialog.modelChoices.length; ++i)
            next[i] = false
        dialog.selectedModels = next
        dialog.selectedModelCount = 0
    }

    function submit() {
        const task = taskField.text.trim()
        const selectedSlots = []
        for (let i = 0; i < dialog.modelChoices.length; ++i) {
            if (modelSelected(i)) {
                const choice = dialog.modelChoices[i]
                selectedSlots.push({
                    providerId: choice.providerId,
                    modelId: choice.modelId,
                    role: "candidate"
                })
            }
        }
        if (task.length === 0 || selectedSlots.length < 2)
            return
        dialog.submitted({
            projectId: dialog.projectId,
            title: titleField.text.trim(),
            prompt: task,
            slots: selectedSlots
        })
        dialog.close()
    }

    background: Rectangle {
        radius: dialog.colors.cornerRadiusMedium
        color: dialog.colors.popupBackground
        border.width: 1
        border.color: dialog.colors.border
    }

    header: Rectangle {
        implicitHeight: dialog.hostWindow.dp(50)
        color: dialog.colors.surfaceAlternative
        topLeftRadius: dialog.colors.cornerRadiusMedium
        topRightRadius: dialog.colors.cornerRadiusMedium

        Text {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: dialog.hostWindow.dp(18)
            anchors.rightMargin: dialog.hostWindow.dp(18)
            text: dialog.title
            color: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(15)
            font.weight: Font.DemiBold
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: dialog.colors.separator
        }
    }

    contentItem: ColumnLayout {
        spacing: dialog.hostWindow.dp(13)
        implicitWidth: dialog.implicitWidth - dialog.padding * 2
        // ── Instruction ────────────────────────────────────────────────
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Describe one task that the selected models should solve independently.")
            color: dialog.colors.textSecondary
            font.pixelSize: dialog.hostWindow.dp(14)
        }

        // ── Title ──────────────────────────────────────────────────────
        Text {
            text: qsTr("Title (optional)")
            color: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(13)
            font.weight: Font.Medium
        }

        TextField {
            id: titleField
            objectName: "comparisonTitleField"
            Layout.fillWidth: true
            Layout.preferredHeight: dialog.hostWindow.dp(38)
            placeholderText: qsTr("For example: API response quality")
            color: dialog.colors.textPrimary
            placeholderTextColor: dialog.colors.textMuted
            font.pixelSize: dialog.hostWindow.dp(13)
            selectByMouse: true
            verticalAlignment: TextInput.AlignVCenter
            leftPadding: dialog.hostWindow.dp(12)
            rightPadding: dialog.hostWindow.dp(12)
            background: Rectangle {
                radius: dialog.colors.cornerRadiusSmall
                color: dialog.colors.inputBackground
                border.width: 1
                border.color: titleField.activeFocus
                              ? dialog.colors.inputBorderFocused
                              : dialog.colors.border
            }
        }

        // ── Task ───────────────────────────────────────────────────────
        Text {
            text: qsTr("Task")
            color: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(13)
            font.weight: Font.Medium
        }

        TextArea {
            id: taskField
            objectName: "comparisonTaskField"
            Layout.fillWidth: true
            Layout.preferredHeight: dialog.hostWindow.dp(104)
            wrapMode: TextArea.Wrap
            placeholderText: qsTr("Describe the objective, constraints and expected result.")
            color: dialog.colors.textPrimary
            placeholderTextColor: dialog.colors.textMuted
            font.pixelSize: dialog.hostWindow.dp(13)
            selectByMouse: true
            leftPadding: dialog.hostWindow.dp(12)
            rightPadding: dialog.hostWindow.dp(16)
            topPadding: dialog.hostWindow.dp(10)
            bottomPadding: dialog.hostWindow.dp(10)
            background: Rectangle {
                radius: dialog.colors.cornerRadiusSmall
                color: dialog.colors.inputBackground
                border.width: 1
                border.color: taskField.activeFocus
                              ? dialog.colors.inputBorderFocused
                              : dialog.colors.border
            }
        }

        // ── Models header ──────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: dialog.hostWindow.dp(2)
            spacing: dialog.hostWindow.dp(7)

            Text {
                Layout.fillWidth: true
                text: qsTr("Models")
                color: dialog.colors.textPrimary
                font.pixelSize: dialog.hostWindow.dp(13)
                font.weight: Font.DemiBold
            }

            Rectangle {
                Layout.preferredHeight: dialog.hostWindow.dp(24)
                Layout.preferredWidth: selectionCountLabel.implicitWidth
                                       + dialog.hostWindow.dp(16)
                radius: height / 2
                color: dialog.selectedModelCount >= 2
                       ? dialog.colors.successSoft
                       : dialog.colors.surfaceAlternative
                border.width: 1
                border.color: dialog.selectedModelCount >= 2
                              ? dialog.colors.success
                              : dialog.colors.border

                Text {
                    id: selectionCountLabel
                    anchors.centerIn: parent
                    text: dialog.selectedModelCount >= 2
                          ? qsTr("%1 selected").arg(dialog.selectedModelCount)
                          : qsTr("Select at least 2")
                    color: dialog.selectedModelCount >= 2
                           ? dialog.colors.success
                           : dialog.colors.textMuted
                    font.pixelSize: dialog.hostWindow.dp(10)
                    font.weight: dialog.selectedModelCount >= 2
                                 ? Font.DemiBold : Font.Normal
                }
            }

            ComparisonActionButton {
                text: qsTr("Defaults")
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                enabled: dialog.modelChoices.length > 0
                onClicked: dialog.selectDefaults()
            }

            ComparisonActionButton {
                text: qsTr("Clear")
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                enabled: dialog.selectedModelCount > 0
                onClicked: dialog.clearSelection()
            }
        }

        // ── Model list — providers grouped, scrollable ─────────────────
        Rectangle {
            id: modelListBox
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(
                modelListFlickable.contentHeight + dialog.hostWindow.dp(2),
                dialog.hostWindow.dp(220))
            Layout.minimumHeight: dialog.hostWindow.dp(72)
            color: dialog.colors.inputBackground
            radius: dialog.colors.cornerRadiusSmall
            border.width: 1
            border.color: dialog.colors.border
            clip: true

            Flickable {
                id: modelListFlickable
                anchors.fill: parent
                contentWidth: width
                contentHeight: groupsLayout.implicitHeight
                clip: false
                boundsBehavior: Flickable.StopAtBounds

                ScrollBar.vertical: ComparisonScrollBar {
                    id: modelListScrollBar
                    hostWindow: dialog.hostWindow
                    colors: dialog.colors
                    policy: modelListFlickable.contentHeight > modelListFlickable.height
                            ? ScrollBar.AlwaysOn
                            : ScrollBar.AsNeeded
                }

                ColumnLayout {
                    id: groupsLayout
                    width: Math.max(0, modelListFlickable.width
                                    - modelListScrollBar.width
                                    - dialog.hostWindow.dp(3))
                    spacing: 0

                    // Empty state: no enabled models in catalog
                    Text {
                        Layout.fillWidth: true
                        Layout.margins: dialog.hostWindow.dp(14)
                        visible: dialog.modelChoices.length === 0
                        text: qsTr("No models available. Add providers and models on the Providers page.")
                        color: dialog.colors.textMuted
                        font.pixelSize: dialog.hostWindow.dp(11)
                        wrapMode: Text.WordWrap
                    }

                    // Provider groups
                    Repeater {
                        id: providerRepeater
                        model: dialog.groupedChoices

                        ColumnLayout {
                            id: providerGroup
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            spacing: 0

                            // Separator before every group except the first
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 1
                                color: dialog.colors.separator
                                visible: providerGroup.index > 0
                            }

                            // Provider header bar
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: dialog.hostWindow.dp(32)
                                color: dialog.colors.surfaceAlternative

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: dialog.hostWindow.dp(10)
                                    anchors.rightMargin: dialog.hostWindow.dp(10)
                                    spacing: dialog.hostWindow.dp(7)

                                    Rectangle {
                                        Layout.preferredWidth: dialog.hostWindow.dp(6)
                                        Layout.preferredHeight: dialog.hostWindow.dp(6)
                                        radius: width / 2
                                        color: dialog.colors.accent
                                    }

                                    Text {
                                        id: providerLabel
                                        Layout.fillWidth: true
                                        text: providerGroup.modelData.providerName
                                        color: dialog.colors.textPrimary
                                        font.pixelSize: dialog.hostWindow.dp(11)
                                        font.weight: Font.DemiBold
                                        elide: Text.ElideRight
                                    }

                                    Text {
                                        text: qsTr("%1 models").arg(
                                                  providerGroup.modelData.models.length)
                                        color: dialog.colors.textMuted
                                        font.pixelSize: dialog.hostWindow.dp(9)
                                    }
                                }
                            }

                            // Model checkboxes for this provider
                            Repeater {
                                model: providerGroup.modelData.models

                                CheckBox {
                                    id: modelChoice
                                    required property var modelData
                                    required property int index
                                    // objectName uses global choiceIndex so QmlFrontend tests can locate it
                                    objectName: "comparisonModelChoice_" + modelChoice.modelData.choiceIndex
                                    Layout.fillWidth: true
                                    implicitHeight: dialog.hostWindow.dp(36)
                                    hoverEnabled: true
                                    checked: dialog.modelSelected(modelChoice.modelData.choiceIndex)
                                    text: modelChoice.modelData.modelName
                                    onToggled: dialog.setModelSelected(
                                                   modelChoice.modelData.choiceIndex, checked)

                                    indicator: Rectangle {
                                        implicitWidth: dialog.hostWindow.dp(16)
                                        implicitHeight: dialog.hostWindow.dp(16)
                                        x: dialog.hostWindow.dp(10)
                                        y: (modelChoice.height - height) / 2
                                        radius: dialog.hostWindow.dp(4)
                                        color: modelChoice.checked
                                               ? dialog.colors.accent
                                               : "transparent"
                                        border.width: 1
                                        border.color: modelChoice.checked
                                                      ? dialog.colors.accent
                                                      : dialog.colors.borderStrong

                                        Text {
                                            anchors.centerIn: parent
                                            visible: modelChoice.checked
                                            text: "✓"
                                            color: dialog.colors.textOnAccent
                                            font.pixelSize: dialog.hostWindow.dp(11)
                                            font.weight: Font.DemiBold
                                        }
                                    }

                                    contentItem: Text {
                                        leftPadding: dialog.hostWindow.dp(36)
                                        rightPadding: dialog.hostWindow.dp(10)
                                        text: modelChoice.text
                                        color: dialog.colors.textPrimary
                                        font.pixelSize: dialog.hostWindow.dp(12)
                                        verticalAlignment: Text.AlignVCenter
                                        elide: Text.ElideRight
                                    }

                                    background: Rectangle {
                                        color: modelChoice.hovered
                                               ? dialog.colors.buttonHover
                                               : "transparent"
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // ── Action buttons ─────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: dialog.hostWindow.dp(4)
            spacing: dialog.hostWindow.dp(8)

            Item { Layout.fillWidth: true }

            ComparisonActionButton {
                text: qsTr("Cancel")
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                onClicked: dialog.close()
            }

            ComparisonActionButton {
                text: qsTr("Create comparison")
                primary: true
                enabled: taskField.text.trim().length > 0 && dialog.selectedModelCount >= 2
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                onClicked: dialog.submit()
            }
        }
    }
}
