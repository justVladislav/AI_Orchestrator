pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// QML-P4R.5 package 3: renderer and explicit command surface for the
// Backend-owned manual verification handoff. Every decision displayed here
// comes directly from ManifestVerificationManualHandoffController. This file
// never reconstructs lineage, verification authority, or Apply eligibility,
// never receives an absolute candidate path, and never starts work from a
// refresh, timer, or filesystem event.
Rectangle {
    id: panel
    objectName: "manualHandoffPanel"

    required property var hostWindow
    required property var colors
    required property var controller
    required property var runsController
    property string runId: ""
    property bool confirmingDiscard: false
    // Provider-free reset hardening: local confirmation state for "Reset
    // non-editable changes" - mirrors confirmingDiscard exactly. Reset
    // itself is never triggered directly from the action button.
    property bool confirmingReset: false

    // Post-P4R.6 human review/Apply hardening: panel-local UI state for the
    // "Verified proposals" review section below - mirrors AgentSessionPanel's
    // own diffChangeId/originalText/proposedText/confirm*DialogOpen fields
    // exactly. Nothing here is authorization; every decision still comes
    // from panel.controller (selectedMemberDetail.applyAllowed, never
    // recomputed here).
    property string diffChangeId: ""
    property string originalText: ""
    property string proposedText: ""
    property bool confirmApplyDialogOpen: false
    property bool confirmRejectDialogOpen: false

    readonly property string projectedHandoffId:
        panel.controller && panel.controller.hasHandoff
        ? panel.controller.handoffId : ""

    onRunIdChanged: {
        confirmingDiscard = false
        confirmingReset = false
        panel.clearMemberSelection()
    }
    onProjectedHandoffIdChanged: {
        confirmingDiscard = false
        confirmingReset = false
    }

    function clearMemberSelection() {
        diffChangeId = ""
        originalText = ""
        proposedText = ""
        confirmApplyDialogOpen = false
        confirmRejectDialogOpen = false
        if (panel.controller) panel.controller.selectMember("")
    }

    function selectMemberProposal(proposalId) {
        diffChangeId = ""
        originalText = ""
        proposedText = ""
        panel.controller.selectMember(
            panel.controller.selectedMemberProposalId === proposalId ? "" : proposalId)
    }

    visible: panel.controller !== null && panel.controller.isApplicable
    Layout.fillWidth: true
    Layout.preferredHeight: contentColumn.implicitHeight + panel.hostWindow.dp(24)
    radius: panel.colors.cornerRadiusMedium
    color: panel.colors.surface
    border.width: 1
    border.color: panel.colors.warning

    function candidateHealthColor(health) {
        if (health === "unchanged") return panel.colors.success
        if (health === "edited") return panel.colors.warning
        if (health === "missing" || health === "unreadable"
                || health === "storage_mismatch") return panel.colors.error
        return panel.colors.textSecondary
    }

    Connections {
        target: panel.runsController
        function onRunUpdated(runId) {
            // Eligibility can change when the selected Run becomes terminal
            // without changing selection. Re-read Backend authority only;
            // refresh() never creates or verifies a handoff.
            if (runId === panel.runId && panel.controller
                    && !panel.controller.busy)
                panel.controller.refresh()
        }
    }

    Connections {
        target: panel.controller
        function onCommandFinished(commandRunId, succeeded) {
            // The controller has already refreshed its durable read model.
            // Refresh only the surrounding Run list/status projection; never
            // launch another handoff command from a completion notification.
            if (commandRunId === panel.runId && panel.runsController)
                panel.runsController.refresh()
        }
    }

    ColumnLayout {
        id: contentColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: panel.hostWindow.dp(12)
        anchors.rightMargin: panel.hostWindow.dp(12)
        spacing: panel.hostWindow.dp(7)

        RowLayout {
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(8)
            Text {
                Layout.fillWidth: true
                text: qsTr("Manual verification handoff")
                color: panel.colors.textPrimary
                font.pixelSize: panel.hostWindow.dp(12)
                font.weight: Font.DemiBold
            }
            Text {
                visible: panel.controller && panel.controller.hasHandoff
                text: panel.controller ? panel.controller.candidateHealth : ""
                color: panel.controller
                       ? panel.candidateHealthColor(panel.controller.candidateHealth)
                       : panel.colors.textSecondary
                font.pixelSize: panel.hostWindow.dp(9)
                font.weight: Font.DemiBold
            }
        }

        Text {
            Layout.fillWidth: true
            text: qsTr("Create an isolated repair workspace, edit only the allowed relative "
                       + "paths, then run the frozen verification profile again. This path is "
                       + "provider-free but may run local build, lint, or test commands. It "
                       + "never edits the live project automatically.")
            color: panel.colors.textSecondary
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(9)
        }

        Text {
            objectName: "manualHandoffLineage"
            Layout.fillWidth: true
            visible: panel.controller && panel.controller.hasHandoff
            text: panel.controller
                  ? qsTr("Verification anchor: %1 (%2)\nOrigin Manifest Run: %3 (%4)")
                        .arg(panel.controller.verificationAnchorRunId)
                        .arg(panel.controller.verificationAnchorStatus)
                        .arg(panel.controller.originManifestRunId)
                        .arg(panel.controller.originManifestStatus)
                  : ""
            color: panel.colors.textSecondary
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(8.5)
        }

        Text {
            objectName: "manualHandoffCreateDenial"
            Layout.fillWidth: true
            visible: panel.controller && !panel.controller.hasHandoff
                     && !panel.controller.canCreate
                     && (panel.controller.createReasonCode.length > 0
                         || panel.controller.createReasonMessage.length > 0)
            text: panel.controller
                  ? panel.controller.createReasonCode + ": "
                    + panel.controller.createReasonMessage
                  : ""
            color: panel.colors.error
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(9)
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(8)
            visible: panel.controller && !panel.controller.hasHandoff
            RunActionButton {
                objectName: "manualHandoffCreateButton"
                hostWindow: panel.hostWindow
                colors: panel.colors
                text: panel.controller && panel.controller.busy
                      ? qsTr("Creating...") : qsTr("Create repair workspace")
                primary: true
                enabled: panel.controller && panel.controller.canCreate
                         && !panel.controller.busy
                onClicked: panel.controller.create()
            }
            Item { Layout.fillWidth: true }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(5)
            visible: panel.controller && panel.controller.hasHandoff

            Text {
                objectName: "manualHandoffAllowedPathsHeading"
                Layout.fillWidth: true
                text: qsTr("Editable relative paths (only)")
                color: panel.colors.textPrimary
                font.pixelSize: panel.hostWindow.dp(9)
                font.weight: Font.DemiBold
            }
            Text {
                objectName: "manualHandoffEditableScopeNotice"
                Layout.fillWidth: true
                text: qsTr("Edit only the paths listed below. Module-wide lint may also report pre-existing warnings for context; those files are not authorized for this handoff and must remain unchanged.")
                color: panel.colors.warning
                wrapMode: Text.WordWrap
                font.pixelSize: panel.hostWindow.dp(8.5)
            }
            Repeater {
                model: panel.controller ? panel.controller.allowedRelativePaths : []
                delegate: Text {
                    required property string modelData
                    Layout.fillWidth: true
                    text: "• " + modelData
                    color: panel.colors.textSecondary
                    wrapMode: Text.WrapAnywhere
                    font.pixelSize: panel.hostWindow.dp(8.5)
                }
            }

            // What the Run itself failed on is shown by the shared RecordedFindingsBlock above both repair panels (the old
            // editable-scope-only list that stood here would only repeat it). The controller property initialScopeDiagnostics stays
            // for compatibility.
            RowLayout {
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(7)
                RunActionButton {
                    objectName: "manualHandoffOpenButton"
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    text: qsTr("Open repair workspace")
                    enabled: panel.controller && !panel.controller.busy
                    onClicked: panel.controller.openCandidateLocation()
                }
                RunActionButton {
                    objectName: "manualHandoffCopyPathButton"
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    text: qsTr("Copy location")
                    enabled: panel.controller && !panel.controller.busy
                    onClicked: panel.controller.copyCandidatePathToClipboard()
                }
                Item { Layout.fillWidth: true }
            }

            // ── Candidate changes: editable/reset-required split, Refresh, ──
            // ── and explicit Backend-owned Reset ─────────────────────────
            // Provider-free reset hardening. Both lists below are a direct,
            // unmodified projection of the Backend's own
            // inspectCandidateEdits() classification - this file never
            // derives the diff itself.
            ManualHandoffCandidateChanges {
                hostWindow: panel.hostWindow
                colors: panel.colors
                controller: panel.controller
                confirmingReset: panel.confirmingReset
                onRefreshRequested: panel.controller.refresh()
                onResetConfirmationRequested: confirming => panel.confirmingReset = confirming
                onResetRequested: {
                    panel.confirmingReset = false
                    panel.controller.resetNonEditableChanges()
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(7)
                visible: !panel.confirmingDiscard
                RunActionButton {
                    objectName: "manualHandoffReverifyButton"
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    text: panel.controller && panel.controller.busy
                          ? qsTr("Verifying...") : qsTr("Reverify edits (provider-free)")
                    primary: true
                    enabled: panel.controller
                             && !panel.controller.busy
                             && panel.controller.resetRequiredRelativePaths.length === 0
                    onClicked: panel.controller.reverify()
                }
                RunActionButton {
                    objectName: "manualHandoffDiscardButton"
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    text: qsTr("Discard workspace")
                    danger: true
                    enabled: panel.controller && !panel.controller.busy
                    onClicked: panel.confirmingDiscard = true
                }
                Item { Layout.fillWidth: true }
            }

            Text {
                Layout.fillWidth: true
                visible: panel.confirmingDiscard
                text: qsTr("Discard this retained repair workspace and its attempt history? "
                           + "This does not modify the live project, but the retained handoff "
                           + "cannot be recovered.")
                color: panel.colors.warning
                wrapMode: Text.WordWrap
                font.pixelSize: panel.hostWindow.dp(9)
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(7)
                visible: panel.confirmingDiscard
                RunActionButton {
                    objectName: "manualHandoffConfirmDiscardButton"
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    text: qsTr("Confirm discard")
                    danger: true
                    enabled: panel.controller && !panel.controller.busy
                    onClicked: {
                        panel.confirmingDiscard = false
                        panel.controller.discard()
                    }
                }
                RunActionButton {
                    objectName: "manualHandoffCancelDiscardButton"
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    text: qsTr("Keep workspace")
                    enabled: panel.controller && !panel.controller.busy
                    onClicked: panel.confirmingDiscard = false
                }
                Item { Layout.fillWidth: true }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(8)
            visible: panel.controller && panel.controller.busy
            Text {
                objectName: "manualHandoffBusyText"
                Layout.fillWidth: true
                text: panel.controller
                      ? qsTr("Provider-free operation running for Run %1")
                            .arg(panel.controller.activeCommandRunId)
                      : ""
                color: panel.colors.information
                wrapMode: Text.WordWrap
                font.pixelSize: panel.hostWindow.dp(9)
            }
            RunActionButton {
                objectName: "manualHandoffCancelButton"
                hostWindow: panel.hostWindow
                colors: panel.colors
                text: qsTr("Cancel operation")
                visible: panel.controller && panel.controller.canCancel
                enabled: visible
                onClicked: panel.controller.cancel()
            }
        }

        Text {
            objectName: "manualHandoffErrorText"
            Layout.fillWidth: true
            visible: panel.controller && panel.controller.errorText.length > 0
            text: panel.controller ? panel.controller.errorText : ""
            color: panel.colors.error
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(9)
        }

        Text {
            objectName: "manualHandoffApplyResultText"
            Layout.fillWidth: true
            visible: panel.controller && panel.controller.applyResultText.length > 0
            text: panel.controller ? panel.controller.applyResultText : ""
            color: panel.colors.information
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(9)
        }

        ManualHandoffAttemptHistory {
            hostWindow: panel.hostWindow
            colors: panel.colors
            controller: panel.controller
            onFindingsRequested: attempt => panel.showAttemptFindings(attempt)
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(3)
            visible: panel.controller && panel.controller.hasFinalVerification

            Text {
                Layout.fillWidth: true
                text: qsTr("Authoritative final verification")
                color: panel.colors.textPrimary
                font.pixelSize: panel.hostWindow.dp(9)
                font.weight: Font.DemiBold
            }
            Text {
                Layout.fillWidth: true
                text: panel.controller
                      ? qsTr("Proposal safety: %1 • Build/lint/test: %2 • Acceptance: %3")
                            .arg(panel.controller.finalProposalSafetyStatus)
                            .arg(panel.controller.finalBuildLintTestStatus)
                            .arg(panel.controller.finalAcceptanceStatus)
                      : ""
                color: panel.colors.textSecondary
                wrapMode: Text.WordWrap
                font.pixelSize: panel.hostWindow.dp(8.5)
            }
        }

        Text {
            objectName: "manualHandoffApplyStatus"
            Layout.fillWidth: true
            visible: panel.controller && panel.controller.hasHandoff
            text: panel.controller && panel.controller.canApply
                  ? qsTr("Apply is authorized for at least one verified member below.")
                  : (panel.controller
                     ? panel.controller.applyReasonCode + ": "
                       + panel.controller.applyReasonMessage
                     : "")
            color: panel.controller && panel.controller.canApply
                   ? panel.colors.success : panel.colors.textMuted
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(9)
        }

        // ── Verified proposals: exact-member human review/Apply ─────────
        // Post-P4R.6 human review/Apply hardening. Every field rendered
        // below is a direct Backend projection
        // (ManifestVerificationManualHandoffReadModelService's own
        // reviewMembers, reusing the SAME ChangeProposalService/
        // ManifestApplyVerificationGate as AgentSessionPanel) - this file
        // never computes AG-07/AG-16 authority, lineage, or eligibility
        // itself, and never synthesizes an agent-tool session.
        ManualHandoffReviewSection {
            hostWindow: panel.hostWindow
            colors: panel.colors
            controller: panel.controller
            diffChangeId: panel.diffChangeId
            originalText: panel.originalText
            proposedText: panel.proposedText
            confirmApplyDialogOpen: panel.confirmApplyDialogOpen
            confirmRejectDialogOpen: panel.confirmRejectDialogOpen
            onMemberSelectionRequested: proposalId => panel.selectMemberProposal(proposalId)
            onDiffToggleRequested: changeId => {
                if (panel.diffChangeId === changeId) {
                    panel.diffChangeId = ""
                    return
                }
                panel.diffChangeId = changeId
                panel.originalText = panel.controller.readOriginalContent(
                    panel.controller.selectedMemberProposalId,
                    changeId)
                panel.proposedText = panel.controller.readProposedContent(
                    panel.controller.selectedMemberProposalId,
                    changeId)
            }
            onLargeDiffRequested: relativePath => {
                reviewDiffDialog.fileLabel = relativePath
                reviewDiffDialog.open()
            }
            onApplyRequested: {
                panel.confirmApplyDialogOpen = true
                reviewApplyDialog.open()
            }
            onRejectRequested: {
                reviewRejectReasonField.text = ""
                panel.confirmRejectDialogOpen = true
                reviewRejectDialog.open()
            }
        }
    }

    // Both confirmation dialogs are deliberately direct children of the root
    // Rectangle (panel), never of contentColumn or the review section
    // (ColumnLayouts) - a Dialog/Popup is rendered through the window's own
    // Overlay regardless of where it sits in the QML tree, but qmllint's
    // Quick.layout-positioning rule still flags any Item child of a Layout
    // that sets explicit anchors/width, which centering/sizing a modal
    // confirmation genuinely needs. Moving them here (panel is a plain
    // Rectangle, not a Layout) avoids that false-positive without changing
    // any visible behavior - same convention as AgentSessionPanel.qml's own
    // dialogs would use if AgentSessionPanel's own root were not itself a
    // ColumnLayout.

    function showAttemptFindings(attempt) {
        findingsDialog.heading = qsTr("attempt %1").arg(attempt.attemptOrdinal)
        findingsDialog.editablePaths = panel.controller ? panel.controller.allowedRelativePaths : []
        findingsDialog.editableFindings = attempt.actionableDiagnostics || []
        findingsDialog.outsideScopeFindings = attempt.outsideScopeDiagnostics || []
        findingsDialog.newActionableFindings = attempt.newActionableDiagnostics || []
        findingsDialog.newOutsideFindings = attempt.newOutsideManifestOrContextDiagnostics || []
        findingsDialog.generalFindings = attempt.generalDiagnostics || []
        findingsDialog.baselineDebtFindings = attempt.unchangedBaselineDebtDiagnostics || []
        findingsDialog.open()
    }

    ManualHandoffFindingsDialog {
        id: findingsDialog
        hostWindow: panel.hostWindow
        colors: panel.colors
    }

    DiffViewDialog {
        id: reviewDiffDialog
        hostWindow: panel.hostWindow
        colors: panel.colors
        originalText: panel.originalText
        proposedText: panel.proposedText
    }

    // Approve-and-apply confirmation - same modal convention as
    // AgentSessionPanel.qml's own approveAndApplyDialog.
    Dialog {
        id: reviewApplyDialog
        anchors.centerIn: parent
        width: panel.hostWindow.dp(430)
        modal: true
        title: qsTr("Approve and apply")
        standardButtons: Dialog.NoButton
        onClosed: panel.confirmApplyDialogOpen = false

        background: Rectangle {
            radius: panel.colors.cornerRadiusMedium
            color: panel.colors.popupBackground
            border.width: 1
            border.color: panel.colors.border
        }

        contentItem: ColumnLayout {
            spacing: panel.hostWindow.dp(12)

            Text {
                Layout.fillWidth: true
                text: {
                    const detail = panel.controller ? panel.controller.selectedMemberDetail : {}
                    const changes = (detail && detail.changes) || []
                    const findings = (detail && detail.findings) || []
                    return qsTr("%1 file(s) · %2 finding(s) will be written to the workspace.")
                        .arg(changes.length).arg(findings.length)
                }
                color: panel.colors.textSecondary
                wrapMode: Text.WordWrap
                font.pixelSize: panel.hostWindow.dp(13)
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(4)
                Repeater {
                    model: panel.controller
                        ? (panel.controller.selectedMemberDetail.changes || []) : []
                    delegate: RowLayout {
                        id: applyChangeRow
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: panel.hostWindow.dp(6)
                        Text {
                            text: applyChangeRow.modelData.operation
                            color: panel.colors.accent
                            font.pixelSize: panel.hostWindow.dp(9)
                            font.weight: Font.DemiBold
                        }
                        Text {
                            Layout.fillWidth: true
                            text: applyChangeRow.modelData.relativePath
                            color: panel.colors.textPrimary
                            font.pixelSize: panel.hostWindow.dp(9)
                            elide: Text.ElideMiddle
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(8)
                Item { Layout.fillWidth: true }
                RunActionButton {
                    objectName: "manualHandoffReviewCancelApplyButton"
                    text: qsTr("Cancel")
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    onClicked: reviewApplyDialog.close()
                }
                RunActionButton {
                    objectName: "manualHandoffReviewConfirmApplyButton"
                    text: qsTr("Approve and apply")
                    danger: true
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    enabled: panel.controller
                        && !panel.controller.busy
                        && panel.controller.selectedMemberDetail.applyAllowed === true
                    onClicked: {
                        reviewApplyDialog.close()
                        const proposalId = panel.controller.selectedMemberProposalId
                        if (panel.controller && !panel.controller.busy
                                && proposalId.length > 0
                                && panel.controller.selectedMemberDetail.applyAllowed === true) {
                            panel.controller.approveAndApplyMember(proposalId)
                        }
                    }
                }
            }
        }
    }

    // Reject/discard confirmation - same modal convention as
    // AgentSessionPanel.qml's own rejectProposalDialog.
    Dialog {
        id: reviewRejectDialog
        anchors.centerIn: parent
        width: panel.hostWindow.dp(430)
        modal: true
        title: qsTr("Reject / discard proposal")
        standardButtons: Dialog.NoButton
        onClosed: panel.confirmRejectDialogOpen = false

        background: Rectangle {
            radius: panel.colors.cornerRadiusMedium
            color: panel.colors.popupBackground
            border.width: 1
            border.color: panel.colors.border
        }

        contentItem: ColumnLayout {
            spacing: panel.hostWindow.dp(12)

            Text {
                Layout.fillWidth: true
                text: qsTr("This proposal will not be applied. The decision and an "
                           + "optional reason are recorded and remain visible.")
                color: panel.colors.textSecondary
                wrapMode: Text.WordWrap
                font.pixelSize: panel.hostWindow.dp(13)
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(4)
                Text {
                    text: qsTr("Reason (optional)")
                    color: panel.colors.textMuted
                    font.pixelSize: panel.hostWindow.dp(9)
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: panel.hostWindow.dp(72)
                    radius: panel.colors.cornerRadiusSmall
                    color: panel.colors.surface
                    border.width: 1
                    border.color: reviewRejectReasonField.activeFocus
                                  ? panel.colors.accent : panel.colors.border
                    TextArea {
                        id: reviewRejectReasonField
                        anchors.fill: parent
                        anchors.margins: panel.hostWindow.dp(6)
                        wrapMode: TextArea.Wrap
                        font.pixelSize: panel.hostWindow.dp(10)
                        color: panel.colors.textPrimary
                        background: Item {}
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(8)
                Item { Layout.fillWidth: true }
                RunActionButton {
                    text: qsTr("Cancel")
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    onClicked: reviewRejectDialog.close()
                }
                RunActionButton {
                    objectName: "manualHandoffReviewConfirmRejectButton"
                    text: qsTr("Reject / discard")
                    danger: true
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    enabled: panel.controller && !panel.controller.busy
                    onClicked: {
                        reviewRejectDialog.close()
                        const proposalId = panel.controller.selectedMemberProposalId
                        if (panel.controller && !panel.controller.busy
                                && proposalId.length > 0) {
                            panel.controller.rejectMember(
                                proposalId, reviewRejectReasonField.text)
                        }
                    }
                }
            }
        }
    }
}
