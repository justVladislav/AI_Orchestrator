pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog

    required property var hostWindow
    required property var colors
    property string modelId: ""
    property string modelName: ""
    property string errorText: ""

    // The whole pricing form travels in ONE signal (the page saves it in ONE atomic Backend update):
    // the flat prices, the cache prices and the long-context tier. longContextEnabled == false is the
    // explicit removal of the tier (the page then sends zeros); the Backend validates everything else.
    signal pricingAccepted(string modelId, string inputPrice, string outputPrice,
                            string cacheWritePrice, string cacheReadPrice,
                            string cacheWriteOneHourPrice,
                            bool longContextEnabled, string longContextThreshold,
                            string longContextInputPrice, string longContextOutputPrice,
                            string longContextCacheWritePrice, string longContextCacheReadPrice,
                            string longContextCacheWriteOneHourPrice)

    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - hostWindow.dp(32) : hostWindow.dp(440),
                    hostWindow.dp(440))
    // A tall form scrolls instead of running off a small window.
    height: Math.min(implicitHeight, parent ? parent.height - hostWindow.dp(32) : implicitHeight)
    modal: true
    standardButtons: Dialog.NoButton

    function decimalPrice(micros) {
        let value = (Number(micros || 0) / 1000000).toFixed(6)
        value = value.replace(/0+$/, "").replace(/\.$/, "")
        return value.length > 0 ? value : "0"
    }

    // Every value below is exactly what the Backend stored: nothing is guessed or pre-filled for a
    // model that has no long-context tier (its tier fields stay empty until the user types them).
    function openForModel(modelData) {
        modelId = modelData.modelId
        modelName = modelData.name
        inputPriceField.text = decimalPrice(modelData.inputPriceMicros)
        outputPriceField.text = decimalPrice(modelData.outputPriceMicros)
        cacheWritePriceField.text = decimalPrice(modelData.cacheWritePriceMicros)
        cacheReadPriceField.text = decimalPrice(modelData.cacheReadPriceMicros)
        cacheWriteOneHourPriceField.text = decimalPrice(modelData.cacheWriteOneHourPriceMicros)
        const tierEnabled = Boolean(modelData.longContextEnabled)
        longContextCheck.checked = tierEnabled
        longContextThresholdField.text = tierEnabled
            ? String(Number(modelData.longContextThresholdInputTokens || 0)) : ""
        longContextInputPriceField.text = tierEnabled
            ? decimalPrice(modelData.longContextInputPriceMicros) : ""
        longContextOutputPriceField.text = tierEnabled
            ? decimalPrice(modelData.longContextOutputPriceMicros) : ""
        longContextCacheWritePriceField.text = tierEnabled
            ? decimalPrice(modelData.longContextCacheWritePriceMicros) : ""
        longContextCacheReadPriceField.text = tierEnabled
            ? decimalPrice(modelData.longContextCacheReadPriceMicros) : ""
        longContextCacheWriteOneHourPriceField.text = tierEnabled
            ? decimalPrice(modelData.longContextCacheWriteOneHourPriceMicros) : ""
        errorText = ""
        open()
    }

    onOpened: inputPriceField.forceActiveFocus()

    background: Rectangle {
        radius: dialog.colors.cornerRadiusMedium
        color: dialog.colors.popupBackground
        border.width: 1
        border.color: dialog.colors.border
    }

    header: Rectangle {
        implicitHeight: dialog.hostWindow.dp(54)
        color: dialog.colors.surfaceAlternative
        Text {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: dialog.hostWindow.dp(18)
            anchors.rightMargin: dialog.hostWindow.dp(18)
            text: qsTr("Model pricing")
            color: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(15)
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
    }

    contentItem: ScrollView {
        id: scroller
        clip: true
        contentWidth: availableWidth

        ColumnLayout {
            width: scroller.availableWidth
            spacing: dialog.hostWindow.dp(12)

            Text {
                Layout.fillWidth: true
                text: dialog.modelName
                color: dialog.colors.textPrimary
                font.pixelSize: dialog.hostWindow.dp(12)
                font.weight: Font.Medium
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Enter prices in US dollars per one million tokens. They apply to new executions; the stored cost of an execution that already finished is never rewritten.")
                color: dialog.colors.textSecondary
                font.pixelSize: dialog.hostWindow.dp(10)
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: dialog.hostWindow.dp(10)
                PriceField {
                    id: inputPriceField
                    objectName: "inputPriceField"
                    Layout.fillWidth: true
                    label: qsTr("Input tokens")
                }
                PriceField {
                    id: outputPriceField
                    objectName: "outputPriceField"
                    Layout.fillWidth: true
                    label: qsTr("Output tokens")
                }
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("Prompt cache rates. Leave at 0 for a model/provider that never reports cache usage.")
                color: dialog.colors.textSecondary
                font.pixelSize: dialog.hostWindow.dp(10)
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: dialog.hostWindow.dp(10)
                PriceField {
                    id: cacheWritePriceField
                    objectName: "cacheWritePriceField"
                    Layout.fillWidth: true
                    label: qsTr("Cache write (5 min)")
                }
                PriceField {
                    id: cacheReadPriceField
                    objectName: "cacheReadPriceField"
                    Layout.fillWidth: true
                    label: qsTr("Cache read")
                }
            }
            PriceField {
                id: cacheWriteOneHourPriceField
                objectName: "cacheWriteOneHourPriceField"
                Layout.fillWidth: true
                label: qsTr("Cache write (1 hour)")
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: dialog.colors.separator
            }

            CheckBox {
                id: longContextCheck
                objectName: "longContextCheck"
                text: qsTr("Long-context pricing")
                palette.text: dialog.colors.textPrimary
                font.pixelSize: dialog.hostWindow.dp(11)
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Long-context prices apply when a single Response reports more than this number of input tokens.")
                color: dialog.colors.textSecondary
                font.pixelSize: dialog.hostWindow.dp(10)
                wrapMode: Text.WordWrap
            }

            ColumnLayout {
                id: longContextSection
                objectName: "longContextSection"
                Layout.fillWidth: true
                visible: longContextCheck.checked
                spacing: dialog.hostWindow.dp(12)

                PriceField {
                    id: longContextThresholdField
                    objectName: "longContextThresholdField"
                    Layout.fillWidth: true
                    label: qsTr("Threshold")
                    currency: false
                    unit: qsTr(" (input tokens)")
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: dialog.hostWindow.dp(10)
                    PriceField {
                        id: longContextInputPriceField
                        objectName: "longContextInputPriceField"
                        Layout.fillWidth: true
                        label: qsTr("Input tokens")
                    }
                    PriceField {
                        id: longContextOutputPriceField
                        objectName: "longContextOutputPriceField"
                        Layout.fillWidth: true
                        label: qsTr("Output tokens")
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: dialog.hostWindow.dp(10)
                    PriceField {
                        id: longContextCacheWritePriceField
                        objectName: "longContextCacheWritePriceField"
                        Layout.fillWidth: true
                        label: qsTr("Cache write (5 min)")
                    }
                    PriceField {
                        id: longContextCacheReadPriceField
                        objectName: "longContextCacheReadPriceField"
                        Layout.fillWidth: true
                        label: qsTr("Cache read")
                    }
                }
                PriceField {
                    id: longContextCacheWriteOneHourPriceField
                    objectName: "longContextCacheWriteOneHourPriceField"
                    Layout.fillWidth: true
                    label: qsTr("Cache write (1 hour)")
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("A price left at 0 counts as not entered: any cost that uses it is shown as a lower bound (\"at least\").")
                    color: dialog.colors.textSecondary
                    font.pixelSize: dialog.hostWindow.dp(10)
                    wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Reference only, never applied automatically: GPT-6 Sol Standard pricing switches to higher rates above 272,000 input tokens. Verify current pricing before saving.")
                    color: dialog.colors.textMuted
                    font.pixelSize: dialog.hostWindow.dp(9)
                    wrapMode: Text.WordWrap
                }
            }

            Text {
                objectName: "pricingErrorText"
                Layout.fillWidth: true
                visible: dialog.errorText.length > 0
                text: dialog.errorText
                color: dialog.colors.error
                font.pixelSize: dialog.hostWindow.dp(9)
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                ProviderActionButton {
                    text: qsTr("Cancel")
                    hostWindow: dialog.hostWindow
                    colors: dialog.colors
                    onClicked: dialog.reject()
                }
                ProviderActionButton {
                    objectName: "savePricingButton"
                    text: qsTr("Save pricing")
                    primary: true
                    hostWindow: dialog.hostWindow
                    colors: dialog.colors
                    onClicked: dialog.pricingAccepted(dialog.modelId,
                                                       inputPriceField.text.trim(),
                                                       outputPriceField.text.trim(),
                                                       cacheWritePriceField.text.trim(),
                                                       cacheReadPriceField.text.trim(),
                                                       cacheWriteOneHourPriceField.text.trim(),
                                                       longContextCheck.checked,
                                                       longContextThresholdField.text.trim(),
                                                       longContextInputPriceField.text.trim(),
                                                       longContextOutputPriceField.text.trim(),
                                                       longContextCacheWritePriceField.text.trim(),
                                                       longContextCacheReadPriceField.text.trim(),
                                                       longContextCacheWriteOneHourPriceField.text.trim())
                }
            }
        }
    }

    component PriceField: ColumnLayout {
        id: priceControl
        property alias text: editor.text
        property string label: ""
        // A currency field shows "$" and is priced per one million tokens; a token-count field
        // (the long-context threshold) shows neither.
        property bool currency: true
        property string unit: qsTr(" ($ / 1M)")
        spacing: dialog.hostWindow.dp(5)
        Text {
            text: priceControl.label + priceControl.unit
            color: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(10)
            font.weight: Font.Medium
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: dialog.hostWindow.dp(40)
            radius: dialog.colors.cornerRadiusSmall
            color: dialog.colors.inputBackground
            border.width: editor.activeFocus ? 1 : 0
            border.color: dialog.colors.inputBorderFocused
            Text {
                visible: priceControl.currency
                anchors.left: parent.left
                anchors.leftMargin: dialog.hostWindow.dp(11)
                anchors.verticalCenter: parent.verticalCenter
                text: "$"
                color: dialog.colors.textMuted
                font.pixelSize: dialog.hostWindow.dp(11)
            }
            TextInput {
                id: editor
                anchors.fill: parent
                anchors.leftMargin: priceControl.currency ? dialog.hostWindow.dp(28) : dialog.hostWindow.dp(11)
                anchors.rightMargin: dialog.hostWindow.dp(10)
                color: dialog.colors.textPrimary
                font.pixelSize: dialog.hostWindow.dp(11)
                verticalAlignment: TextInput.AlignVCenter
                selectByMouse: true
                inputMethodHints: Qt.ImhFormattedNumbersOnly
            }
        }
    }
}
