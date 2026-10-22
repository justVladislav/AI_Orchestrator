pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: panel

    required property var hostWindow
    required property var colors
    property var summaryData: null

    signal sendToSynthesisRequested()

    readonly property string synthesisStatus: stringValue("synthesisStatus", "idle")
    readonly property bool synthesisEnabled: boolValue("synthesisEnabled", false)

    radius: colors.cornerRadiusMedium
    color: colors.surfaceAlternative
    border.width: 1
    border.color: colors.border
    clip: true
    implicitHeight: hostWindow.dp(320)

    function value(name, fallback) {
        if (!summaryData || summaryData[name] === undefined || summaryData[name] === null)
            return fallback
        return summaryData[name]
    }

    function stringValue(name, fallback) {
        const result = value(name, fallback)
        return result === undefined || result === null ? fallback : String(result)
    }

    function boolValue(name, fallback) {
        return value(name, fallback) === true
    }

    function listValue(name) {
        const result = value(name, [])
        return result && result.length !== undefined ? result : []
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: panel.hostWindow.dp(14)
        spacing: panel.hostWindow.dp(10)

        Text {
            Layout.fillWidth: true
            text: qsTr("Comparison summary")
            color: panel.colors.textPrimary
            font.pixelSize: panel.hostWindow.dp(14)
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }

        Flickable {
            id: summaryFlickable
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: summaryColumn.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            ScrollBar.vertical: ScrollBar {
                id: summaryScrollBar
                policy: ScrollBar.AsNeeded
                contentItem: Rectangle {
                    implicitWidth: panel.hostWindow.dp(4)
                    radius: width / 2
                    color: summaryScrollBar.pressed ? panel.colors.accent : panel.colors.textMuted
                    opacity: summaryScrollBar.active ? 0.72 : 0.28
                }
                background: Item { }
            }

            ColumnLayout {
                id: summaryColumn
                width: Math.max(0, summaryFlickable.width - summaryScrollBar.width
                                - panel.hostWindow.dp(4))
                spacing: panel.hostWindow.dp(12)

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: panel.hostWindow.dp(2)

                    Text {
                        text: qsTr("Recommended result")
                        color: panel.colors.textMuted
                        font.pixelSize: panel.hostWindow.dp(10)
                    }

                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: panel.stringValue("recommendedLabel", "").length > 0
                              ? panel.stringValue("recommendedLabel", "")
                              : qsTr("Not enough evaluated results yet.")
                        color: panel.colors.textPrimary
                        font.pixelSize: panel.hostWindow.dp(13)
                        font.weight: Font.Medium
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: panel.stringValue("recommendedLabel", "").length > 0
                        wrapMode: Text.WordWrap
                        text: {
                            const source = panel.stringValue("recommendationSource", "")
                            if (source === "human")
                                return qsTr("Preferred by human evaluation")
                            if (source === "quality")
                                return qsTr("Suggested by the persisted quality evaluation")
                            return qsTr("Recommended by the comparison service")
                        }
                        color: panel.colors.textMuted
                        font.pixelSize: panel.hostWindow.dp(10)
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: panel.hostWindow.dp(3)
                    visible: panel.listValue("consensusPoints").length > 0

                    Text {
                        text: qsTr("Where they agree")
                        color: panel.colors.textSecondary
                        font.pixelSize: panel.hostWindow.dp(10.5)
                        font.weight: Font.DemiBold
                    }

                    Repeater {
                        model: panel.listValue("consensusPoints")
                        delegate: Text {
                            required property string modelData
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("• %1").arg(modelData)
                            color: panel.colors.textSecondary
                            font.pixelSize: panel.hostWindow.dp(11)
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: panel.hostWindow.dp(3)
                    visible: panel.listValue("differencePoints").length > 0

                    Text {
                        text: qsTr("Main differences")
                        color: panel.colors.textSecondary
                        font.pixelSize: panel.hostWindow.dp(10.5)
                        font.weight: Font.DemiBold
                    }

                    Repeater {
                        model: panel.listValue("differencePoints")
                        delegate: Text {
                            required property string modelData
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("• %1").arg(modelData)
                            color: panel.colors.textSecondary
                            font.pixelSize: panel.hostWindow.dp(11)
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: panel.hostWindow.dp(3)
                    visible: panel.listValue("warnings").length > 0

                    Text {
                        text: qsTr("Warnings")
                        color: panel.colors.warning
                        font.pixelSize: panel.hostWindow.dp(10.5)
                        font.weight: Font.DemiBold
                    }

                    Repeater {
                        model: panel.listValue("warnings")
                        delegate: Text {
                            required property string modelData
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("• %1").arg(modelData)
                            color: panel.colors.warning
                            font.pixelSize: panel.hostWindow.dp(11)
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: totalsGrid.implicitHeight + panel.hostWindow.dp(18)
                    radius: panel.colors.cornerRadiusSmall
                    color: panel.colors.inputBackground
                    border.width: 1
                    border.color: panel.colors.border

                    GridLayout {
                        id: totalsGrid
                        anchors.fill: parent
                        anchors.margins: panel.hostWindow.dp(10)
                        columns: 2
                        columnSpacing: panel.hostWindow.dp(12)
                        rowSpacing: panel.hostWindow.dp(5)

                        Text {
                            text: qsTr("Total cost")
                            color: panel.colors.textMuted
                            font.pixelSize: panel.hostWindow.dp(10)
                        }
                        Text {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignRight
                            text: panel.stringValue("totalCostText", "—")
                            color: panel.colors.textPrimary
                            font.pixelSize: panel.hostWindow.dp(11)
                            elide: Text.ElideLeft
                        }

                        Text {
                            text: qsTr("Total tokens")
                            color: panel.colors.textMuted
                            font.pixelSize: panel.hostWindow.dp(10)
                        }
                        Text {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignRight
                            text: panel.stringValue("totalTokensText", "—")
                            color: panel.colors.textPrimary
                            font.pixelSize: panel.hostWindow.dp(11)
                            elide: Text.ElideLeft
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: panel.hostWindow.dp(3)

                    Text {
                        text: qsTr("Selected for synthesis")
                        color: panel.colors.textMuted
                        font.pixelSize: panel.hostWindow.dp(10)
                    }

                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: panel.listValue("selectedLabels").length > 0
                              ? panel.listValue("selectedLabels").join(qsTr(", "))
                              : qsTr("None selected yet")
                        color: panel.colors.textSecondary
                        font.pixelSize: panel.hostWindow.dp(11)
                    }
                }
            }
        }

        ComparisonActionButton {
            Layout.fillWidth: true
            text: panel.synthesisStatus === "running"
                  ? qsTr("Synthesizing…") : qsTr("Send to synthesis")
            primary: true
            enabled: panel.synthesisEnabled && panel.synthesisStatus !== "running"
            hostWindow: panel.hostWindow
            colors: panel.colors
            onClicked: panel.sendToSynthesisRequested()
        }

        Text {
            Layout.fillWidth: true
            visible: !panel.synthesisEnabled || panel.synthesisStatus === "failed"
            wrapMode: Text.WordWrap
            text: panel.synthesisStatus === "failed"
                  ? qsTr("Synthesis failed. The persisted results remain available.")
                  : qsTr("Select at least two completed results to enable synthesis.")
            color: panel.synthesisStatus === "failed"
                   ? panel.colors.error : panel.colors.textMuted
            font.pixelSize: panel.hostWindow.dp(10.5)
            maximumLineCount: 2
            elide: Text.ElideRight
        }
    }
}
