pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: panel

    required property var hostWindow
    required property var colors
    required property var runData
    required property string providerBudgetStopCode
    required property bool providerUsageUnavailable

    objectName: "providerBudgetPanel"
    Layout.fillWidth: true
    Layout.preferredHeight: providerBudgetColumn.implicitHeight + panel.hostWindow.dp(20)
    radius: panel.colors.cornerRadiusSmall
    color: panel.colors.surface
    border.width: 1
    border.color: panel.providerBudgetStopCode.length > 0
                  ? panel.colors.error : panel.colors.border

    ColumnLayout {
        id: providerBudgetColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: panel.hostWindow.dp(11)
        anchors.rightMargin: panel.hostWindow.dp(11)
        spacing: panel.hostWindow.dp(5)

        Text {
            objectName: "providerBudgetUsageText"
            Layout.fillWidth: true
            text: panel.runData
                  ? qsTranslate("RunDetailsPanel", "Input %1 / %2 · output %3 / %4 · turns %5 / %6")
                    .arg(panel.budgetNumber(panel.runData.providerBudgetInputTokens))
                    .arg(panel.budgetNumber(panel.runData.providerBudgetMaxInputTokens))
                    .arg(panel.budgetNumber(panel.runData.providerBudgetOutputTokens))
                    .arg(panel.budgetNumber(panel.runData.providerBudgetMaxOutputTokens))
                    .arg(panel.budgetNumber(panel.runData.providerTurnsStarted))
                    .arg(panel.budgetNumber(panel.runData.providerBudgetMaxTurns))
                    + (Number(panel.runData.providerBudgetTurnsRaisedFrom || 0) > 0
                       ? qsTranslate("RunDetailsPanel", " (raised automatically from %1)")
                         .arg(panel.budgetNumber(panel.runData.providerBudgetTurnsRaisedFrom))
                       : "")
                  : ""
            color: panel.providerBudgetStopCode.length > 0
                   ? panel.colors.error : panel.colors.textPrimary
            font.pixelSize: panel.hostWindow.dp(9.5)
            wrapMode: Text.WordWrap
        }
        Text {
            Layout.fillWidth: true
            text: panel.runData
                  ? qsTranslate("RunDetailsPanel", "Tool calls %1 / %2 · returned %3 / %4 bytes · request timeout %5 s · Run limit %6 min")
                    .arg(panel.budgetNumber(panel.runData.providerToolCalls))
                    .arg(panel.budgetNumber(panel.runData.providerBudgetMaxToolCalls))
                    .arg(panel.budgetNumber(panel.runData.providerToolResultBytes))
                    .arg(panel.budgetNumber(panel.runData.providerBudgetMaxToolResultBytes))
                    .arg(panel.budgetNumber(panel.runData.providerBudgetMaxRequestTimeMs / 1000))
                    .arg(panel.budgetNumber(panel.runData.providerBudgetMaxWallTimeMs / 60000))
                  : ""
            color: panel.colors.textSecondary
            font.pixelSize: panel.hostWindow.dp(8.5)
            wrapMode: Text.WordWrap
        }
        Text {
            objectName: "providerCacheUsageText"
            Layout.fillWidth: true
            visible: panel.runData
                     && (Number(panel.runData.cacheCreationInputTokens) > 0
                         || Number(panel.runData.cacheReadInputTokens) > 0)
            text: panel.runData
                  ? qsTranslate("RunDetailsPanel", "Prompt cache: %1 written · %2 read (read tokens do not count toward the input limit above)")
                    .arg(panel.budgetNumber(panel.runData.cacheCreationInputTokens))
                    .arg(panel.budgetNumber(panel.runData.cacheReadInputTokens))
                  : ""
            color: panel.colors.textSecondary
            font.pixelSize: panel.hostWindow.dp(8.5)
            wrapMode: Text.WordWrap
        }
        Text {
            // Package 4 (2026-09-28 planner-terminal-budget-hardening):
            // a real Anthropic usage export showed most of a Run's
            // "raw input" total is steeply-discounted cache-read
            // tokens, not fresh billable work - this line renders
            // all five Backend-projected categories explicitly so
            // none of them can be mistaken for another. Every
            // number here is a direct Backend read-model field;
            // QML performs no subtraction or classification.
            objectName: "providerUsageBreakdownText"
            Layout.fillWidth: true
            visible: panel.runData
                     && (Number(panel.runData.cacheCreationInputTokens) > 0
                         || Number(panel.runData.cacheReadInputTokens) > 0)
            text: panel.runData
                  ? qsTranslate("RunDetailsPanel", "Raw input %1 = non-cache input %2 + cache write %3 + cache read %4 · output %5")
                    .arg(panel.budgetNumber(panel.runData.inputTokens))
                    .arg(panel.budgetNumber(panel.runData.nonCacheInputTokens))
                    .arg(panel.budgetNumber(panel.runData.cacheCreationInputTokens))
                    .arg(panel.budgetNumber(panel.runData.cacheReadInputTokens))
                    .arg(panel.budgetNumber(panel.runData.outputTokens))
                  : ""
            color: panel.colors.textSecondary
            font.pixelSize: panel.hostWindow.dp(8.5)
            wrapMode: Text.WordWrap
        }
        Text {
            objectName: "providerBudgetStopReason"
            Layout.fillWidth: true
            visible: panel.providerUsageUnavailable
                     || panel.providerBudgetStopCode.length > 0
            text: panel.runData
                  ? (panel.providerBudgetStopCode.length > 0
                     ? qsTranslate("RunDetailsPanel", "Stopped by Backend: %1").arg(panel.providerBudgetStopCode)
                     : qsTranslate("RunDetailsPanel", "Provider usage is unavailable; execution is fail-closed."))
                  : ""
            color: panel.colors.error
            font.pixelSize: panel.hostWindow.dp(8.5)
            wrapMode: Text.WordWrap
        }
        Text {
            Layout.fillWidth: true
            text: qsTranslate("RunDetailsPanel", "Cost enforcement is unavailable until a trusted versioned pricing snapshot is present; token limits remain authoritative.")
            color: panel.colors.textMuted
            font.pixelSize: panel.hostWindow.dp(8)
            wrapMode: Text.WordWrap
        }
    }

    function budgetNumber(value) {
        return Number(value || 0).toLocaleString(Qt.locale(), "f", 0)
    }
}
