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

    // Proposal review/apply states (ChangeSetState) are a distinct vocabulary
    // from the attempt/candidate-health statuses statusColor() above already
    // handles - kept as its own function, mirroring AgentSessionPanel.qml's
    // own statusColor() for the identical state strings.
    function proposalStateColor(state) {
        if (state === "ready_for_review" || state === "applied"
                || state === "no_changes_needed") return panel.colors.success
        if (state === "rejected" || state === "apply_failed") return panel.colors.error
        return panel.colors.accent
    }

    // AG-16.2-style presentation only - the ALLOW/DENY decision itself is
    // never re-derived here, mirroring AgentSessionPanel.qml's own
    // applyBlockLabel() exactly.
    function applyBlockLabel(code, message) {
        if (!code) return ""
        if (code === "manifest_apply.gate_not_applicable")
            return qsTr("Manifest verification gate not applicable")
        if (code === "manifest_apply.eligible")
            return qsTr("Verified")
        return message && message.length > 0 ? code + ": " + message : code
    }

    visible: panel.controller !== null && panel.controller.isApplicable
    Layout.fillWidth: true
    Layout.preferredHeight: contentColumn.implicitHeight + panel.hostWindow.dp(24)
    radius: panel.colors.cornerRadiusMedium
    color: panel.colors.surface
    border.width: 1
    border.color: panel.colors.warning

    function statusColor(status) {
        if (status === "completed" || status === "passed") return panel.colors.success
        if (status === "failed" || status === "cancelled"
                || status === "interrupted") return panel.colors.error
        if (status === "prepared" || status === "running"
                || status === "queued") return panel.colors.warning
        return panel.colors.textSecondary
    }

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
            ColumnLayout {
                objectName: "manualHandoffCandidateChanges"
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(5)

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Candidate changes")
                    color: panel.colors.textPrimary
                    font.pixelSize: panel.hostWindow.dp(9)
                    font.weight: Font.DemiBold
                }

                ColumnLayout {
                    objectName: "manualHandoffEditableChangesSection"
                    Layout.fillWidth: true
                    spacing: panel.hostWindow.dp(2)
                    visible: panel.controller
                             && panel.controller.editableChangedRelativePaths.length > 0
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Editable changes")
                        color: panel.colors.textSecondary
                        font.pixelSize: panel.hostWindow.dp(8.5)
                        font.weight: Font.DemiBold
                    }
                    Repeater {
                        model: panel.controller ? panel.controller.editableChangedRelativePaths : []
                        delegate: Text {
                            required property string modelData
                            Layout.fillWidth: true
                            text: "• " + modelData
                            color: panel.colors.success
                            wrapMode: Text.WrapAnywhere
                            font.pixelSize: panel.hostWindow.dp(8.5)
                        }
                    }
                }

                ColumnLayout {
                    objectName: "manualHandoffResetRequiredSection"
                    Layout.fillWidth: true
                    spacing: panel.hostWindow.dp(2)
                    visible: panel.controller
                             && panel.controller.resetRequiredRelativePaths.length > 0
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Reset required before Reverify")
                        color: panel.colors.warning
                        font.pixelSize: panel.hostWindow.dp(8.5)
                        font.weight: Font.DemiBold
                    }
                    Repeater {
                        model: panel.controller ? panel.controller.resetRequiredRelativePaths : []
                        delegate: Text {
                            required property string modelData
                            Layout.fillWidth: true
                            text: "• " + modelData
                            color: panel.colors.warning
                            wrapMode: Text.WrapAnywhere
                            font.pixelSize: panel.hostWindow.dp(8.5)
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Only the editable changes above will be carried into a "
                               + "successor proposal. Every other file present is build/lint "
                               + "context only. Reverify stays fail-closed until any change "
                               + "requiring reset is cleared.")
                    color: panel.colors.textSecondary
                    wrapMode: Text.WordWrap
                    font.pixelSize: panel.hostWindow.dp(8)
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: panel.hostWindow.dp(7)
                    visible: !panel.confirmingReset
                    RunActionButton {
                        objectName: "manualHandoffRefreshCandidateButton"
                        hostWindow: panel.hostWindow
                        colors: panel.colors
                        text: qsTr("Refresh candidate changes")
                        enabled: panel.controller && !panel.controller.busy
                        onClicked: panel.controller.refresh()
                    }
                    RunActionButton {
                        objectName: "manualHandoffResetButton"
                        hostWindow: panel.hostWindow
                        colors: panel.colors
                        text: qsTr("Reset non-editable changes")
                        danger: true
                        visible: panel.controller
                                 && panel.controller.resetRequiredRelativePaths.length > 0
                        enabled: visible && !panel.controller.busy
                        onClicked: panel.confirmingReset = true
                    }
                    Item { Layout.fillWidth: true }
                }

                Text {
                    objectName: "manualHandoffResetConfirmationText"
                    Layout.fillWidth: true
                    visible: panel.confirmingReset
                    text: panel.controller
                          ? qsTr("Reset the following non-editable path(s)?\n%1\n\nEvery "
                                 + "authorized editable change above is preserved. The live "
                                 + "project is never modified.")
                                .arg(panel.controller.resetRequiredRelativePaths.join("\n"))
                          : ""
                    color: panel.colors.warning
                    wrapMode: Text.WordWrap
                    font.pixelSize: panel.hostWindow.dp(9)
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: panel.hostWindow.dp(7)
                    visible: panel.confirmingReset
                    RunActionButton {
                        objectName: "manualHandoffConfirmResetButton"
                        hostWindow: panel.hostWindow
                        colors: panel.colors
                        text: qsTr("Confirm reset")
                        danger: true
                        enabled: panel.controller && !panel.controller.busy
                        onClicked: {
                            panel.confirmingReset = false
                            panel.controller.resetNonEditableChanges()
                        }
                    }
                    RunActionButton {
                        objectName: "manualHandoffCancelResetButton"
                        hostWindow: panel.hostWindow
                        colors: panel.colors
                        text: qsTr("Cancel")
                        enabled: panel.controller && !panel.controller.busy
                        onClicked: panel.confirmingReset = false
                    }
                    Item { Layout.fillWidth: true }
                }

                Text {
                    objectName: "manualHandoffResetResultText"
                    Layout.fillWidth: true
                    visible: panel.controller && panel.controller.resetResultText.length > 0
                    text: panel.controller ? panel.controller.resetResultText : ""
                    color: panel.colors.information
                    wrapMode: Text.WordWrap
                    font.pixelSize: panel.hostWindow.dp(9)
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

        ColumnLayout {
            objectName: "manualHandoffAttemptHistory"
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(4)
            visible: panel.controller && panel.controller.hasHandoff
                     && panel.controller.attempts.length > 0

            Text {
                Layout.fillWidth: true
                text: qsTr("Verification attempts")
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
                        text: qsTr("Attempt %1 / %2 / %3%4")
                              .arg(attemptRow.modelData.attemptOrdinal)
                              .arg(attemptRow.modelData.attemptStatus)
                              .arg(attemptRow.modelData.resultKind.length > 0
                                   ? attemptRow.modelData.resultKind : qsTr("result pending"))
                              .arg(attemptRow.modelData.isAuthoritative
                                   ? qsTr(" / authoritative") : "")
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
                        text: qsTr("Proposal safety: %1 • Build/lint/test: %2 • Acceptance: %3")
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
                        text: qsTr("Show the findings of this attempt in a window")
                        onClicked: panel.showAttemptFindings(attemptRow.modelData)
                    }
                }
            }
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
        ColumnLayout {
            id: reviewSection
            objectName: "manualHandoffReviewSection"
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(5)
            visible: panel.controller && panel.controller.reviewMembers.length > 0

            Text {
                Layout.fillWidth: true
                text: qsTr("Verified proposals")
                color: panel.colors.textPrimary
                font.pixelSize: panel.hostWindow.dp(11)
                font.weight: Font.DemiBold
            }

            Repeater {
                id: reviewMembersRepeater
                objectName: "manualHandoffReviewMembersRepeater"
                model: panel.controller ? panel.controller.reviewMembers : []
                delegate: Rectangle {
                    id: memberItem
                    required property var modelData
                    objectName: "manualHandoffReviewMemberItem_" + memberItem.modelData.proposalId
                    Layout.fillWidth: true
                    Layout.preferredHeight: memberColumn.implicitHeight + panel.hostWindow.dp(12)
                    radius: panel.colors.cornerRadiusSmall
                    color: panel.controller
                           && panel.controller.selectedMemberProposalId
                              === memberItem.modelData.proposalId
                           ? panel.colors.accentSoft : panel.colors.surface
                    border.width: 1
                    border.color: memberItem.modelData.reviewRejected === true
                                  ? panel.colors.error
                                  : panel.proposalStateColor(memberItem.modelData.state)

                    ColumnLayout {
                        id: memberColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: panel.hostWindow.dp(9)
                        anchors.rightMargin: panel.hostWindow.dp(9)
                        spacing: panel.hostWindow.dp(2)
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: panel.hostWindow.dp(6)
                            Text {
                                text: memberItem.modelData.alreadyApplied ? qsTr("applied")
                                      : memberItem.modelData.state
                                color: panel.proposalStateColor(memberItem.modelData.state)
                                font.pixelSize: panel.hostWindow.dp(8.5)
                                font.weight: Font.DemiBold
                            }
                            Text {
                                visible: memberItem.modelData.reviewRejected === true
                                text: qsTr("rejected by reviewer")
                                color: panel.colors.error
                                font.pixelSize: panel.hostWindow.dp(8)
                                font.weight: Font.DemiBold
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("%1 file(s) · %2 finding(s)")
                                      .arg(memberItem.modelData.changes.length)
                                      .arg(memberItem.modelData.findings.length)
                                color: panel.colors.textMuted
                                font.pixelSize: panel.hostWindow.dp(8)
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: memberItem.modelData.summary
                            color: panel.colors.textSecondary
                            font.pixelSize: panel.hostWindow.dp(8.5)
                            wrapMode: Text.WordWrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                        }
                    }

                    MouseArea {
                        objectName: "manualHandoffReviewMemberRowArea_" + memberItem.modelData.proposalId
                        // Plain zero-arg signal, invokable directly via
                        // QMetaObject::invokeMethod(area, "activated") from a
                        // real-QML click-through test - MouseArea's own
                        // built-in clicked(mouse) signal takes a MouseEvent
                        // argument invokeMethod cannot easily construct. A
                        // real mouse click reaches the same onActivated
                        // handler via onClicked emitting this signal, so
                        // both paths run identical logic.
                        signal activated()
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: activated()
                        onActivated: panel.selectMemberProposal(memberItem.modelData.proposalId)
                    }
                }
            }

            // ── Selected member detail: findings + Apply/Reject ─────────
            ColumnLayout {
                id: selectedMemberColumn
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(6)
                visible: panel.controller
                    && panel.controller.selectedMemberProposalId.length > 0

                Repeater {
                    model: panel.controller
                        ? (panel.controller.selectedMemberDetail.findings || []) : []
                    delegate: RowLayout {
                        id: findingRow
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: panel.hostWindow.dp(5)
                        Text { text: "⚠"; color: panel.colors.error; font.pixelSize: panel.hostWindow.dp(9) }
                        Text {
                            Layout.fillWidth: true
                            text: findingRow.modelData.code + ": " + findingRow.modelData.message
                            color: panel.colors.error
                            font.pixelSize: panel.hostWindow.dp(9)
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: {
                        const detail = panel.controller ? panel.controller.selectedMemberDetail : {}
                        return Boolean(detail && detail.reviewRejected === true)
                    }
                    text: {
                        const detail = panel.controller ? panel.controller.selectedMemberDetail : {}
                        const reason = detail && detail.reviewRejectedReason
                        return reason && reason.length > 0
                            ? qsTr("Rejected by reviewer: %1").arg(reason)
                            : qsTr("Rejected by reviewer.")
                    }
                    color: panel.colors.error
                    font.pixelSize: panel.hostWindow.dp(9)
                    font.weight: Font.DemiBold
                    wrapMode: Text.WordWrap
                }

                Text {
                    Layout.fillWidth: true
                    visible: {
                        const detail = panel.controller ? panel.controller.selectedMemberDetail : {}
                        return Boolean(detail && detail.alreadyApplied === true)
                    }
                    text: qsTr("Already applied to the workspace.")
                    color: panel.colors.success
                    font.pixelSize: panel.hostWindow.dp(9)
                    font.weight: Font.DemiBold
                }

                RowLayout {
                    objectName: "manualHandoffReviewApplyStatusRow"
                    Layout.fillWidth: true
                    spacing: panel.hostWindow.dp(5)
                    visible: {
                        const detail = panel.controller ? panel.controller.selectedMemberDetail : {}
                        return Boolean(detail && detail.alreadyApplied !== true
                            && detail.reviewRejected !== true
                            && typeof detail.applyAllowed === "boolean")
                    }
                    Text {
                        text: {
                            const detail = panel.controller ? panel.controller.selectedMemberDetail : {}
                            if (!detail) return ""
                            return detail.applyAllowed === true ? "✓" : "⚠"
                        }
                        color: {
                            const detail = panel.controller ? panel.controller.selectedMemberDetail : {}
                            if (!detail) return panel.colors.textMuted
                            if (detail.applyAllowed !== true) return panel.colors.error
                            return detail.applyBlockCode === "manifest_apply.eligible"
                                ? panel.colors.success : panel.colors.textMuted
                        }
                        font.pixelSize: panel.hostWindow.dp(9)
                        font.weight: Font.DemiBold
                    }
                    Text {
                        Layout.fillWidth: true
                        text: {
                            const detail = panel.controller ? panel.controller.selectedMemberDetail : {}
                            if (!detail) return ""
                            return panel.applyBlockLabel(detail.applyBlockCode, detail.applyBlockMessage)
                        }
                        color: {
                            const detail = panel.controller ? panel.controller.selectedMemberDetail : {}
                            if (!detail) return panel.colors.textMuted
                            if (detail.applyAllowed !== true) return panel.colors.error
                            return detail.applyBlockCode === "manifest_apply.eligible"
                                ? panel.colors.success : panel.colors.textMuted
                        }
                        font.pixelSize: panel.hostWindow.dp(9)
                        wrapMode: Text.WordWrap
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: panel.hostWindow.dp(7)
                    visible: {
                        const detail = panel.controller ? panel.controller.selectedMemberDetail : {}
                        return Boolean(detail && detail.alreadyApplied !== true
                            && detail.reviewRejected !== true)
                    }

                    RunActionButton {
                        objectName: "manualHandoffReviewApplyButton"
                        hostWindow: panel.hostWindow
                        colors: panel.colors
                        text: qsTr("Apply to workspace")
                        primary: true
                        enabled: !panel.confirmApplyDialogOpen && !panel.confirmRejectDialogOpen
                            && panel.controller
                            && !panel.controller.busy
                            && panel.controller.selectedMemberDetail.applyAllowed === true
                        onClicked: {
                            panel.confirmApplyDialogOpen = true
                            reviewApplyDialog.open()
                        }
                    }
                    RunActionButton {
                        objectName: "manualHandoffReviewRejectButton"
                        hostWindow: panel.hostWindow
                        colors: panel.colors
                        text: qsTr("Reject / discard")
                        danger: true
                        enabled: !panel.confirmApplyDialogOpen && !panel.confirmRejectDialogOpen
                            && panel.controller && !panel.controller.busy
                        onClicked: {
                            reviewRejectReasonField.text = ""
                            panel.confirmRejectDialogOpen = true
                            reviewRejectDialog.open()
                        }
                    }
                    Item { Layout.fillWidth: true }
                }

                Repeater {
                    objectName: "manualHandoffReviewSelectedChangesRepeater"
                    model: panel.controller
                        ? (panel.controller.selectedMemberDetail.changes || []) : []
                    delegate: ColumnLayout {
                        id: reviewChangeItem
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: panel.hostWindow.dp(4)

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: panel.hostWindow.dp(6)
                            Text { text: reviewChangeItem.modelData.operation; color: panel.colors.accent; font.pixelSize: panel.hostWindow.dp(9); font.weight: Font.DemiBold }
                            Text {
                                Layout.fillWidth: true
                                text: reviewChangeItem.modelData.relativePath
                                color: panel.colors.textPrimary
                                font.pixelSize: panel.hostWindow.dp(9)
                                elide: Text.ElideMiddle
                            }
                            Text {
                                text: panel.diffChangeId === reviewChangeItem.modelData.changeId
                                      ? qsTr("Hide diff") : qsTr("View diff")
                                color: reviewDiffToggleArea.containsMouse
                                       ? panel.colors.textPrimary : panel.colors.accent
                                font.pixelSize: panel.hostWindow.dp(9)
                                MouseArea {
                                    id: reviewDiffToggleArea
                                    objectName: "manualHandoffReviewDiffToggle_"
                                        + reviewChangeItem.modelData.changeId
                                    // Plain zero-arg signal, invokable
                                    // directly via QMetaObject::invokeMethod
                                    // from a real-QML click-through test -
                                    // see manualHandoffReviewMemberRowArea's
                                    // own matching doc comment above. A real
                                    // mouse click reaches the same
                                    // onActivated handler via onClicked
                                    // emitting this signal.
                                    signal activated()
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: activated()
                                    onActivated: {
                                        if (panel.diffChangeId === reviewChangeItem.modelData.changeId) {
                                            panel.diffChangeId = ""
                                            return
                                        }
                                        panel.diffChangeId = reviewChangeItem.modelData.changeId
                                        panel.originalText = panel.controller.readOriginalContent(
                                            panel.controller.selectedMemberProposalId,
                                            reviewChangeItem.modelData.changeId)
                                        panel.proposedText = panel.controller.readProposedContent(
                                            panel.controller.selectedMemberProposalId,
                                            reviewChangeItem.modelData.changeId)
                                    }
                                }
                            }
                            Text {
                                visible: panel.diffChangeId === reviewChangeItem.modelData.changeId
                                text: qsTr("Open large view")
                                color: reviewDiffLargeArea.containsMouse
                                       ? panel.colors.textPrimary : panel.colors.accent
                                font.pixelSize: panel.hostWindow.dp(9)
                                MouseArea {
                                    id: reviewDiffLargeArea
                                    objectName: "manualHandoffReviewDiffLarge_"
                                        + reviewChangeItem.modelData.changeId
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        reviewDiffDialog.fileLabel =
                                            reviewChangeItem.modelData.relativePath
                                        reviewDiffDialog.open()
                                    }
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            visible: panel.diffChangeId === reviewChangeItem.modelData.changeId
                            spacing: panel.hostWindow.dp(6)

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: panel.hostWindow.dp(2)
                                Text { text: qsTr("Original"); color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(8) }
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: panel.hostWindow.dp(140)
                                    color: panel.colors.surface
                                    border.width: 1
                                    border.color: panel.colors.border
                                    Flickable {
                                        anchors.fill: parent
                                        anchors.margins: panel.hostWindow.dp(6)
                                        contentWidth: width
                                        contentHeight: reviewOriginalText.implicitHeight
                                        clip: true
                                        boundsBehavior: Flickable.StopAtBounds
                                        TextEdit {
                                            id: reviewOriginalText
                                            width: parent.width
                                            readOnly: true
                                            selectByMouse: true
                                            textFormat: TextEdit.PlainText
                                            text: panel.originalText.length > 0
                                                  ? panel.originalText : qsTr("(no original content)")
                                            color: panel.colors.textSecondary
                                            selectionColor: panel.colors.accent
                                            selectedTextColor: panel.colors.textOnAccent
                                            font.pixelSize: panel.hostWindow.dp(8.5)
                                            font.family: "monospace"
                                            wrapMode: TextEdit.WrapAnywhere
                                        }
                                    }
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: panel.hostWindow.dp(2)
                                Text { text: qsTr("Proposed"); color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(8) }
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: panel.hostWindow.dp(140)
                                    color: panel.colors.surface
                                    border.width: 1
                                    border.color: panel.colors.border
                                    Flickable {
                                        anchors.fill: parent
                                        anchors.margins: panel.hostWindow.dp(6)
                                        contentWidth: width
                                        contentHeight: reviewProposedText.implicitHeight
                                        clip: true
                                        boundsBehavior: Flickable.StopAtBounds
                                        TextEdit {
                                            id: reviewProposedText
                                            width: parent.width
                                            readOnly: true
                                            selectByMouse: true
                                            textFormat: TextEdit.PlainText
                                            text: panel.proposedText.length > 0
                                                  ? panel.proposedText : qsTr("(no proposed content)")
                                            color: panel.colors.textSecondary
                                            selectionColor: panel.colors.accent
                                            selectedTextColor: panel.colors.textOnAccent
                                            font.pixelSize: panel.hostWindow.dp(8.5)
                                            font.family: "monospace"
                                            wrapMode: TextEdit.WrapAnywhere
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Both confirmation dialogs are deliberately direct children of the root
    // Rectangle (panel), never of contentColumn/reviewSection (both
    // ColumnLayouts) - a Dialog/Popup is rendered through the window's own
    // Overlay regardless of where it sits in the QML tree, but qmllint's
    // Quick.layout-positioning rule still flags any Item child of a Layout
    // that sets explicit anchors/width, which centering/sizing a modal
    // confirmation genuinely needs. Moving them here (panel is a plain
    // Rectangle, not a Layout) avoids that false-positive without changing
    // any visible behavior - same convention as AgentSessionPanel.qml's own
    // dialogs would use if AgentSessionPanel's own root were not itself a
    // ColumnLayout.

    // The findings of an attempt (or of the Run itself) in their own, readable window: the Backend's groups, untouched.
    function attemptFindingCount(attempt) {
        return (attempt.actionableDiagnostics || []).length + (attempt.outsideScopeDiagnostics || []).length
            + (attempt.generalDiagnostics || []).length + (attempt.newActionableDiagnostics || []).length
            + (attempt.newOutsideManifestOrContextDiagnostics || []).length + (attempt.unchangedBaselineDebtDiagnostics || []).length
    }

    // One line of counts per attempt, straight from the Backend's buckets (QML only counts and words them).
    function attemptFindingSummary(attempt) {
        const parts = []
        const add = function (list, one, many) {
            const n = (list || []).length
            if (n > 0) parts.push(n === 1 ? one : many.arg(n))
        }
        add(attempt.actionableDiagnostics, qsTr("1 in the files you may edit"), qsTr("%1 in the files you may edit"))
        add(attempt.newActionableDiagnostics, qsTr("1 new to fix"), qsTr("%1 new to fix"))
        add(attempt.outsideScopeDiagnostics, qsTr("1 in a file you may NOT edit"), qsTr("%1 in files you may NOT edit"))
        add(attempt.newOutsideManifestOrContextDiagnostics, qsTr("1 new outside the Manifest's write scope"),
            qsTr("%1 new outside the Manifest's write scope"))
        add(attempt.generalDiagnostics, qsTr("1 general message"), qsTr("%1 general messages"))
        add(attempt.unchangedBaselineDebtDiagnostics, qsTr("1 older warning (not blocking)"), qsTr("%1 older warnings (not blocking)"))
        return qsTr("Findings: %1").arg(parts.join(" \u2022 "))
    }

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
