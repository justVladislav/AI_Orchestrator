pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: card

    required property var hostWindow
    required property var colors

    property string resultId: ""
    property string providerName: ""
    property string modelName: ""
    property string status: "queued"
    property string content: ""
    property string errorText: ""
    property var strengths: []
    property var warnings: []
    property int inputTokens: 0
    property int outputTokens: 0
    property string totalCostText: "—"
    property string latencyText: "—"
    property bool qualityAvailable: false
    property real qualityScore: 0
    property string qualitySource: ""
    property bool isPreferred: false
    property bool selectedForSynthesis: false
    property bool canRetry: false
    property bool canSelectForSynthesis: false

    signal retryRequested(string resultId)
    signal preferredRequested(string resultId)
    signal synthesisSelectionRequested(string resultId, bool selected)
    signal humanRatingRequested(string resultId, int score, string comment)

    readonly property bool isQueued: status === "queued"
    readonly property bool isRunning: status === "running"
    readonly property bool isCompleted: status === "completed"
    readonly property bool isPartial: status === "partial"
    readonly property bool isFailed: status === "failed"
    readonly property bool isCancelled: status === "cancelled"
    readonly property real logicalWidth: width / Math.max(hostWindow.scaleFactor, 0.01)
    readonly property bool compactActions: logicalWidth < 300

    readonly property color statusColor: isCompleted
                                         ? colors.success
                                         : isRunning
                                           ? colors.information
                                           : isPartial || isQueued
                                             ? colors.warning
                                             : isFailed
                                               ? colors.error
                                               : colors.textMuted
    readonly property string statusLabel: {
        switch (status) {
        case "queued": return qsTr("Queued")
        case "running": return qsTr("Running")
        case "completed": return qsTr("Completed")
        case "partial": return qsTr("Partial")
        case "failed": return qsTr("Failed")
        case "cancelled": return qsTr("Cancelled")
        default: return qsTr("Unknown")
        }
    }

    radius: colors.cornerRadiusMedium
    color: colors.surfaceAlternative
    border.width: isPreferred ? 2 : 1
    border.color: isPreferred ? colors.accent : colors.border
    clip: true

    TextEdit {
        id: clipboardBridge
        visible: false
        text: ""
    }

    function copyResponse() {
        if (content.length === 0)
            return
        clipboardBridge.text = content
        clipboardBridge.selectAll()
        clipboardBridge.copy()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: card.hostWindow.dp(14)
        spacing: card.hostWindow.dp(10)

        RowLayout {
            Layout.fillWidth: true
            spacing: card.hostWindow.dp(8)

            ColumnLayout {
                Layout.fillWidth: true
                spacing: card.hostWindow.dp(1)

                Text {
                    Layout.fillWidth: true
                    text: card.providerName
                    color: card.colors.textPrimary
                    elide: Text.ElideRight
                    font.pixelSize: card.hostWindow.dp(14)
                    font.weight: Font.DemiBold
                }

                Text {
                    Layout.fillWidth: true
                    text: card.modelName
                    color: card.colors.textMuted
                    elide: Text.ElideRight
                    font.pixelSize: card.hostWindow.dp(10.5)
                }
            }

            Rectangle {
                Layout.preferredWidth: statusLabelText.implicitWidth + card.hostWindow.dp(16)
                Layout.preferredHeight: card.hostWindow.dp(22)
                radius: height / 2
                color: card.isCompleted
                       ? card.colors.successSoft
                       : card.isRunning
                         ? card.colors.accentSoft
                         : card.isFailed
                           ? card.colors.errorSoft
                           : card.isPartial || card.isQueued
                             ? card.colors.warningSoft
                             : card.colors.inputBackground

                RowLayout {
                    anchors.centerIn: parent
                    spacing: card.hostWindow.dp(5)

                    Rectangle {
                        Layout.preferredWidth: card.hostWindow.dp(6)
                        Layout.preferredHeight: card.hostWindow.dp(6)
                        radius: width / 2
                        color: card.statusColor
                    }

                    Text {
                        id: statusLabelText
                        text: card.statusLabel
                        color: card.statusColor
                        font.pixelSize: card.hostWindow.dp(10)
                        font.weight: Font.DemiBold
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: card.hostWindow.dp(76)
            radius: card.colors.cornerRadiusSmall
            color: card.colors.inputBackground
            border.width: 1
            border.color: card.colors.border
            clip: true

            Flickable {
                id: detailsFlickable
                anchors.fill: parent
                anchors.margins: card.hostWindow.dp(10)
                contentWidth: width
                contentHeight: detailsColumn.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                ScrollBar.vertical: ScrollBar {
                    id: detailsScrollBar
                    policy: ScrollBar.AsNeeded
                    contentItem: Rectangle {
                        implicitWidth: card.hostWindow.dp(4)
                        radius: width / 2
                        color: detailsScrollBar.pressed ? card.colors.accent : card.colors.textMuted
                        opacity: detailsScrollBar.active ? 0.72 : 0.28
                    }
                    background: Item { }
                }

                ColumnLayout {
                    id: detailsColumn
                    width: Math.max(0, detailsFlickable.width - detailsScrollBar.width
                                    - card.hostWindow.dp(4))
                    spacing: card.hostWindow.dp(10)

                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: card.content.length > 0
                              ? card.content
                              : card.isQueued
                                ? qsTr("Waiting for execution to start…")
                                : card.isRunning
                                  ? qsTr("Waiting for this provider to finish…")
                                  : qsTr("No response text.")
                        font.italic: card.content.length === 0
                        color: card.colors.textPrimary
                        font.pixelSize: card.hostWindow.dp(12)
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: card.errorText.length > 0
                        wrapMode: Text.WordWrap
                        text: card.errorText
                        color: card.colors.error
                        font.pixelSize: card.hostWindow.dp(11)
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: card.hostWindow.dp(3)
                        visible: card.strengths.length > 0

                        Text {
                            text: qsTr("Strengths")
                            color: card.colors.textSecondary
                            font.pixelSize: card.hostWindow.dp(10.5)
                            font.weight: Font.DemiBold
                        }

                        Repeater {
                            model: card.strengths
                            delegate: Text {
                                required property string modelData
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                text: qsTr("• %1").arg(modelData)
                                color: card.colors.textSecondary
                                font.pixelSize: card.hostWindow.dp(11)
                            }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: card.hostWindow.dp(3)
                        visible: card.warnings.length > 0

                        Text {
                            text: qsTr("Warnings")
                            color: card.colors.warning
                            font.pixelSize: card.hostWindow.dp(10.5)
                            font.weight: Font.DemiBold
                        }

                        Repeater {
                            model: card.warnings
                            delegate: Text {
                                required property string modelData
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                text: qsTr("• %1").arg(modelData)
                                color: card.colors.warning
                                font.pixelSize: card.hostWindow.dp(11)
                            }
                        }
                    }
                }
            }
        }

        ComparisonMetrics {
            Layout.fillWidth: true
            hostWindow: card.hostWindow
            colors: card.colors
            inputTokens: card.inputTokens
            outputTokens: card.outputTokens
            totalCostText: card.totalCostText
            latencyText: card.latencyText
            qualityAvailable: card.qualityAvailable
            qualityScore: card.qualityScore
            qualitySource: card.qualitySource
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: card.hostWindow.dp(6)

            ComparisonActionButton {
                iconGlyph: "\u29C9"
                accessibleLabel: qsTr("Copy response")
                hostWindow: card.hostWindow
                colors: card.colors
                enabled: card.content.length > 0
                onClicked: card.copyResponse()
            }

            ComparisonActionButton {
                iconGlyph: "\u21BB"
                accessibleLabel: qsTr("Retry this result")
                hostWindow: card.hostWindow
                colors: card.colors
                enabled: card.canRetry
                onClicked: card.retryRequested(card.resultId)
            }

            Item { Layout.fillWidth: true }

            ComparisonActionButton {
                text: card.compactActions ? qsTr("Synth.") : qsTr("Synthesis")
                toggled: card.selectedForSynthesis
                enabled: card.canSelectForSynthesis
                accessibleLabel: qsTr("Select for synthesis")
                hostWindow: card.hostWindow
                colors: card.colors
                onClicked: card.synthesisSelectionRequested(card.resultId,
                                                               !card.selectedForSynthesis)
            }

            ComparisonActionButton {
                text: card.isPreferred || card.compactActions ? qsTr("Best") : qsTr("Mark as best")
                primary: card.isPreferred
                enabled: card.isCompleted || card.isPartial
                accessibleLabel: qsTr("Mark as preferred result")
                hostWindow: card.hostWindow
                colors: card.colors
                onClicked: card.preferredRequested(card.resultId)
            }
        }
    }
}
