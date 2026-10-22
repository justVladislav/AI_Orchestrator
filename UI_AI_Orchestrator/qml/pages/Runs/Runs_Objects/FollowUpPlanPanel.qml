pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

// PC-5D: the follow-up block that sits under the Outcome of a Run: `Plan next Run...` (or, for a Failed Run, `Plan recovery...`) and the
// status of the request. The Outcome block itself stays read-only; the opt-in PAID planner lives here and nowhere else.
//
// Strictly a RENDERER of `backend.followUpPlan.status` (FollowUpPlanController) plus the owner of that controller's calls for its
// dialogs. This file never decides whether a Run may be planned from (the Backend's gate says so, with a reason), never decides whether
// a request was free or possibly charged (the Backend says so), never reads a file or a plan, and has no way to start a Run: its only
// paid command is the `Authorize` of a quote the user has just read, and the only thing a confirmed plan becomes is a group of saved
// Drafts. Provider-authored text (a stored reply, the plan itself) is shown read-only, as data.
Rectangle {
    id: panel
    objectName: "followUpPlanPanel"

    required property var hostWindow
    required property var colors
    // backend.followUpPlan. Null-safe: without it the block simply stays hidden.
    property var controller: null

    // Drafts were saved and the user asked to see them (RunsPage opens its Drafts dialog).
    signal openDraftsRequested()

    readonly property var status: panel.controller ? panel.controller.status : ({ availability: "not_applicable" })
    readonly property string availability: String(panel.status.availability || "not_applicable")
    readonly property bool evaluated: panel.availability === "evaluated"
    readonly property bool unavailable: panel.availability === "unavailable"
    readonly property var nextRun: panel.evaluated ? (panel.status.nextRun || ({})) : ({})
    readonly property var recovery: panel.evaluated ? (panel.status.recovery || ({})) : ({})
    readonly property bool requestPresent: panel.evaluated && panel.status.requestPresent === true
    readonly property var request: panel.requestPresent ? (panel.status.request || ({})) : ({})
    readonly property string requestState: String(panel.request.state || "")
    readonly property bool mayBeCharged: panel.requestPresent && panel.request.mayBeCharged === true
    readonly property bool busy: panel.controller ? panel.controller.busy : false
    // The failure of the last synchronous command (cancel, discard, review), in the Backend's words.
    property string actionError: ""

    // -- Presentation only: the Backend's machine values in words ----------------------------------------------------------
    function usd(micros) {
        return "$" + (Number(micros || 0) / 1000000).toFixed(4)
    }

    function stateTitle(state) {
        switch (state) {
        case "quoted": return qsTr("Quote ready - nothing sent")
        case "authorized": return qsTr("Authorized - waiting to be sent")
        case "composing": return qsTr("Composing - the model is being called")
        case "plan_ready": return qsTr("Plan ready - review it")
        case "drafts_saved": return qsTr("Drafts saved")
        case "stale": return qsTr("Quote out of date - nothing sent")
        case "expired": return qsTr("Quote expired - nothing sent")
        case "cancelled": return qsTr("Cancelled")
        case "discarded": return qsTr("Discarded")
        case "interrupted": return qsTr("Interrupted")
        case "failed": return qsTr("Failed - no plan")
        default: return String(state)
        }
    }

    function costText(request) {
        const accounting = request.accounting || ({})
        if (request.accounting && Object.keys(accounting).length > 0) {
            return accounting.priceKnown === false
                   ? qsTr("The call was paid for; its price is not known. Tokens: %1 in, %2 out.").arg(accounting.inputTokens).arg(accounting.outputTokens)
                   : qsTr("Paid: %1 (%2 tokens in, %3 out).").arg(panel.usd(accounting.costMicros)).arg(accounting.inputTokens).arg(accounting.outputTokens)
        }
        return qsTr("The call may have been made and may still be charged, up to %1. Check the model's cost log.")
                  .arg(panel.usd((request.quote || ({})).worstCaseCostMicros))
    }

    function stateExplanation(request) {
        const state = String(request.state || "")
        const when = Qt.formatDateTime(request.quoteExpiresAt, "HH:mm")
        switch (state) {
        case "quoted": return qsTr("Nothing has been sent and nothing is charged. This quote is valid until %1.").arg(when)
        case "authorized": return qsTr("You authorized sending. It is sent once, by the application, and only until %1; until then you can still cancel at no cost.").arg(when)
        case "composing": return qsTr("The provider is being called now. Stopping the call may still be charged.")
        case "plan_ready": return qsTr("The model returned a valid plan. %1 No Drafts exist until you review the plan and confirm it.").arg(panel.costText(request))
        case "drafts_saved": return qsTr("The plan was confirmed and its Drafts were saved. %1 Nothing was started: open each draft in New Run to complete and start it yourself.").arg(panel.costText(request))
        case "stale": return qsTr("Something changed since the quote (the project, the Run, its files, the model or the price), so it was not sent. Nothing was charged. Ask for a new quote.")
        case "expired": return qsTr("The quote was not authorized in time, so it was never sent. Nothing was charged. Ask for a new quote.")
        case "cancelled": return request.mayBeCharged === true
                                 ? qsTr("Stopped while the provider was being called. %1").arg(panel.costText(request))
                                 : qsTr("Cancelled before anything was sent. Nothing was charged.")
        case "discarded": return request.mayBeCharged === true ? qsTr("The plan was dropped. %1").arg(panel.costText(request)) : qsTr("The quote was dropped. Nothing was charged.")
        case "interrupted": return qsTr("The application stopped while the model was being called. %1").arg(panel.costText(request))
        case "failed": return qsTr("The call finished, but no usable plan came out of it. %1").arg(panel.costText(request))
        default: return ""
        }
    }

    function stateColor(state) {
        if (state === "failed") return panel.colors.error
        if (state === "plan_ready" || state === "drafts_saved") return panel.colors.success
        if (state === "composing" || state === "authorized" || state === "quoted") return panel.colors.warning
        return panel.colors.textMuted
    }

    function errorLine(result) {
        return qsTr("%1 (%2)").arg(String(result.errorMessage || "")).arg(String(result.errorCode || ""))
    }

    // ---- the quote dialog -------------------------------------------------------------------------------------------------
    property real pendingDescribe: 0
    property real pendingQuote: 0
    property real pendingAuthorize: 0

    function openRequestDialog(kind) {
        if (!panel.controller)
            return
        panel.actionError = ""
        quoteDialog.reset()
        quoteDialog.kind = kind
        quoteDialog.projectId = String(panel.status.projectId || "")
        quoteDialog.sourceRunId = String(panel.status.runId || "")
        const models = panel.controller.modelChoices()
        if (models.ok) {
            quoteDialog.models = models.models
            quoteDialog.selectFirstUsableModel()
        } else {
            quoteDialog.modelError = panel.errorLine(models)
        }
        quoteDialog.filesLoading = true
        quoteDialog.pathsToRestore = null
        panel.pendingDescribe = panel.controller.describeFiles(quoteDialog.projectId, quoteDialog.sourceRunId, kind)
        if (panel.pendingDescribe === 0) {
            quoteDialog.filesLoading = false
            quoteDialog.filesError = String(panel.controller.lastError)
        }
        quoteDialog.open()
    }

    // Re-opens a quote that is still waiting: its numbers, files and the Authorize button, exactly as stored.
    function openStoredQuote() {
        if (!panel.controller || panel.requestState !== "quoted")
            return
        panel.actionError = ""
        quoteDialog.reset()
        quoteDialog.kind = String(panel.request.kind)
        quoteDialog.projectId = String(panel.request.projectId)
        quoteDialog.sourceRunId = String(panel.request.sourceRunId)
        quoteDialog.quote = panel.request
        quoteDialog.open()
    }

    // A quote reopened from durable state has no in-memory form behind it. Rebuild that form from the quote's immutable fields and
    // re-read the current model/file choices; asking for the replacement remains free and the Backend atomically discards the old
    // quoted request only after it has built the new one successfully.
    function changeStoredQuote(request) {
        if (!panel.controller || !request)
            return
        quoteDialog.errorText = ""
        const models = panel.controller.modelChoices()
        if (models.ok) {
            quoteDialog.models = models.models
            quoteDialog.modelError = ""
        } else {
            quoteDialog.models = []
            quoteDialog.modelError = panel.errorLine(models)
        }
        quoteDialog.restoreForEditing(request)
        quoteDialog.files = []
        quoteDialog.filesError = ""
        quoteDialog.filesLoading = true
        panel.pendingDescribe = panel.controller.describeFiles(
                    String(request.projectId), String(request.sourceRunId), String(request.kind))
        if (panel.pendingDescribe === 0) {
            quoteDialog.filesLoading = false
            quoteDialog.pathsToRestore = null
            quoteDialog.filesError = String(panel.controller.lastError)
        }
    }

    function handleFinished(operationId, operation, result) {
        if (operation === "describeFiles" && operationId === panel.pendingDescribe) {
            panel.pendingDescribe = 0
            quoteDialog.filesLoading = false
            if (result.ok) {
                quoteDialog.files = result.files
                const restored = quoteDialog.pathsToRestore
                quoteDialog.checkedPaths = restored === null
                        ? result.files.filter(file => file.includedByDefault === true).map(file => file.path)
                        : result.files.filter(file => file.eligible === true
                                               && restored.indexOf(String(file.path)) >= 0).map(file => file.path)
                quoteDialog.pathsToRestore = null
                quoteDialog.filesError = ""
            } else {
                quoteDialog.files = []
                quoteDialog.checkedPaths = []
                quoteDialog.pathsToRestore = null
                quoteDialog.filesError = panel.errorLine(result)
            }
        } else if (operation === "requestQuote" && operationId === panel.pendingQuote) {
            panel.pendingQuote = 0
            if (result.ok) {
                quoteDialog.errorText = ""
                quoteDialog.quote = result.request
            } else {
                quoteDialog.errorText = panel.errorLine(result)
            }
        } else if (operation === "authorize" && operationId === panel.pendingAuthorize) {
            panel.pendingAuthorize = 0
            if (result.ok) {
                quoteDialog.errorText = ""
                quoteDialog.quote = null
                quoteDialog.close()
            } else {
                quoteDialog.errorText = panel.errorLine(result)
                // The quote cannot be authorized any more (a change or its time): the only way forward is a new quote.
                const code = String(result.errorCode || "")
                if (code.indexOf("follow_up.stale_") === 0 || code === "follow_up.quote_expired")
                    quoteDialog.quote = null
            }
        }
    }

    Connections {
        target: panel.controller
        ignoreUnknownSignals: true
        function onOperationFinished(operationId, operation, result) {
            panel.handleFinished(operationId, operation, result)
        }
    }

    // ---- commands on the request --------------------------------------------------------------------------------------------
    function cancelRequest() {
        panel.actionError = ""
        const result = panel.controller.cancel(String(panel.request.id))
        if (!result.ok)
            panel.actionError = panel.errorLine(result)
    }

    function discardRequest() {
        panel.actionError = ""
        const result = panel.controller.discard(String(panel.request.id))
        if (!result.ok)
            panel.actionError = panel.errorLine(result)
    }

    function viewStoredReply() {
        panel.actionError = ""
        const result = panel.controller.storedReply(String(panel.request.id))
        if (!result.ok) {
            panel.actionError = panel.errorLine(result)
            return
        }
        replyDialog.failureCode = String(result.code)
        replyDialog.failureMessage = String(result.message)
        replyDialog.replyText = String(result.reply)
        replyDialog.open()
    }

    // ---- the plan review (the PC-3D dialog) ----------------------------------------------------------------------------------
    readonly property var stalePlanCodes: ["follow_up.plan_changed", "follow_up.not_plan_ready"]

    function applyReview(result) {
        reviewDialog.creation = null
        reviewDialog.offerPreviewAgain = false
        if (!result.ok) {
            reviewDialog.preview = null
            reviewDialog.errorText = panel.errorLine(result)
            return false
        }
        reviewDialog.errorText = ""
        reviewDialog.selectedRunIds = []
        reviewDialog.preview = result
        panel.loadReviewChoices(String(result.projectId))
        return true
    }

    // The two pickers of the dialog: a failure is shown (and can be retried), never taken for "nothing to choose".
    function loadReviewChoices(projectId) {
        const templates = panel.controller.rosterTemplates(projectId)
        const runs = panel.controller.sourceRunChoices(projectId, String(reviewDialog.lockedRunId || ""))
        const problems = []
        if (!templates.ok)
            problems.push(qsTr("The roster templates could not be loaded: %1").arg(panel.errorLine(templates)))
        if (!runs.ok)
            problems.push(qsTr("The source Runs could not be loaded: %1").arg(panel.errorLine(runs)))
        reviewDialog.templates = templates.ok ? templates.templates : []
        reviewDialog.sourceRuns = runs.ok ? runs.runs : []
        reviewDialog.choiceErrors = problems
        reviewDialog.selectedRunIds = reviewDialog.selectedRunIds.filter(
                    id => reviewDialog.sourceRuns.some(run => run.runId === id))
    }

    function openReview() {
        if (!panel.controller || panel.requestState !== "plan_ready")
            return
        panel.actionError = ""
        reviewDialog.reset()
        reviewDialog.mode = "follow_up"
        reviewDialog.lockedRunId = String(panel.request.sourceRunId)
        panel.applyReview(panel.controller.preview(String(panel.request.id)))
        reviewDialog.open()
    }

    function reviewAgain() {
        if (reviewDialog.preview === null && panel.requestPresent)
            panel.applyReview(panel.controller.preview(String(panel.request.id)))
        else if (reviewDialog.preview !== null)
            panel.applyReview(panel.controller.preview(String(reviewDialog.preview.requestId)))
    }

    // The one write: the plan the user was shown (its fingerprint), an optional template and optional further source Runs.
    function confirmReview(requestId, templateDraftId, sourceRunIds, applySuggestedParallelism) {
        if (reviewDialog.preview === null)
            return
        const result = panel.controller.confirm(requestId, String(reviewDialog.preview.planFingerprint), templateDraftId, sourceRunIds,
                                                applySuggestedParallelism === true)
        if (!result.ok) {
            reviewDialog.errorText = panel.errorLine(result)
            reviewDialog.offerPreviewAgain = panel.stalePlanCodes.indexOf(String(result.errorCode)) >= 0
            return
        }
        reviewDialog.errorText = ""
        reviewDialog.offerPreviewAgain = false
        reviewDialog.creation = result
    }

    visible: panel.evaluated || panel.unavailable
    Layout.fillWidth: true
    Layout.preferredHeight: visible ? followUpColumn.implicitHeight + panel.hostWindow.dp(20) : 0
    radius: panel.colors.cornerRadiusSmall
    color: panel.unavailable ? panel.colors.warningSoft : panel.colors.surface
    border.width: 1
    border.color: panel.unavailable ? panel.colors.warning : panel.colors.border

    ColumnLayout {
        id: followUpColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: panel.hostWindow.dp(11)
        anchors.rightMargin: panel.hostWindow.dp(11)
        spacing: panel.hostWindow.dp(6)

        RowLayout {
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(8)
            Text {
                text: qsTr("Follow-up plan")
                color: panel.colors.textPrimary
                font.pixelSize: panel.hostWindow.dp(13)
                font.weight: Font.DemiBold
            }
            Text {
                objectName: "followUpPlanState"
                Layout.fillWidth: true
                visible: panel.requestPresent
                text: panel.requestPresent ? panel.stateTitle(panel.requestState) : ""
                color: panel.requestPresent ? panel.stateColor(panel.requestState) : panel.colors.textMuted
                elide: Text.ElideRight
                font.pixelSize: panel.hostWindow.dp(11.5)
                font.weight: Font.DemiBold
            }
            Item { Layout.fillWidth: true; visible: !panel.requestPresent }
        }

        // A fault (also a runtime that does not own the database), shown as a fault.
        Text {
            objectName: "followUpPlanUnavailable"
            Layout.fillWidth: true
            visible: panel.unavailable
            text: panel.unavailable ? qsTr("Follow-up planning is not available: %1 (%2)")
                                          .arg(String(panel.status.errorMessage || "")).arg(String(panel.status.errorCode || "")) : ""
            textFormat: Text.PlainText
            color: panel.colors.warning
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(10)
        }

        // ---- the request ---------------------------------------------------------------------------------------------
        Text {
            objectName: "followUpPlanExplanation"
            Layout.fillWidth: true
            visible: panel.requestPresent
            text: panel.requestPresent ? panel.stateExplanation(panel.request) : ""
            color: panel.colors.textSecondary
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(10)
        }
        Text {
            objectName: "followUpPlanFailure"
            Layout.fillWidth: true
            visible: panel.requestPresent && panel.requestState === "failed"
            readonly property var failure: panel.requestPresent ? (panel.request.failure || ({})) : ({})
            text: qsTr("%1: %2").arg(String(failure.code || "")).arg(String(failure.message || ""))
            textFormat: Text.PlainText
            color: panel.colors.error
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(10)
        }
        Text {
            objectName: "followUpPlanGoal"
            Layout.fillWidth: true
            visible: panel.requestPresent
            text: panel.requestPresent ? qsTr("Goal: %1").arg(String(panel.request.goal || "")) : ""
            textFormat: Text.PlainText
            color: panel.colors.textMuted
            wrapMode: Text.WordWrap
            maximumLineCount: 3
            elide: Text.ElideRight
            font.pixelSize: panel.hostWindow.dp(9.5)
        }

        RowLayout {
            Layout.fillWidth: true
            visible: panel.requestPresent
            spacing: panel.hostWindow.dp(8)

            RunActionButton {
                objectName: "followUpOpenQuoteButton"
                visible: panel.requestState === "quoted"
                hostWindow: panel.hostWindow
                colors: panel.colors
                text: qsTr("Open quote")
                primary: true
                enabled: !panel.busy
                onClicked: panel.openStoredQuote()
            }
            RunActionButton {
                objectName: "followUpDiscardQuoteButton"
                visible: panel.requestState === "quoted"
                hostWindow: panel.hostWindow
                colors: panel.colors
                text: qsTr("Discard quote")
                enabled: !panel.busy
                onClicked: panel.discardRequest()
            }
            RunActionButton {
                objectName: "followUpCancelButton"
                visible: panel.requestState === "authorized" || panel.requestState === "composing"
                hostWindow: panel.hostWindow
                colors: panel.colors
                text: panel.requestState === "composing" ? qsTr("Stop (may still be charged)") : qsTr("Cancel (free)")
                danger: true
                onClicked: panel.cancelRequest()
            }
            RunActionButton {
                objectName: "followUpReviewPlanButton"
                visible: panel.requestState === "plan_ready"
                hostWindow: panel.hostWindow
                colors: panel.colors
                text: qsTr("Review plan")
                primary: true
                onClicked: panel.openReview()
            }
            RunActionButton {
                objectName: "followUpDiscardPlanButton"
                visible: panel.requestState === "plan_ready"
                hostWindow: panel.hostWindow
                colors: panel.colors
                text: qsTr("Discard plan")
                danger: true
                onClicked: panel.discardRequest()
            }
            RunActionButton {
                objectName: "followUpOpenDraftsButton"
                visible: panel.requestState === "drafts_saved"
                hostWindow: panel.hostWindow
                colors: panel.colors
                text: qsTr("Open Drafts")
                primary: true
                onClicked: panel.openDraftsRequested()
            }
            RunActionButton {
                objectName: "followUpViewReplyButton"
                visible: panel.requestState === "failed" && (panel.request.failure || ({})).hasStoredReply === true
                hostWindow: panel.hostWindow
                colors: panel.colors
                text: qsTr("View stored reply")
                onClicked: panel.viewStoredReply()
            }
            Item { Layout.fillWidth: true }
        }

        // The new plan comes AFTER the status of the one that exists: what happened first, what can be done next.
        // ---- the commands -------------------------------------------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            visible: panel.evaluated && (panel.nextRun.offered === true || panel.recovery.offered === true)
            spacing: panel.hostWindow.dp(8)

            RunActionButton {
                objectName: "planNextRunButton"
                visible: panel.nextRun.offered === true
                hostWindow: panel.hostWindow
                colors: panel.colors
                text: qsTr("Plan next Run...")
                primary: true
                enabled: panel.nextRun.allowed === true && !panel.busy
                onClicked: panel.openRequestDialog("next_run")
            }
            RunActionButton {
                objectName: "planRecoveryButton"
                visible: panel.recovery.offered === true
                hostWindow: panel.hostWindow
                colors: panel.colors
                text: qsTr("Plan recovery...")
                primary: true
                enabled: panel.recovery.allowed === true && !panel.busy
                onClicked: panel.openRequestDialog("recovery")
            }
            Item { Layout.fillWidth: true }
        }

        // Why a plan is not possible right now, in the Backend's own words (a request that holds the slot is shown below instead).
        Text {
            objectName: "followUpPlanBlocked"
            Layout.fillWidth: true
            readonly property var blocked: panel.nextRun.offered === true && panel.nextRun.allowed !== true ? panel.nextRun
                                           : panel.recovery.offered === true && panel.recovery.allowed !== true ? panel.recovery : null
            visible: panel.evaluated && blocked !== null && String(blocked.reasonCode || "") !== "follow_up.request_open"
            text: blocked !== null ? qsTr("Not available for this Run: %1").arg(String(blocked.reasonMessage || blocked.reasonCode || "")) : ""
            textFormat: Text.PlainText
            color: panel.colors.textSecondary
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(10)
        }

        Text {
            objectName: "followUpPlanActionError"
            Layout.fillWidth: true
            visible: panel.actionError.length > 0
            text: panel.actionError
            textFormat: Text.PlainText
            color: panel.colors.error
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(10)
        }

        Text {
            Layout.fillWidth: true
            visible: panel.evaluated && (panel.nextRun.offered === true || panel.recovery.offered === true)
            text: qsTr("Planning asks a paid model and always shows you a quote first. It never starts a Run: a confirmed plan only saves Drafts, which you complete and start yourself.")
            color: panel.colors.textMuted
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(8.5)
        }
    }

    // Direct children of the root Rectangle (not of a Layout), like the dialogs of the neighbouring panels.
    FollowUpPlanDialog {
        id: quoteDialog
        hostWindow: panel.hostWindow
        colors: panel.colors
        busy: panel.busy
        busyOperation: panel.controller ? String(panel.controller.busyOperation) : ""

        onQuoteRequested: intent => {
            panel.pendingQuote = panel.controller.requestQuote(intent)
            if (panel.pendingQuote === 0)
                quoteDialog.errorText = String(panel.controller.lastError)
        }
        onAuthorizeRequested: requestId => {
            panel.pendingAuthorize = panel.controller.authorize(requestId)
            if (panel.pendingAuthorize === 0)
                quoteDialog.errorText = String(panel.controller.lastError)
        }
        onChangeRequested: request => panel.changeStoredQuote(request)
    }

    FollowUpReplyDialog {
        id: replyDialog
        hostWindow: panel.hostWindow
        colors: panel.colors
    }

    NextRunPlanDialog {
        id: reviewDialog
        // The Prompt Workshop in Chats has its own instance of this dialog: this one has its own name.
        objectName: "followUpReviewDialog"
        hostWindow: panel.hostWindow
        colors: panel.colors
        offersSuggestedParallelism: true

        onPreviewAgainRequested: panel.reviewAgain()
        onReloadChoicesRequested: panel.loadReviewChoices(reviewDialog.preview ? String(reviewDialog.preview.projectId) : "")
        onConfirmRequested: (previewToken, templateDraftId, sourceRunIds, applySuggestedParallelism) =>
                            panel.confirmReview(previewToken, templateDraftId, sourceRunIds, applySuggestedParallelism)
        onOpenDraftsRequested: panel.openDraftsRequested()
    }
}
