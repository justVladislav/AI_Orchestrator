pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Session/provider/token/cost summary for the active conversation. Used
// embedded as the third column in wide layout, and reused (with
// showCloseButton: true) inside a Popup for the medium-width "Session
// details" overlay, so the panel itself stays layout-agnostic.
Rectangle {
    id: panel

    required property var hostWindow
    required property var colors

    // sessionData: { providerName, modelName, connectionState, connectionLabel,
    //                messageCount, inputTokens, outputTokens, totalTokens,
    //                estimatedCost, contextSummary, lastResponseTime }
    property var sessionData: null
    property bool showCloseButton: false
    // PC-1D: backend.projectContext and the conversation's project id (empty without a project).
    property var projectContext: null
    property string projectId: ""
    // Fingerprint of the snapshot armed for the next message, or empty (PC-3E).
    property string attachedFingerprint: ""

    signal clearContextRequested()
    signal closeRequested()
    signal projectContextPreviewRequested(bool includeProjectMemory)
    signal projectContextClearRequested()
    signal projectContextAttachRequested()

    radius: colors.cornerRadiusMedium
    color: colors.surfaceAlternative
    border.width: 1
    border.color: colors.border

    function formatTokens(value) {
        return value === undefined || value === null ? "0" : String(value)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: panel.hostWindow.dp(16)
        spacing: panel.hostWindow.dp(12)

        RowLayout {
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(8)

            Text {
                Layout.fillWidth: true
                text: qsTr("Session")
                color: panel.colors.textPrimary
                font.pixelSize: panel.hostWindow.dp(15)
                font.weight: Font.DemiBold
            }

            ChatIconButton {
                visible: panel.showCloseButton
                hostWindow: panel.hostWindow
                colors: panel.colors
                iconGlyph: "\u2715"
                accessibleLabel: qsTr("Close session details")
                onClicked: panel.closeRequested()
            }
        }

        Flickable {
            id: sessionFlickable
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: sessionColumn.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            ScrollBar.vertical: ScrollBar { id: sessionScrollBar }

            ColumnLayout {
                id: sessionColumn
                width: Math.max(0, sessionFlickable.width - sessionScrollBar.width
                                - panel.hostWindow.dp(4))
                spacing: panel.hostWindow.dp(14)

                RowLayout {
                    Layout.fillWidth: true
                    spacing: panel.hostWindow.dp(8)

                    Rectangle {
                        Layout.preferredWidth: panel.hostWindow.dp(8)
                        Layout.preferredHeight: panel.hostWindow.dp(8)
                        radius: width / 2
                        color: panel.sessionData && panel.sessionData.connectionState === "connected"
                               ? panel.colors.success
                               : panel.sessionData && panel.sessionData.connectionState === "unavailable"
                                 ? panel.colors.error
                                 : panel.colors.textMuted
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: panel.hostWindow.dp(1)

                        Text {
                            Layout.fillWidth: true
                            text: panel.sessionData
                                  ? panel.sessionData.providerName + " · " + panel.sessionData.modelName
                                  : qsTr("No conversation selected")
                            color: panel.colors.textPrimary
                            elide: Text.ElideRight
                            font.pixelSize: panel.hostWindow.dp(13)
                            font.weight: Font.Medium
                        }

                        Text {
                            Layout.fillWidth: true
                            text: panel.sessionData ? panel.sessionData.connectionLabel : ""
                            color: panel.colors.textMuted
                            elide: Text.ElideRight
                            font.pixelSize: panel.hostWindow.dp(10.5)
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: panel.sessionData
                          ? qsTr("Project context: %1").arg(panel.sessionData.projectName)
                          : qsTr("No project context")
                    color: panel.colors.textSecondary
                    elide: Text.ElideRight
                    font.pixelSize: panel.hostWindow.dp(10.5)
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: tokenGrid.implicitHeight + panel.hostWindow.dp(20)
                    radius: panel.colors.cornerRadiusSmall
                    color: panel.colors.inputBackground
                    border.width: 1
                    border.color: panel.colors.border

                    GridLayout {
                        id: tokenGrid
                        anchors.fill: parent
                        anchors.margins: panel.hostWindow.dp(11)
                        columns: 2
                        columnSpacing: panel.hostWindow.dp(14)
                        rowSpacing: panel.hostWindow.dp(8)

                        Text {
                            text: qsTr("Messages")
                            color: panel.colors.textMuted
                            font.pixelSize: panel.hostWindow.dp(10)
                        }
                        Text {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignRight
                            text: panel.sessionData ? panel.formatTokens(panel.sessionData.messageCount) : "0"
                            color: panel.colors.textPrimary
                            font.pixelSize: panel.hostWindow.dp(11.5)
                        }

                        Text {
                            text: qsTr("Input tokens")
                            color: panel.colors.textMuted
                            font.pixelSize: panel.hostWindow.dp(10)
                        }
                        Text {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignRight
                            text: panel.sessionData ? panel.formatTokens(panel.sessionData.inputTokens) : "0"
                            color: panel.colors.textPrimary
                            font.pixelSize: panel.hostWindow.dp(11.5)
                        }

                        Text {
                            text: qsTr("Output tokens")
                            color: panel.colors.textMuted
                            font.pixelSize: panel.hostWindow.dp(10)
                        }
                        Text {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignRight
                            text: panel.sessionData ? panel.formatTokens(panel.sessionData.outputTokens) : "0"
                            color: panel.colors.textPrimary
                            font.pixelSize: panel.hostWindow.dp(11.5)
                        }

                        Text {
                            text: qsTr("Total tokens")
                            color: panel.colors.textSecondary
                            font.pixelSize: panel.hostWindow.dp(10)
                            font.weight: Font.DemiBold
                        }
                        Text {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignRight
                            text: panel.sessionData ? panel.formatTokens(panel.sessionData.totalTokens) : "0"
                            color: panel.colors.textPrimary
                            font.pixelSize: panel.hostWindow.dp(11.5)
                            font.weight: Font.DemiBold
                        }

                        Text {
                            text: qsTr("Estimated cost")
                            color: panel.colors.textMuted
                            font.pixelSize: panel.hostWindow.dp(10)
                        }
                        Text {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignRight
                            text: panel.sessionData ? panel.sessionData.estimatedCost : "$0.00"
                            color: panel.colors.textPrimary
                            font.pixelSize: panel.hostWindow.dp(11.5)
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: panel.sessionData && panel.sessionData.modelMetrics.length > 0
                    text: qsTr("Usage by model")
                    color: panel.colors.textPrimary
                    font.pixelSize: panel.hostWindow.dp(12.5)
                    font.weight: Font.DemiBold
                }

                Repeater {
                    model: panel.sessionData ? panel.sessionData.modelMetrics : []

                    delegate: Rectangle {
                        id: modelUsageDelegate
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredHeight: modelUsageColumn.implicitHeight + panel.hostWindow.dp(18)
                        radius: panel.colors.cornerRadiusSmall
                        color: panel.colors.inputBackground
                        border.width: 1
                        border.color: panel.colors.border

                        ColumnLayout {
                            id: modelUsageColumn
                            anchors.fill: parent
                            anchors.margins: panel.hostWindow.dp(9)
                            spacing: panel.hostWindow.dp(4)

                            Text {
                                Layout.fillWidth: true
                                text: modelUsageDelegate.modelData.providerName + " · " + modelUsageDelegate.modelData.modelName
                                color: panel.colors.textPrimary
                                elide: Text.ElideRight
                                font.pixelSize: panel.hostWindow.dp(11)
                                font.weight: Font.DemiBold
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("%1 responses · %2 tokens · %3 · avg %4")
                                    .arg(modelUsageDelegate.modelData.responseCount).arg(modelUsageDelegate.modelData.totalTokens)
                                    .arg(modelUsageDelegate.modelData.estimatedCost).arg(modelUsageDelegate.modelData.averageLatency)
                                color: panel.colors.textMuted
                                elide: Text.ElideRight
                                font.pixelSize: panel.hostWindow.dp(9.5)
                            }
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Context")
                    color: panel.colors.textPrimary
                    font.pixelSize: panel.hostWindow.dp(12.5)
                    font.weight: Font.DemiBold
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: contextText.implicitHeight + panel.hostWindow.dp(20)
                    radius: panel.colors.cornerRadiusSmall
                    color: panel.colors.inputBackground
                    border.width: 1
                    border.color: panel.colors.border

                    Text {
                        id: contextText
                        anchors.fill: parent
                        anchors.margins: panel.hostWindow.dp(11)
                        text: panel.sessionData && panel.sessionData.contextSummary.length > 0
                              ? panel.sessionData.contextSummary
                              : qsTr("No context attached.")
                        color: panel.colors.textSecondary
                        wrapMode: Text.WordWrap
                        font.pixelSize: panel.hostWindow.dp(11)
                    }
                }

                ChatProjectContextPreview {
                    Layout.fillWidth: true
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    controller: panel.projectContext
                    projectId: panel.projectId
                    attachedFingerprint: panel.attachedFingerprint

                    onPreviewRequested: includeProjectMemory => panel.projectContextPreviewRequested(includeProjectMemory)
                    onClearRequested: panel.projectContextClearRequested()
                    onAttachRequested: panel.projectContextAttachRequested()
                }

                Text {
                    Layout.fillWidth: true
                    visible: panel.sessionData && panel.sessionData.lastResponseTime.length > 0
                    text: panel.sessionData
                          ? qsTr("Last response %1").arg(panel.sessionData.lastResponseTime)
                          : ""
                    color: panel.colors.textMuted
                    font.pixelSize: panel.hostWindow.dp(10.5)
                }

                ChatActionButton {
                    Layout.fillWidth: true
                    text: qsTr("Clear context")
                    enabled: panel.sessionData !== null
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    onClicked: panel.clearContextRequested()
                }
            }
        }
    }
}
