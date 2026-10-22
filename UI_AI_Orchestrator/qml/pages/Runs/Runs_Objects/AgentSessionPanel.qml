import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Displays the agent tool session (if any) for the currently selected run:
// provider turns, audited tool calls (status/truncated/denied), and any
// change proposals the session produced, with validation findings and an
// on-demand original/proposed content diff preview.
//
// Nothing this component renders is an absolute filesystem path, a raw
// tool argument/result payload, a secret, or a filesystem handle - every
// string comes from AgentSessionController, which only ever exposes the
// UI-safe AgentToolReadModelService projection.
ColumnLayout {
    id: panel
    objectName: "agentSessionPanel"

    required property var hostWindow
    required property var colors
    required property var controller
    // backend.runs (RunController). Its runUpdated(runId) signal already
    // fires on every RunEvent for this run (queued/started/stage/progress -
    // the same events that used to drive a blind 1200ms poll here), so it
    // doubles as the "something for this session might have changed" signal
    // without a second, independent polling loop hitting the database.
    required property var runsController
    property string runId: ""
    // Whether the owning Run is still queued/running. Driven from
    // RunDetailsPanel's own runData.status - the panel has no other way to
    // know the run is still live, since AgentToolSession does not exist at
    // all until AgentToolTurnLoopRunner actually calls
    // AgentToolExecutionService::beginSession() partway through the run.
    property bool runActive: false

    property string selectedChangeSetId: ""
    property string diffChangeId: ""
    property string originalText: ""
    property string proposedText: ""
    // Gates the Apply button while the "Approve and apply" confirmation
    // dialog below is open. The dialog is modal (blocks input to the panel
    // underneath on its own), so this is a belt-and-suspenders disable
    // rather than the primary guard.
    property bool confirmDialogOpen: false
    // Same belt-and-suspenders gate for the AG-07 "Reject / discard"
    // confirmation dialog.
    property bool rejectDialogOpen: false

    onRunIdChanged: {
        selectedChangeSetId = ""
        diffChangeId = ""
        originalText = ""
        proposedText = ""
        if (panel.controller) panel.controller.selectRun(panel.runId)
    }

    // The Connections block below reacts the instant a RunEvent arrives, but
    // the run's own status only flips to non-active *after* run() has fully
    // returned - by which point the session's terminal state (Completed
    // proposal + AgentToolSession.state) is already durably persisted (see
    // AgentToolTurnLoopRunner::run(): turns are recorded before the
    // proposal is created and the session is marked Completed). Without
    // this, the very last RunEvent can still land in that in-between window -
    // final turn recorded, proposal/session-completion not yet written -
    // and freeze the panel there forever (stuck "active" badge, no
    // proposal shown) since nothing refreshes again afterwards. One
    // guaranteed catch-up refresh once the run is known to be finished
    // fixes it.
    onRunActiveChanged: {
        if (!panel.runActive && panel.controller) panel.controller.refresh()
    }

    spacing: hostWindow.dp(10)
    visible: controller !== null && controller.hasSession

    // AgentSessionController is pull-based (no push event stream of its own
    // yet), but RunController already re-emits runUpdated(runId) for every
    // RunEvent - queued/started/stage transitions/per-turn progress ticks -
    // so reacting to that gives the same coverage a poll would, without an
    // independent timer hitting the database on a fixed cadence regardless
    // of whether anything actually changed. runId itself never changes while
    // a run is in progress, so onRunIdChanged alone only loads the session
    // once, at selection time; this Connections block is what discovers a
    // not-yet-created session (AgentToolSession doesn't exist until
    // AgentToolTurnLoopRunner calls beginSession() partway through the run)
    // and keeps pulling new turns/tool calls/proposals for an existing one.
    Connections {
        target: panel.runsController
        function onRunUpdated(runId) {
            // refresh() re-discovers a not-yet-created session (falls back
            // to selectRun() internally when no session is selected yet)
            // and, once one exists, just re-queries and replaces the list
            // models without the extra clear-then-reselect round trip
            // selectRun() does.
            if (runId === panel.runId && panel.controller) panel.controller.refresh()
        }
    }

    function statusColor(status) {
        if (status === "completed" || status === "ready_for_review"
            || status === "no_changes_needed" || status === "applied")
            return colors.success
        if (status === "failed" || status === "rejected" || status === "interrupted"
            || status === "apply_failed")
            return colors.error
        return colors.accent
    }

    function selectProposalId(changeSetId) {
        panel.selectedChangeSetId = changeSetId || ""
        panel.diffChangeId = ""
        panel.originalText = ""
        panel.proposedText = ""
        panel.controller.selectProposal(panel.selectedChangeSetId)
    }

    // AG-16.2: the single, Backend-derived apply-eligibility decision for
    // the currently selected proposal is never computed here - always the
    // exact allowed/reasonCode/reasonMessage
    // ManifestApplyVerificationGate::evaluate() returned, projected as-is
    // by AgentSessionController::selectProposal(). A known reasonCode is
    // mapped to a short, friendly label purely for presentation; the
    // ALLOW/DENY decision itself is never re-derived here.
    function applyBlockLabel(code, message) {
        if (!code) return ""
        if (code === "manifest_apply.gate_not_applicable")
            return qsTr("Manifest verification gate not applicable")
        if (code === "manifest_apply.eligible")
            return qsTr("Verified")
        return message && message.length > 0 ? code + ": " + message : code
    }

    RowLayout {
        Layout.fillWidth: true
        Text {
            Layout.fillWidth: true
            text: qsTr("Agent tool session")
            color: panel.colors.textPrimary
            font.pixelSize: panel.hostWindow.dp(13)
            font.weight: Font.DemiBold
        }
        Rectangle {
            Layout.preferredWidth: sessionStatusText.implicitWidth + panel.hostWindow.dp(16)
            Layout.preferredHeight: panel.hostWindow.dp(22)
            radius: height / 2
            color: panel.colors.accentSoft
            Text {
                id: sessionStatusText
                anchors.centerIn: parent
                text: panel.controller ? panel.controller.sessionState : ""
                color: panel.statusColor(panel.controller ? panel.controller.sessionState : "")
                font.pixelSize: panel.hostWindow.dp(9)
                font.weight: Font.DemiBold
            }
        }
    }
    Text {
        Layout.fillWidth: true
        text: panel.controller ? qsTr("Started %1").arg(panel.controller.sessionStartedText) : ""
        color: panel.colors.textMuted
        font.pixelSize: panel.hostWindow.dp(9)
    }

    // ── Provider turns ───────────────────────────────────────────────
    Text {
        Layout.fillWidth: true
        text: qsTr("Provider turns")
        color: panel.colors.textPrimary
        font.pixelSize: panel.hostWindow.dp(11)
        font.weight: Font.DemiBold
    }
    Repeater {
        id: turnsRepeater
        model: panel.controller ? panel.controller.turnsModel : null
        delegate: RowLayout {
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(6)
            Text { text: "#" + model.turnIndex; color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(9) }
            Text { Layout.fillWidth: true; text: model.finishReason; color: panel.colors.textPrimary; font.pixelSize: panel.hostWindow.dp(9) }
            Text { text: qsTr("%1 call(s) · %2/%3 tok").arg(model.callCount).arg(model.inputTokens).arg(model.outputTokens); color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(8.5) }
            Text { text: model.createdAtText; color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(8.5) }
        }
    }
    Text {
        Layout.fillWidth: true
        // Repeater.count (not model.rowCount()) - a plain method call inside
        // a QML binding never re-evaluates as the underlying model's rows
        // change, since it isn't a NOTIFYing property read. Repeater.count
        // is, so it stays in sync as turns are added.
        visible: turnsRepeater.count === 0
        text: qsTr("No provider turns recorded.")
        color: panel.colors.textMuted
        font.pixelSize: panel.hostWindow.dp(9)
    }

    // ── Tool calls ───────────────────────────────────────────────────
    Text {
        Layout.fillWidth: true
        text: qsTr("Tool calls")
        color: panel.colors.textPrimary
        font.pixelSize: panel.hostWindow.dp(11)
        font.weight: Font.DemiBold
    }
    Repeater {
        id: toolCallsRepeater
        model: panel.controller ? panel.controller.toolCallsModel : null
        delegate: Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: callRow.implicitHeight + panel.hostWindow.dp(10)
            radius: panel.colors.cornerRadiusSmall
            color: panel.colors.surface
            border.width: 1
            border.color: model.denied ? panel.colors.error : panel.colors.border

            RowLayout {
                id: callRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: panel.hostWindow.dp(9)
                anchors.rightMargin: panel.hostWindow.dp(9)
                spacing: panel.hostWindow.dp(7)
                Text { Layout.fillWidth: true; text: model.toolName; color: panel.colors.textPrimary; font.pixelSize: panel.hostWindow.dp(9.5) }
                Text { visible: model.truncated; text: qsTr("truncated"); color: panel.colors.warning; font.pixelSize: panel.hostWindow.dp(8.5) }
                Text { visible: model.denied; text: qsTr("denied: %1").arg(model.errorCode); color: panel.colors.error; font.pixelSize: panel.hostWindow.dp(8.5) }
                Text { text: model.status; color: panel.statusColor(model.status); font.pixelSize: panel.hostWindow.dp(8.5); font.weight: Font.DemiBold }
                Text { text: qsTr("%1 B").arg(model.returnedBytes); color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(8.5) }
            }
        }
    }
    Text {
        Layout.fillWidth: true
        visible: toolCallsRepeater.count === 0
        text: qsTr("No tool calls recorded.")
        color: panel.colors.textMuted
        font.pixelSize: panel.hostWindow.dp(9)
    }

    // ── Change proposals ─────────────────────────────────────────────
    Text {
        Layout.fillWidth: true
        text: qsTr("Change proposals")
        color: panel.colors.textPrimary
        font.pixelSize: panel.hostWindow.dp(11)
        font.weight: Font.DemiBold
    }
    Repeater {
        id: proposalsRepeater
        model: panel.controller ? panel.controller.proposalsModel : null
        delegate: Rectangle {
            id: proposalItem
            Layout.fillWidth: true
            Layout.preferredHeight: proposalColumn.implicitHeight + panel.hostWindow.dp(14)
            radius: panel.colors.cornerRadiusSmall
            color: panel.selectedChangeSetId === model.changeSetId ? panel.colors.accentSoft : panel.colors.surface
            border.width: 1
            border.color: model.reviewRejected === true ? panel.colors.error : panel.statusColor(model.state)

            ColumnLayout {
                id: proposalColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: panel.hostWindow.dp(10)
                anchors.rightMargin: panel.hostWindow.dp(10)
                spacing: panel.hostWindow.dp(3)
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: model.state; color: panel.statusColor(model.state); font.pixelSize: panel.hostWindow.dp(9); font.weight: Font.DemiBold }
                    // AG-07: an explicit human "rejected by reviewer" fact is
                    // layered on top of the unchanged validator state above -
                    // it never relabels model.state itself (see
                    // ProposalReviewTypes.hpp).
                    Text {
                        visible: model.reviewRejected === true
                        text: qsTr("rejected by reviewer")
                        color: panel.colors.error
                        font.pixelSize: panel.hostWindow.dp(8.5)
                        font.weight: Font.DemiBold
                    }
                    Text {
                        visible: model.superseded === true
                        text: qsTr("superseded")
                        color: panel.colors.warning
                        font.pixelSize: panel.hostWindow.dp(8.5)
                        font.weight: Font.DemiBold
                    }
                    Text {
                        visible: model.supersedesChangeSetId.length > 0
                        text: qsTr("repaired successor")
                        color: panel.colors.information
                        font.pixelSize: panel.hostWindow.dp(8.5)
                        font.weight: Font.DemiBold
                    }
                    Text { Layout.fillWidth: true; text: qsTr("%1 file(s) · %2 finding(s)").arg(model.changeCount).arg(model.findingCount); color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(8.5) }
                }
                Text {
                    Layout.fillWidth: true
                    text: model.summary
                    color: panel.colors.textSecondary
                    font.pixelSize: panel.hostWindow.dp(9)
                    wrapMode: Text.WordWrap
                    maximumLineCount: 3
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    visible: model.supersedesChangeSetId.length > 0
                             || model.supersededByChangeSetId.length > 0
                    text: model.supersedesChangeSetId.length > 0
                          ? qsTr("Repaired from %1").arg(model.supersedesChangeSetId)
                          : qsTr("Superseded by %1").arg(model.supersededByChangeSetId)
                    color: panel.colors.textMuted
                    font.pixelSize: panel.hostWindow.dp(8)
                    elide: Text.ElideMiddle
                }
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    const id = model.changeSetId
                    panel.selectProposalId(panel.selectedChangeSetId === id ? "" : id)
                }
            }
        }
    }
    Text {
        Layout.fillWidth: true
        visible: proposalsRepeater.count === 0
        text: qsTr("This session produced no change proposal.")
        color: panel.colors.textMuted
        font.pixelSize: panel.hostWindow.dp(9)
    }

    // ── Selected proposal detail: findings + per-file diff toggle ────
    ColumnLayout {
        Layout.fillWidth: true
        visible: panel.selectedChangeSetId.length > 0
        spacing: panel.hostWindow.dp(6)

        RowLayout {
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(6)
            visible: {
                const detail = panel.controller ? panel.controller.selectedProposalDetail : {}
                return Boolean(detail
                        && ((detail.supersedesChangeSetId
                             && detail.supersedesChangeSetId.length > 0)
                            || (detail.supersededByChangeSetId
                                && detail.supersededByChangeSetId.length > 0)))
            }
            RunActionButton {
                visible: {
                    const detail = panel.controller ? panel.controller.selectedProposalDetail : {}
                    return Boolean(detail && detail.supersedesChangeSetId
                                   && detail.supersedesChangeSetId.length > 0)
                }
                hostWindow: panel.hostWindow
                colors: panel.colors
                text: qsTr("Open rejected predecessor")
                iconText: "←"
                onClicked: panel.selectProposalId(
                               panel.controller.selectedProposalDetail.supersedesChangeSetId)
            }
            RunActionButton {
                visible: {
                    const detail = panel.controller ? panel.controller.selectedProposalDetail : {}
                    return Boolean(detail && detail.supersededByChangeSetId
                                   && detail.supersededByChangeSetId.length > 0)
                }
                hostWindow: panel.hostWindow
                colors: panel.colors
                text: qsTr("Open repaired successor")
                iconText: "→"
                primary: true
                onClicked: panel.selectProposalId(
                               panel.controller.selectedProposalDetail.supersededByChangeSetId)
            }
            Item { Layout.fillWidth: true }
        }

        Repeater {
            model: panel.controller ? (panel.controller.selectedProposalDetail.findings || []) : []
            delegate: RowLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(5)
                Text { text: "⚠"; color: panel.colors.error; font.pixelSize: panel.hostWindow.dp(9) }
                Text {
                    Layout.fillWidth: true
                    text: modelData.code + ": " + modelData.message
                    color: panel.colors.error
                    font.pixelSize: panel.hostWindow.dp(9)
                    wrapMode: Text.WordWrap
                }
            }
        }

        // Apply result feedback (success or error from last applyProposal call).
        Text {
            Layout.fillWidth: true
            visible: panel.controller && panel.controller.applyResultText.length > 0
            text: panel.controller ? panel.controller.applyResultText : ""
            color: panel.colors.success
            font.pixelSize: panel.hostWindow.dp(9)
            wrapMode: Text.WordWrap
        }
        Text {
            objectName: "agentSessionErrorText"
            Layout.fillWidth: true
            visible: panel.controller && panel.controller.errorText.length > 0
            text: {
                if (!panel.controller) return ""
                return panel.controller.errorCode.length > 0
                    ? panel.controller.errorCode + ": " + panel.controller.errorText
                    : panel.controller.errorText
            }
            color: panel.colors.error
            font.pixelSize: panel.hostWindow.dp(9)
            wrapMode: Text.WordWrap
        }

        // AG-07: once a reviewer has rejected this proposal, Approve/Apply is
        // replaced by a plain terminal statement instead of being silently
        // hidden - reject/discard is a visible, permanent decision, not a
        // dead end the user has to infer.
        Text {
            Layout.fillWidth: true
            visible: {
                const detail = panel.controller ? panel.controller.selectedProposalDetail : {}
                return detail && detail.reviewRejected === true
            }
            text: {
                const detail = panel.controller ? panel.controller.selectedProposalDetail : {}
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

        // Verification status line: visible whenever a still-reviewable
        // ReadyForReview proposal is selected, regardless of whether Apply
        // is currently allowed - a blocked proposal stays visible with its
        // reason readable, per AG-16.2's "keep a blocked proposal visible"
        // requirement, rather than disappearing along with the buttons.
        RowLayout {
            id: applyVerificationStatusRow
            objectName: "applyVerificationStatusRow"
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(5)
            visible: {
                const detail = panel.controller ? panel.controller.selectedProposalDetail : {}
                return Boolean(detail && detail.state === "ready_for_review"
                    && detail.reviewRejected !== true
                    && typeof detail.applyAllowed === "boolean")
            }
            Text {
                text: {
                    const detail = panel.controller ? panel.controller.selectedProposalDetail : {}
                    if (!detail) return ""
                    return detail.applyAllowed === true ? "✓" : "⚠"
                }
                // Failed/missing verification must never receive success
                // styling; a gate-not-applicable allow is neutral, never
                // styled as a passed verification.
                color: {
                    const detail = panel.controller ? panel.controller.selectedProposalDetail : {}
                    if (!detail) return panel.colors.textMuted
                    if (detail.applyAllowed !== true) return panel.colors.error
                    return detail.applyBlockCode === "manifest_apply.eligible"
                        ? panel.colors.success : panel.colors.textMuted
                }
                font.pixelSize: panel.hostWindow.dp(9)
                font.weight: Font.DemiBold
            }
            Text {
                objectName: "applyVerificationStatusText"
                Layout.fillWidth: true
                text: {
                    const detail = panel.controller ? panel.controller.selectedProposalDetail : {}
                    if (!detail) return ""
                    return panel.applyBlockLabel(detail.applyBlockCode, detail.applyBlockMessage)
                }
                color: {
                    const detail = panel.controller ? panel.controller.selectedProposalDetail : {}
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
            spacing: panel.hostWindow.dp(8)
            visible: {
                const detail = panel.controller ? panel.controller.selectedProposalDetail : {}
                // Backend resolves the trusted workspace from the immutable
                // Run snapshot. UI must never supply or gate that security
                // boundary with a mutable path.
                return detail && detail.state === "ready_for_review" && detail.reviewRejected !== true
            }

            // Apply button: only shown for a still-reviewable ReadyForReview
            // proposal, and only enabled while the Backend's own
            // ManifestApplyVerificationGate decision (applyAllowed) says so -
            // this is a visual guide, not a concurrency lock: the final
            // Backend approve()/apply() call re-checks the same gate itself
            // immediately before writing anything (see
            // ChangeSetApplyService::apply()).
            Rectangle {
                id: applyButton
                objectName: "applyToWorkspaceButton"
                Layout.fillWidth: true
                Layout.preferredHeight: panel.hostWindow ? panel.hostWindow.dp(32) : 32
                radius: panel.colors ? panel.colors.cornerRadiusMedium : 6
                enabled: !panel.confirmDialogOpen && !panel.rejectDialogOpen
                    && panel.controller
                    && panel.controller.selectedProposalDetail.applyAllowed === true
                opacity: enabled ? 1.0 : 0.6
                color: applyArea.containsMouse
                       ? Qt.lighter(panel.colors.accent, 1.12)
                       : panel.colors.accent

                Text {
                    anchors.centerIn: parent
                    text: qsTr("Apply to workspace")
                    color: panel.colors.textOnAccent
                    font.pixelSize: panel.hostWindow.dp(10)
                    font.weight: Font.DemiBold
                }
                MouseArea {
                    id: applyArea
                    anchors.fill: parent
                    enabled: applyButton.enabled
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (panel.controller && panel.selectedChangeSetId.length > 0
                                && panel.controller.selectedProposalDetail.applyAllowed === true) {
                            panel.confirmDialogOpen = true
                            approveAndApplyDialog.open()
                        }
                    }
                }
            }

            // Reject / discard button (AG-07): a distinct, equally visible
            // action beside Approve and apply, per ARCHITECTURE_GAPS.md's
            // "Run Details exposes Approve and apply but no equally clear
            // user action to reject/discard a proposal."
            Rectangle {
                id: rejectButton
                Layout.preferredWidth: panel.hostWindow.dp(120)
                Layout.preferredHeight: panel.hostWindow ? panel.hostWindow.dp(32) : 32
                radius: panel.colors ? panel.colors.cornerRadiusMedium : 6
                enabled: !panel.confirmDialogOpen && !panel.rejectDialogOpen
                opacity: enabled ? 1.0 : 0.6
                color: "transparent"
                border.width: 1
                border.color: panel.colors.error

                Text {
                    anchors.centerIn: parent
                    text: qsTr("Reject / discard")
                    color: panel.colors.error
                    font.pixelSize: panel.hostWindow.dp(10)
                    font.weight: Font.DemiBold
                }
                MouseArea {
                    id: rejectArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (panel.controller && panel.selectedChangeSetId.length > 0) {
                            rejectReasonField.text = ""
                            panel.rejectDialogOpen = true
                            rejectProposalDialog.open()
                        }
                    }
                }
            }
        }

        // Confirmation between "reviewed the diff" and "wrote to disk" - the
        // Apply button used to call applyProposal() directly on click with
        // no intervening step. Follows the same Dialog convention as
        // ProjectsPage.qml's "Archive project?" dialog: standardButtons:
        // Dialog.NoButton, hand-rolled Cancel/Confirm buttons. Reuses
        // selectedProposalDetail, already loaded and reactive from
        // selectProposal() above - no new backend round trip just to show
        // this summary.
        DiffViewDialog {
            id: diffViewDialog
            hostWindow: panel.hostWindow
            colors: panel.colors
            originalText: panel.originalText
            proposedText: panel.proposedText
        }

        Dialog {
            id: approveAndApplyDialog
            anchors.centerIn: parent
            width: panel.hostWindow.dp(430)
            modal: true
            title: qsTr("Approve and apply")
            standardButtons: Dialog.NoButton
            onClosed: panel.confirmDialogOpen = false

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
                        const detail = panel.controller ? panel.controller.selectedProposalDetail : {}
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
                        model: panel.controller ? (panel.controller.selectedProposalDetail.changes || []) : []
                        delegate: RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: panel.hostWindow.dp(6)
                            Text {
                                text: modelData.operation
                                color: panel.colors.accent
                                font.pixelSize: panel.hostWindow.dp(9)
                                font.weight: Font.DemiBold
                            }
                            Text {
                                Layout.fillWidth: true
                                text: modelData.relativePath
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
                        text: qsTr("Cancel")
                        hostWindow: panel.hostWindow
                        colors: panel.colors
                        onClicked: approveAndApplyDialog.close()
                    }

                    RunActionButton {
                        text: qsTr("Approve and apply")
                        danger: true
                        hostWindow: panel.hostWindow
                        colors: panel.colors
                        // AG-16.2: repeats the exact same Backend-derived
                        // guard the Apply button itself uses - the dialog's
                        // own confirm action is not a separate, looser path.
                        // The Backend approve()/apply() call remains the
                        // authoritative decision regardless; this only keeps
                        // the UI from firing a call it already knows will be
                        // refused.
                        enabled: panel.controller
                            && panel.controller.selectedProposalDetail.applyAllowed === true
                        onClicked: {
                            approveAndApplyDialog.close()
                            if (panel.controller && panel.selectedChangeSetId.length > 0
                                    && panel.controller.selectedProposalDetail.applyAllowed === true) {
                                panel.controller.approveAndApply(panel.selectedChangeSetId)
                            }
                        }
                    }
                }
            }
        }

        // Reject/discard confirmation (AG-07). Same modal convention as
        // approveAndApplyDialog above, with an optional bounded reason field -
        // ProposalReviewService.hpp's kMaximumReasonBytes (2000) is enforced
        // Backend-side; maximumLength here is only a friendly UI hint at the
        // same order of magnitude.
        Dialog {
            id: rejectProposalDialog
            anchors.centerIn: parent
            width: panel.hostWindow.dp(430)
            modal: true
            title: qsTr("Reject / discard proposal")
            standardButtons: Dialog.NoButton
            onClosed: panel.rejectDialogOpen = false

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
                    text: qsTr("This proposal will not be applied. The decision and an optional reason are recorded and remain visible.")
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
                        border.color: rejectReasonField.activeFocus ? panel.colors.accent : panel.colors.border
                        TextArea {
                            id: rejectReasonField
                            anchors.fill: parent
                            anchors.margins: panel.hostWindow.dp(6)
                            wrapMode: TextArea.Wrap
                            font.pixelSize: panel.hostWindow.dp(10)
                            color: panel.colors.textPrimary
                            // TextArea has no maximumLength property (unlike
                            // TextField/TextInput); ProposalReviewService's
                            // kMaximumReasonBytes (2000) is the real,
                            // enforced Backend limit - an over-limit reason
                            // surfaces through panel.controller.errorText via
                            // the existing rejectProposal() failure path.
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
                        onClicked: rejectProposalDialog.close()
                    }

                    RunActionButton {
                        text: qsTr("Reject / discard")
                        danger: true
                        hostWindow: panel.hostWindow
                        colors: panel.colors
                        onClicked: {
                            rejectProposalDialog.close()
                            if (panel.controller && panel.selectedChangeSetId.length > 0) {
                                panel.controller.rejectProposal(
                                    panel.selectedChangeSetId, rejectReasonField.text)
                            }
                        }
                    }
                }
            }
        }

        Repeater {
            model: panel.controller ? (panel.controller.selectedProposalDetail.changes || []) : []
            delegate: ColumnLayout {
                id: changeItem
                required property var modelData
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(4)

                RowLayout {
                    Layout.fillWidth: true
                    spacing: panel.hostWindow.dp(6)
                    Text { text: changeItem.modelData.operation; color: panel.colors.accent; font.pixelSize: panel.hostWindow.dp(9); font.weight: Font.DemiBold }
                    Text {
                        Layout.fillWidth: true
                        text: changeItem.modelData.relativePath
                        color: panel.colors.textPrimary
                        font.pixelSize: panel.hostWindow.dp(9)
                        elide: Text.ElideMiddle
                    }
                    Text {
                        text: panel.diffChangeId === changeItem.modelData.changeId
                              ? qsTr("Hide diff") : qsTr("View diff")
                        color: diffToggleArea.containsMouse ? panel.colors.textPrimary : panel.colors.accent
                        font.pixelSize: panel.hostWindow.dp(9)
                        MouseArea {
                            id: diffToggleArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (panel.diffChangeId === changeItem.modelData.changeId) {
                                    panel.diffChangeId = ""
                                    return
                                }
                                panel.diffChangeId = changeItem.modelData.changeId
                                panel.originalText = panel.controller.readOriginalContent(
                                            panel.selectedChangeSetId, changeItem.modelData.changeId)
                                panel.proposedText = panel.controller.readProposedContent(
                                            panel.selectedChangeSetId, changeItem.modelData.changeId)
                            }
                        }
                    }
                    Text {
                        visible: panel.diffChangeId === changeItem.modelData.changeId
                        text: qsTr("Open large view")
                        color: diffLargeArea.containsMouse ? panel.colors.textPrimary : panel.colors.accent
                        font.pixelSize: panel.hostWindow.dp(9)
                        MouseArea {
                            id: diffLargeArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                diffViewDialog.fileLabel = changeItem.modelData.relativePath
                                diffViewDialog.open()
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: panel.diffChangeId === changeItem.modelData.changeId
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
                                contentHeight: originalContentText.implicitHeight
                                clip: true
                                boundsBehavior: Flickable.StopAtBounds
                                TextEdit {
                                    id: originalContentText
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
                                contentHeight: proposedContentText.implicitHeight
                                clip: true
                                boundsBehavior: Flickable.StopAtBounds
                                TextEdit {
                                    id: proposedContentText
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
