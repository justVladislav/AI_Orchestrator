pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: panel

    required property var hostWindow
    required property var colors
    required property var controller
    signal findingsRequested(var attempt)

    objectName: "manualHandoffAttemptHistory"
    Layout.fillWidth: true
    spacing: panel.hostWindow.dp(4)
    visible: panel.controller && panel.controller.hasHandoff
             && panel.controller.attempts.length > 0

    Text {
        Layout.fillWidth: true
        text: qsTranslate("ManualHandoffPanel", "Verification attempts")
        color: panel.colors.textPrimary
        font.pixelSize: panel.hostWindow.dp(9)
        font.weight: Font.DemiBold
    }
    Repeater {
        model: panel.controller ? panel.controller.attempts : []
        delegate: ColumnLayout {
            id: attemptRow
            required property var modelData
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(2)
            Text {
                Layout.fillWidth: true
                text: qsTranslate("ManualHandoffPanel", "Attempt %1 / %2 / %3%4")
                      .arg(attemptRow.modelData.attemptOrdinal)
                      .arg(attemptRow.modelData.attemptStatus)
                      .arg(attemptRow.modelData.resultKind.length > 0
                           ? attemptRow.modelData.resultKind : qsTranslate("ManualHandoffPanel", "result pending"))
                      .arg(attemptRow.modelData.isAuthoritative
                           ? qsTranslate("ManualHandoffPanel", " / authoritative") : "")
                color: attemptRow.modelData.isAuthoritative
                       ? panel.colors.success
                       : panel.statusColor(attemptRow.modelData.attemptStatus)
                wrapMode: Text.WordWrap
                font.pixelSize: panel.hostWindow.dp(8.5)
            }
            Text {
                Layout.fillWidth: true
                visible: attemptRow.modelData.resultKind === "rejected_before_verification"
                         && (attemptRow.modelData.rejectionCode.length > 0
                             || attemptRow.modelData.rejectionMessage.length > 0)
                text: attemptRow.modelData.rejectionCode + ": "
                      + attemptRow.modelData.rejectionMessage
                color: panel.colors.error
                wrapMode: Text.WordWrap
                font.pixelSize: panel.hostWindow.dp(8)
            }
            Text {
                Layout.fillWidth: true
                visible: attemptRow.modelData.hasVerification
                text: qsTranslate("ManualHandoffPanel", "Proposal safety: %1 • Build/lint/test: %2 • Acceptance: %3")
                      .arg(attemptRow.modelData.proposalSafetyStatus)
                      .arg(attemptRow.modelData.buildLintTestStatus)
                      .arg(attemptRow.modelData.acceptanceStatus)
                color: attemptRow.modelData.acceptanceStatus === "passed"
                       ? panel.colors.success : panel.colors.error
                wrapMode: Text.WordWrap
                font.pixelSize: panel.hostWindow.dp(8)
            }
            // Findings of an attempt: counts only, the Backend's own buckets. The findings themselves are in the window
            // (and, for the failed verification the handoff started from, in the shared RecordedFindingsBlock above), so the
            // same finding is never printed twice in small type.
            Text {
                objectName: "manualHandoffAttemptFindingSummary"
                Layout.fillWidth: true
                visible: panel.attemptFindingCount(attemptRow.modelData) > 0
                wrapMode: Text.WordWrap
                text: panel.attemptFindingSummary(attemptRow.modelData)
                color: (attemptRow.modelData.actionableDiagnostics || []).length
                       + (attemptRow.modelData.newActionableDiagnostics || []).length > 0
                       ? panel.colors.error : panel.colors.textSecondary
                font.pixelSize: panel.hostWindow.dp(9)
            }
            RunActionButton {
                objectName: "manualHandoffShowFindingsButton"
                hostWindow: panel.hostWindow
                colors: panel.colors
                visible: panel.attemptFindingCount(attemptRow.modelData) > 0
                text: qsTranslate("ManualHandoffPanel", "Show the findings of this attempt in a window")
                onClicked: panel.findingsRequested(attemptRow.modelData)
            }
        }
    }

    function statusColor(status) {
        if (status === "completed" || status === "passed") return panel.colors.success
        if (status === "failed" || status === "cancelled"
                || status === "interrupted") return panel.colors.error
        if (status === "prepared" || status === "running"
                || status === "queued") return panel.colors.warning
        return panel.colors.textSecondary
    }

    function attemptFindingCount(attempt) {
        return (attempt.actionableDiagnostics || []).length + (attempt.outsideScopeDiagnostics || []).length
            + (attempt.generalDiagnostics || []).length + (attempt.newActionableDiagnostics || []).length
            + (attempt.newOutsideManifestOrContextDiagnostics || []).length + (attempt.unchangedBaselineDebtDiagnostics || []).length
    }

    function attemptFindingSummary(attempt) {
        const parts = []
        const add = function (list, one, many) {
            const n = (list || []).length
            if (n > 0) parts.push(n === 1 ? one : many.arg(n))
        }
        add(attempt.actionableDiagnostics, qsTranslate("ManualHandoffPanel", "1 in the files you may edit"), qsTranslate("ManualHandoffPanel", "%1 in the files you may edit"))
        add(attempt.newActionableDiagnostics, qsTranslate("ManualHandoffPanel", "1 new to fix"), qsTranslate("ManualHandoffPanel", "%1 new to fix"))
        add(attempt.outsideScopeDiagnostics, qsTranslate("ManualHandoffPanel", "1 in a file you may NOT edit"), qsTranslate("ManualHandoffPanel", "%1 in files you may NOT edit"))
        add(attempt.newOutsideManifestOrContextDiagnostics, qsTranslate("ManualHandoffPanel", "1 new outside the Manifest's write scope"),
            qsTranslate("ManualHandoffPanel", "%1 new outside the Manifest's write scope"))
        add(attempt.generalDiagnostics, qsTranslate("ManualHandoffPanel", "1 general message"), qsTranslate("ManualHandoffPanel", "%1 general messages"))
        add(attempt.unchangedBaselineDebtDiagnostics, qsTranslate("ManualHandoffPanel", "1 older warning (not blocking)"), qsTranslate("ManualHandoffPanel", "%1 older warnings (not blocking)"))
        return qsTranslate("ManualHandoffPanel", "Findings: %1").arg(parts.join(" \u2022 "))
    }
}
