pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// C2-B/S2b-3: the result of ONE project-memory compaction, exactly as ProjectMemoryController reported it
// (compactionOutcomeReported -> projectMemoryOutcomeMap). This block only DISPLAYS that map: it computes no amount, classifies
// nothing again and parses no message. The operation, the fate of the received usage (saved / not saved / not confirmed), the
// estimate with its completeness, the storage health and the incomplete-diagnostics mark are always on the surface; the codes,
// the native SQLite codes and the ordered secondary causes are behind "Details" (they never push the main text out). It grows
// with its content: nothing is elided or clipped, long codes and Unicode wrap inside the block.
Rectangle {
    id: block
    objectName: "projectMemoryOperationOutcome"

    required property var hostWindow
    required property var colors
    // The map of one outcome, or null/empty when there is nothing to show.
    property var outcome: null
    property bool detailsExpanded: false

    readonly property bool hasOutcome: block.outcome !== null && block.outcome !== undefined
                                       && block.outcome.operationState !== undefined
    readonly property bool hasDetails: block.hasOutcome
    readonly property color stateColor: !block.hasOutcome ? block.colors.textMuted
        : block.outcome.operationState === "completed" ? block.colors.success
        : block.outcome.operationState === "cancelled" ? block.colors.warning
        : block.colors.error
    readonly property bool accountingNeedsAttention: block.hasOutcome
        && (block.outcome.accounting === "not_saved" || block.outcome.accounting === "unknown")
    // The Backend words the reason of a Failed/Cancelled record by its ORIGIN (stored, attempted, unconfirmed fallback); the sentence
    // for the main screen is empty when it adds nothing or when it only describes the unconfirmed fallback (that goes to the details).
    readonly property bool terminalShown: block.hasOutcome && block.outcome.terminalText.length > 0

    visible: block.hasOutcome
    implicitHeight: block.hasOutcome ? content.implicitHeight + block.hostWindow.dp(18) : 0
    radius: block.colors.cornerRadiusSmall
    color: block.colors.inputBackground
    border.width: 1
    border.color: !block.hasOutcome ? block.colors.border
        : block.outcome.operationState === "completed" && !block.accountingNeedsAttention
          && !block.outcome.connectionNeedsAttention ? block.colors.border : block.colors.warning

    onOutcomeChanged: block.detailsExpanded = false

    ColumnLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: block.hostWindow.dp(9)
        spacing: block.hostWindow.dp(4)

        Text {
            objectName: "projectMemoryOutcomeHeadline"
            Layout.fillWidth: true
            text: block.hasOutcome ? block.outcome.headline : ""
            color: block.stateColor
            wrapMode: Text.Wrap
            font.pixelSize: block.hostWindow.dp(11.5)
            font.weight: Font.DemiBold
        }

        Text {
            objectName: "projectMemoryOutcomePrimary"
            Layout.fillWidth: true
            visible: block.hasOutcome && block.outcome.primaryMessage.length > 0
            text: block.hasOutcome ? block.outcome.primaryMessage : ""
            color: block.colors.error
            wrapMode: Text.Wrap
            font.pixelSize: block.hostWindow.dp(10)
        }

        Text {
            objectName: "projectMemoryOutcomeTerminal"
            Layout.fillWidth: true
            visible: block.terminalShown
            text: block.terminalShown ? block.outcome.terminalText : ""
            color: block.colors.textSecondary
            wrapMode: Text.Wrap
            font.pixelSize: block.hostWindow.dp(9.5)
        }

        Text {
            objectName: "projectMemoryOutcomeAccounting"
            Layout.fillWidth: true
            visible: block.hasOutcome && block.outcome.accountingText.length > 0
            text: block.hasOutcome ? block.outcome.accountingText : ""
            color: block.accountingNeedsAttention ? block.colors.warning : block.colors.textSecondary
            wrapMode: Text.Wrap
            font.pixelSize: block.hostWindow.dp(10)
        }

        Text {
            objectName: "projectMemoryOutcomeCost"
            Layout.fillWidth: true
            visible: block.hasOutcome && block.outcome.costLine.length > 0
            text: block.hasOutcome ? block.outcome.costLine : ""
            color: block.hasOutcome && (block.outcome.observedCost.priceIncomplete || block.accountingNeedsAttention)
                   ? block.colors.warning : block.colors.textMuted
            wrapMode: Text.Wrap
            font.pixelSize: block.hostWindow.dp(9.5)
            font.italic: block.hasOutcome && block.outcome.observedCost.priceIncomplete
        }

        Text {
            objectName: "projectMemoryOutcomeTokens"
            Layout.fillWidth: true
            visible: block.hasOutcome && block.outcome.observedTokens.available
            text: block.hasOutcome && block.outcome.observedTokens.available
                  ? qsTr("%1 in / %2 out tok received").arg(block.outcome.observedTokens.inputTokens)
                        .arg(block.outcome.observedTokens.outputTokens) : ""
            color: block.colors.textMuted
            wrapMode: Text.Wrap
            font.pixelSize: block.hostWindow.dp(9)
        }

        Text {
            objectName: "projectMemoryOutcomeConnection"
            Layout.fillWidth: true
            visible: block.hasOutcome && block.outcome.connectionNeedsAttention
            text: block.hasOutcome ? block.outcome.connectionText : ""
            color: block.colors.error
            wrapMode: Text.Wrap
            font.pixelSize: block.hostWindow.dp(10)
        }

        Text {
            objectName: "projectMemoryOutcomeDiagnosticsIncomplete"
            Layout.fillWidth: true
            visible: block.hasOutcome && block.outcome.diagnosticsIncomplete
            text: block.hasOutcome ? block.outcome.diagnosticsIncompleteText : ""
            color: block.colors.warning
            wrapMode: Text.Wrap
            font.pixelSize: block.hostWindow.dp(9.5)
        }

        ProjectActionButton {
            objectName: "projectMemoryOutcomeDetailsToggle"
            visible: block.hasDetails
            text: block.detailsExpanded ? qsTr("Hide details") : qsTr("Details")
            hostWindow: block.hostWindow
            colors: block.colors
            onClicked: block.detailsExpanded = !block.detailsExpanded
        }

        ColumnLayout {
            objectName: "projectMemoryOutcomeDetails"
            Layout.fillWidth: true
            visible: block.hasDetails && block.detailsExpanded
            spacing: block.hostWindow.dp(3)

            Text {
                objectName: "projectMemoryOutcomePrimaryCode"
                Layout.fillWidth: true
                visible: block.hasOutcome && block.outcome.primaryCode.length > 0
                text: block.hasOutcome ? qsTr("Error code: %1").arg(block.outcome.primaryCode) : ""
                color: block.colors.textMuted
                wrapMode: Text.Wrap
                font.pixelSize: block.hostWindow.dp(9)
            }
            Text {
                objectName: "projectMemoryOutcomeTerminalCode"
                Layout.fillWidth: true
                visible: block.hasOutcome && block.outcome.terminalDetailText.length > 0
                text: block.hasOutcome ? block.outcome.terminalDetailText : ""
                color: block.colors.textMuted
                wrapMode: Text.Wrap
                font.pixelSize: block.hostWindow.dp(9)
            }
            Text {
                objectName: "projectMemoryOutcomeAccountingCode"
                Layout.fillWidth: true
                visible: block.hasOutcome && block.outcome.accountingCode.length > 0
                text: block.hasOutcome ? qsTr("Accounting: %1").arg(block.outcome.accountingCode) : ""
                color: block.colors.textMuted
                wrapMode: Text.Wrap
                font.pixelSize: block.hostWindow.dp(9)
            }
            Text {
                objectName: "projectMemoryOutcomeConnectionDetail"
                Layout.fillWidth: true
                visible: block.hasOutcome && !block.outcome.connectionNeedsAttention
                text: block.hasOutcome ? block.outcome.connectionText : ""
                color: block.colors.textMuted
                wrapMode: Text.Wrap
                font.pixelSize: block.hostWindow.dp(9)
            }
            Text {
                objectName: "projectMemoryOutcomePrimaryNative"
                Layout.fillWidth: true
                visible: block.hasOutcome && block.outcome.primaryNative.present
                text: block.hasOutcome && block.outcome.primaryNative.present
                      ? qsTr("Write: %1").arg(block.outcome.primaryNative.text) : ""
                color: block.colors.textMuted
                wrapMode: Text.Wrap
                font.pixelSize: block.hostWindow.dp(9)
            }
            Text {
                objectName: "projectMemoryOutcomeFallbackNative"
                Layout.fillWidth: true
                visible: block.hasOutcome && block.outcome.fallbackNative.present
                text: block.hasOutcome && block.outcome.fallbackNative.present
                      ? qsTr("Fallback write: %1").arg(block.outcome.fallbackNative.text) : ""
                color: block.colors.textMuted
                wrapMode: Text.Wrap
                font.pixelSize: block.hostWindow.dp(9)
            }
            Repeater {
                model: block.hasOutcome ? block.outcome.diagnostics : []
                delegate: Text {
                    id: diagnosticLine
                    required property var modelData
                    objectName: "projectMemoryOutcomeDiagnostic"
                    Layout.fillWidth: true
                    text: qsTr("%1: %2 (%3)").arg(diagnosticLine.modelData.stageText)
                              .arg(diagnosticLine.modelData.message).arg(diagnosticLine.modelData.code)
                    color: block.colors.textMuted
                    wrapMode: Text.Wrap
                    font.pixelSize: block.hostWindow.dp(9)
                }
            }
        }
    }
}
