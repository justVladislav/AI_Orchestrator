pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: panel
    objectName: "runDetailsPanel"
    required property var hostWindow
    required property var colors
    property var runData: null
    // Optional: the backend.agentSessions controller. Null-safe so any
    // existing RunDetailsPanel instantiation that does not pass it keeps
    // working unchanged; the embedded AgentSessionPanel simply stays hidden.
    property var agentSessionController: null
    // Optional: the backend.manifest controller. Same null-safety as
    // agentSessionController above; the embedded ManifestPanel simply stays
    // hidden (and does stay hidden for every Mock/execution-profile run,
    // which never freezes a manifest) when unset or empty.
    property var manifestController: null
    // Optional: the dedicated Backend manual-handoff projection/command
    // controller. It stays separate from ManifestController so this panel
    // cannot accidentally blend the single-use paid continuation path with
    // provider-free user-authored repair.
    property var manualHandoffController: null
    // PC-4D: the backend.outcomeBrief controller of the read-only Outcome block. Null-safe like the controllers above: without it the
    // block simply stays hidden.
    property var outcomeBriefController: null
    // PC-5D: the backend.followUpPlan controller of the opt-in paid planner (`Plan next Run...`), shown under the Outcome. Null-safe: without
    // it the block simply stays hidden.
    property var followUpPlanController: null
    // Drafts were saved by a confirmed follow-up plan and the user asked to see them.
    signal openDraftsRequested()
    // backend.runs (RunController) - passed through to AgentSessionPanel/
    // ManifestPanel so they can react to runUpdated(runId) instead of
    // polling on a Timer.
    property var runsController: null
    // Optional: true when a calendar day is selected and that day has zero
    // runs (RunsPage's ensureSelectionMatchesProjectFilter() keeps
    // selection empty in that case). Swaps the generic "Select a run" empty
    // state below for a date-specific one instead of leaving the reason
    // unexplained.
    property bool selectedDateHasNoRuns: false
    // Ticked once a second by RunsPage while any run is active. Reading this
    // from liveDuration() below is what keeps the elapsed-time display alive
    // without RunsPage rebuilding its entire run-list snapshot every second
    // just for this one number - see RunsPage.qml's nowMs property.
    property real nowMs: Date.now()
    // Long objectives/contexts are clamped by default so the panel does not
    // become an endless scroll; the toggle reveals the full text on demand.
    property bool contextExpanded: false
    property string contextRunId: ""
    // PC-2: how many interface contracts the selected Run froze (0 when it has none).
    readonly property int contractCount: runData && runData.interfaceContracts
                                          ? runData.interfaceContracts.length : 0
    readonly property string providerBudgetStopCode:
        String(panel.runData && panel.runData.providerBudgetStopCode || "")
    readonly property bool providerUsageUnavailable:
        panel.runData && panel.runData.providerUsageAuthoritative === false
    onRunDataChanged: {
        const nextRunId = runData ? runData.runId : ""
        if (nextRunId !== contextRunId)
            contextExpanded = false
        contextRunId = nextRunId
    }
    readonly property real detailMaximumY: Math.max(
        0, detailFlickable.contentHeight - detailFlickable.height)
    readonly property bool showJumpToBottom: panel.contextExpanded
        && panel.detailMaximumY > panel.hostWindow.dp(24)
        && detailFlickable.contentY > panel.hostWindow.dp(12)
        && detailFlickable.contentY < panel.detailMaximumY - panel.hostWindow.dp(12)

    signal cancelRequested(string runId)
    signal retryRequested(string runId)
    signal editRequested(string runId)
    signal artifactRequested(string runId, string artifactId)
    signal artifactsFolderRequested(string runId, string artifactId)
    signal viewContextRequested(string runId, string artifactId)
    signal compareRequested(string runId)

    readonly property var runWarnings: panel.runData && panel.runData.warnings ? panel.runData.warnings : []

    // The single context-snapshot artifact for this run, if any - drives both
    // the dedicated "View context" button and the artifact list's special
    // click handling. "context_snapshot" matches ArtifactKind::ContextSnapshot
    // (see ArtifactTypes.cpp toString()), mirrored into QML as artifact.type
    // by RunsPage.qml's rebuildArtifactsSnapshot().
    readonly property var contextSnapshotArtifact: {
        if (!panel.runData) return null
        const artifacts = panel.runData.artifacts || []
        for (let i = 0; i < artifacts.length; ++i) {
            if (artifacts[i].type === "context_snapshot")
                return artifacts[i]
        }
        return null
    }
    // Planner no-op and rejection reports are durable artifacts attached to
    // the planning stage. Surface the report as a first-class Run Details
    // diagnostic instead of making the user discover it in the generic list.
    readonly property var plannerReportArtifact: {
        if (!panel.runData) return null
        const artifacts = panel.runData.artifacts || []
        for (let i = 0; i < artifacts.length; ++i) {
            if (artifacts[i].name === "manifest-planning-no-changes.md"
                    || artifacts[i].name === "manifest-planning-diagnostics.md")
                return artifacts[i]
        }
        return null
    }
    readonly property var manifestVerificationArtifact: {
        if (!panel.runData) return null
        const artifacts = panel.runData.artifacts || []
        for (let i = 0; i < artifacts.length; ++i) {
            if (artifacts[i].name === "manifest-verification.md")
                return artifacts[i]
        }
        return null
    }
    readonly property var workspaceBaselineArtifact: {
        if (!panel.runData) return null
        const artifacts = panel.runData.artifacts || []
        for (let i = 0; i < artifacts.length; ++i) {
            if (artifacts[i].name === "workspace-baseline.v1.json")
                return artifacts[i]
        }
        return null
    }
    readonly property var workspaceProvenanceArtifact: {
        if (!panel.runData) return null
        const artifacts = panel.runData.artifacts || []
        for (let i = 0; i < artifacts.length; ++i) {
            if (artifacts[i].name === "manifest-workspace-apply-provenance.md")
                return artifacts[i]
        }
        for (let i = 0; i < artifacts.length; ++i) {
            if (artifacts[i].name === "manifest-workspace-provenance.md")
                return artifacts[i]
        }
        return null
    }

    radius: colors.cornerRadiusMedium
    color: colors.surfaceAlternative
    border.width: 1
    border.color: colors.border

    // Recomputed from the raw timestamps + panel.nowMs, instead of reading
    // runData.duration (a string baked once by RunsPage.rebuildRunsSnapshot()
    // and otherwise only refreshed when some other field changes) - this is
    // what lets the elapsed-time display tick every second on its own,
    // without RunsPage rebuilding the entire run list snapshot for it. Same
    // formatting as RunsPage.qml's formatDuration(), duplicated locally so
    // this panel does not need a reference back to the page.
    function liveDuration(start, end) {
        if (!start)
            return "?"
        const endTime = end ? end.getTime() : panel.nowMs
        let totalSeconds = Math.max(0, Math.round((endTime - start.getTime()) / 1000))
        const hours = Math.floor(totalSeconds / 3600)
        totalSeconds -= hours * 3600
        const minutes = Math.floor(totalSeconds / 60)
        const seconds = totalSeconds - minutes * 60
        const pad = n => String(n).padStart(2, "0")
        return hours > 0 ? (pad(hours) + ":" + pad(minutes) + ":" + pad(seconds))
                          : (pad(minutes) + ":" + pad(seconds))
    }

    function statusColor(status) {
        if (status === "prepared") return colors.warning
        if (status === "running") return colors.information
        if (status === "completed") return colors.success
        if (status === "failed") return colors.error
        if (status === "queued") return colors.warning
        return colors.textMuted
    }

    function budgetNumber(value) {
        return Number(value || 0).toLocaleString(Qt.locale(), "f", 0)
    }

    // Live process indicator: while a run is "running" at the run level, the
    // stage timeline (panel.runData.stages) is the only source that says
    // *what* is actually happening right now, so this label is derived from
    // it rather than duplicating/guessing progress. Never fabricates a stage
    // name or percentage the backend hasn't reported.
    readonly property var activeStage: {
        if (!panel.runData || panel.runData.status !== "running")
            return null
        const stages = panel.runData.stages || []
        for (let i = 0; i < stages.length; ++i) {
            if (stages[i].state === "running")
                return stages[i]
        }
        return null
    }

    readonly property bool anyStageStarted: {
        const stages = panel.runData ? (panel.runData.stages || []) : []
        return stages.some(s => s.state !== "pending")
    }

    readonly property string phaseLabel: {
        if (!panel.runData)
            return ""
        const status = panel.runData.status
        if (status === "queued")
            return qsTr("Queued — waiting to start")
        if (status !== "running")
            return panel.runData.statusLabel
        if (panel.activeStage)
            return qsTr("Running: %1").arg(panel.activeStage.name)
        return panel.anyStageStarted ? qsTr("Preparing next stage…") : qsTr("Preparing…")
    }

    readonly property string phaseDetail: {
        if (!panel.runData || panel.runData.status !== "running")
            return ""
        if (panel.activeStage)
            return qsTr("%1 · elapsed %2").arg(
                       panel.activeStage.detail.length > 0
                       ? panel.activeStage.detail
                       : qsTr("Waiting for provider response"))
                   .arg(panel.liveDuration(panel.runData.startedAt, panel.runData.finishedAt))
        return qsTr("Waiting for next stage")
    }

    ColumnLayout {
        anchors.centerIn: parent
        visible: panel.runData === null
        spacing: panel.hostWindow.dp(8)
        Text { Layout.alignment: Qt.AlignHCenter; text: "▶"; color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(28) }
        Text {
            Layout.alignment: Qt.AlignHCenter
            text: panel.selectedDateHasNoRuns ? qsTr("No runs on this date") : qsTr("Select a run")
            color: panel.colors.textPrimary
            font.pixelSize: panel.hostWindow.dp(13)
            font.weight: Font.Medium
        }
        Text {
            Layout.alignment: Qt.AlignHCenter
            text: panel.selectedDateHasNoRuns
                  ? qsTr("Start the first run for this project or choose another date.")
                  : qsTr("Stages, artifacts and logs will appear here.")
            color: panel.colors.textMuted
            font.pixelSize: panel.hostWindow.dp(9)
        }
    }

    Flickable {
        id: detailFlickable
        objectName: "runDetailsFlickable"
        anchors.fill: parent
        anchors.margins: panel.hostWindow.dp(14)
        visible: panel.runData !== null
        contentWidth: width
        contentHeight: detailColumn.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: RunScrollBar {
            hostWindow: panel.hostWindow
            colors: panel.colors
        }

        function jumpToBottom() {
            contentY = Math.max(0, contentHeight - height)
        }

        ColumnLayout {
            id: detailColumn
            width: Math.max(0, parent.width - panel.hostWindow.dp(14))
            spacing: panel.hostWindow.dp(12)

            RowLayout {
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(8)
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: panel.hostWindow.dp(3)
                    // Read-only TextEdit (not Text) so the name and ID can be
                    // selected with the mouse; the Copy links below copy them
                    // verbatim in one click.
                    TextEdit {
                        objectName: "runDetailsTitleText"
                        Layout.fillWidth: true
                        readOnly: true
                        selectByMouse: true
                        text: panel.runData ? panel.runData.title : ""
                        color: panel.colors.textPrimary
                        selectionColor: panel.colors.accent
                        selectedTextColor: panel.colors.textOnAccent
                        font.pixelSize: panel.hostWindow.dp(16)
                        font.weight: Font.DemiBold
                        wrapMode: TextEdit.WrapAnywhere
                    }
                    TextEdit {
                        objectName: "runDetailsIdText"
                        Layout.fillWidth: true
                        readOnly: true
                        selectByMouse: true
                        text: panel.runData ? panel.runData.runId + " · " + panel.runData.projectName : ""
                        color: panel.colors.textMuted
                        selectionColor: panel.colors.accent
                        selectedTextColor: panel.colors.textOnAccent
                        font.pixelSize: panel.hostWindow.dp(9)
                        wrapMode: TextEdit.WrapAnywhere
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: panel.hostWindow.dp(12)
                        RunCopyButton {
                            objectName: "runDetailsCopyNameButton"
                            hostWindow: panel.hostWindow
                            colors: panel.colors
                            label: qsTr("Copy name")
                            copyText: panel.runData ? panel.runData.title : ""
                        }
                        RunCopyButton {
                            objectName: "runDetailsCopyIdButton"
                            hostWindow: panel.hostWindow
                            colors: panel.colors
                            label: qsTr("Copy ID")
                            copyText: panel.runData ? panel.runData.runId : ""
                        }
                        Item { Layout.fillWidth: true }
                    }
                }
                Rectangle {
                    Layout.preferredWidth: statusText.implicitWidth + panel.hostWindow.dp(18)
                    Layout.preferredHeight: panel.hostWindow.dp(27)
                    radius: height / 2
                    color: panel.runData && panel.runData.status === "failed" ? panel.colors.errorSoft
                           : panel.runData && panel.runData.status === "completed" ? panel.colors.successSoft
                           : panel.colors.accentSoft
                    Text { id: statusText; anchors.centerIn: parent; text: panel.runData ? panel.runData.statusLabel : ""; color: panel.runData ? panel.statusColor(panel.runData.status) : panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(9); font.weight: Font.DemiBold }
                }
            }

            // Live process indicator: Queued / Preparing… / Running: <stage> /
            // Waiting for next stage / Waiting for provider response. Driven
            // entirely by panel.runData.status and the stage timeline that
            // RunController refreshes on every backend RunEvent - it never
            // holds its own copy of run state.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: phaseColumn.implicitHeight + panel.hostWindow.dp(18)
                visible: panel.runData && (panel.runData.status === "queued" || panel.runData.status === "running")
                radius: panel.colors.cornerRadiusSmall
                color: panel.colors.accentSoft
                border.width: 1
                border.color: panel.colors.accent

                ColumnLayout {
                    id: phaseColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: panel.hostWindow.dp(12)
                    anchors.rightMargin: panel.hostWindow.dp(12)
                    spacing: panel.hostWindow.dp(2)

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: panel.hostWindow.dp(7)
                        Rectangle {
                            Layout.preferredWidth: panel.hostWindow.dp(7)
                            Layout.preferredHeight: panel.hostWindow.dp(7)
                            radius: width / 2
                            color: panel.colors.accent
                        }
                        Text {
                            Layout.fillWidth: true
                            text: panel.phaseLabel
                            color: panel.colors.accent
                            font.pixelSize: panel.hostWindow.dp(11)
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: panel.phaseDetail.length > 0
                        text: panel.phaseDetail
                        color: panel.colors.textSecondary
                        font.pixelSize: panel.hostWindow.dp(9)
                        elide: Text.ElideRight
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(8)
                RunActionButton { visible: panel.runData && (panel.runData.status === "running" || panel.runData.status === "queued"); hostWindow: panel.hostWindow; colors: panel.colors; text: qsTr("Cancel"); danger: true; onClicked: panel.cancelRequested(panel.runData.runId) }
                RunActionButton { visible: panel.runData && (panel.runData.status === "failed" || panel.runData.status === "cancelled" || panel.runData.status === "completed"); hostWindow: panel.hostWindow; colors: panel.colors; text: qsTr("Retry"); iconText: "↻"; primary: true; onClicked: panel.retryRequested(panel.runData.runId) }
                RunActionButton { visible: panel.runData && (panel.runData.status === "failed" || panel.runData.status === "cancelled" || panel.runData.status === "completed"); hostWindow: panel.hostWindow; colors: panel.colors; text: qsTr("Edit & rerun"); iconText: "✎"; onClicked: panel.editRequested(panel.runData.runId) }
                RunActionButton { visible: panel.runData && panel.runData.status === "completed"; hostWindow: panel.hostWindow; colors: panel.colors; text: qsTr("Compare results"); iconText: "⇄"; primary: true; onClicked: panel.compareRequested(panel.runData.runId) }
                Item { Layout.fillWidth: true }
            }

            // What the failed verification recorded, BEFORE both repair paths below (the paid `Prepare paid verification repair`
            // in ManifestPanel and the free `Create repair workspace` in ManualHandoffPanel), so both see the same findings.
            // Information only: it classifies nothing itself and gates neither button.
            RecordedFindingsBlock {
                Layout.fillWidth: true
                hostWindow: panel.hostWindow
                colors: panel.colors
                controller: panel.manualHandoffController
            }

            // A Prepared verification-continuation child is an explicit
            // operator decision point, not ordinary historical detail. Keep
            // the Backend-owned continuation action near the Run header so a
            // user never has to scroll past stages/artifacts/logs to discover
            // the paid-repair confirmation. The same panel still owns all
            // Manifest/environment detail; only its position changes.
            ManifestPanel {
                Layout.fillWidth: true
                visible: panel.manifestController !== null
                    && (panel.manifestController.hasManifest
                        || panel.manifestController.hasVerificationEnvironment
                        || panel.manifestController.hasVerificationContinuation)
                hostWindow: panel.hostWindow
                colors: panel.colors
                controller: panel.manifestController
                runsController: panel.runsController
                runId: panel.runData ? panel.runData.runId : ""
            }

            ManualHandoffPanel {
                Layout.fillWidth: true
                visible: panel.manualHandoffController !== null
                         && panel.manualHandoffController.isApplicable
                hostWindow: panel.hostWindow
                colors: panel.colors
                controller: panel.manualHandoffController
                runsController: panel.runsController
                runId: panel.runData ? panel.runData.runId : ""
            }

            // PC-4D: the read-only Outcome of the selected Run (is what it produced resolved?) - only for a terminal outcome Run; no
            // action of any kind lives here.
            OutcomeBriefPanel {
                hostWindow: panel.hostWindow
                colors: panel.colors
                controller: panel.outcomeBriefController
            }

            // PC-5D: the opt-in PAID planner (never starts a Run) and the status of its request.
            FollowUpPlanPanel {
                hostWindow: panel.hostWindow
                colors: panel.colors
                controller: panel.followUpPlanController
                onOpenDraftsRequested: panel.openDraftsRequested()
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 4
                columnSpacing: panel.hostWindow.dp(8)
                Repeater {
                    model: panel.runData ? [
                        { "label": qsTr("Provider"), "value": panel.runData.provider },
                        { "label": qsTr("Duration"),
                          "value": panel.liveDuration(panel.runData.startedAt, panel.runData.finishedAt) },
                        { "label": qsTr("Tokens"), "value": panel.runData.tokens },
                        { "label": qsTr("Stages"), "value": panel.runData.stagesLabel }
                    ] : []
                    delegate: Rectangle {
                        id: metricItem
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredHeight: panel.hostWindow.dp(60)
                        radius: panel.colors.cornerRadiusSmall
                        color: panel.colors.surface
                        ColumnLayout {
                            anchors.fill: parent; anchors.margins: panel.hostWindow.dp(9); spacing: panel.hostWindow.dp(3)
                            Text { text: metricItem.modelData.label; color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(8) }
                            Text { Layout.fillWidth: true; text: metricItem.modelData.value; color: panel.colors.textPrimary; font.pixelSize: panel.hostWindow.dp(10.5); font.weight: Font.Medium; elide: Text.ElideRight }
                        }
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("Provider budget")
                color: panel.colors.textPrimary
                font.pixelSize: panel.hostWindow.dp(13)
                font.weight: Font.DemiBold
            }

            Rectangle {
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
                              ? qsTr("Input %1 / %2 · output %3 / %4 · turns %5 / %6")
                                .arg(panel.budgetNumber(panel.runData.providerBudgetInputTokens))
                                .arg(panel.budgetNumber(panel.runData.providerBudgetMaxInputTokens))
                                .arg(panel.budgetNumber(panel.runData.providerBudgetOutputTokens))
                                .arg(panel.budgetNumber(panel.runData.providerBudgetMaxOutputTokens))
                                .arg(panel.budgetNumber(panel.runData.providerTurnsStarted))
                                .arg(panel.budgetNumber(panel.runData.providerBudgetMaxTurns))
                                + (Number(panel.runData.providerBudgetTurnsRaisedFrom || 0) > 0
                                   ? qsTr(" (raised automatically from %1)")
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
                              ? qsTr("Tool calls %1 / %2 · returned %3 / %4 bytes · request timeout %5 s · Run limit %6 min")
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
                              ? qsTr("Prompt cache: %1 written · %2 read (read tokens do not count toward the input limit above)")
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
                              ? qsTr("Raw input %1 = non-cache input %2 + cache write %3 + cache read %4 · output %5")
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
                                 ? qsTr("Stopped by Backend: %1").arg(panel.providerBudgetStopCode)
                                 : qsTr("Provider usage is unavailable; execution is fail-closed."))
                              : ""
                        color: panel.colors.error
                        font.pixelSize: panel.hostWindow.dp(8.5)
                        wrapMode: Text.WordWrap
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Cost enforcement is unavailable until a trusted versioned pricing snapshot is present; token limits remain authoritative.")
                        color: panel.colors.textMuted
                        font.pixelSize: panel.hostWindow.dp(8)
                        wrapMode: Text.WordWrap
                    }
                }
            }

            // PC-2: the interface contracts and constraints this Run froze. The Backend delivers each one,
            // word for word, to the workers it applies to; this is a read-only view of them, with the exact text.
            Text {
                objectName: "runContractsTitle"
                Layout.fillWidth: true
                visible: panel.contractCount > 0
                text: qsTr("Interface contracts")
                color: panel.colors.textPrimary
                font.pixelSize: panel.hostWindow.dp(13)
                font.weight: Font.DemiBold
            }

            Rectangle {
                objectName: "runContractsPanel"
                Layout.fillWidth: true
                visible: panel.contractCount > 0
                Layout.preferredHeight: runContractsColumn.implicitHeight + panel.hostWindow.dp(20)
                radius: panel.colors.cornerRadiusSmall
                color: panel.colors.surface
                border.width: 1
                border.color: panel.colors.border

                ColumnLayout {
                    id: runContractsColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: panel.hostWindow.dp(11)
                    anchors.rightMargin: panel.hostWindow.dp(11)
                    spacing: panel.hostWindow.dp(4)

                    Text {
                        objectName: "runContractsSummary"
                        Layout.fillWidth: true
                        text: qsTr("%1 frozen with this run · fingerprint %2")
                              .arg(panel.contractCount)
                              .arg(panel.runData ? String(panel.runData.interfaceContractsFingerprint || "").slice(0, 12) : "")
                        color: panel.colors.textPrimary
                        font.pixelSize: panel.hostWindow.dp(9.5)
                        wrapMode: Text.WordWrap
                    }

                    Repeater {
                        objectName: "runContractsRepeater"
                        model: panel.runData ? panel.runData.interfaceContracts : []

                        delegate: ColumnLayout {
                            id: contractItem
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: panel.hostWindow.dp(3)

                            Text {
                                objectName: "runContractLine"
                                Layout.fillWidth: true
                                text: qsTr("%1 · %2 · %3 · applies to %4")
                                      .arg(contractItem.modelData.id)
                                      .arg(contractItem.modelData.kind === "constraint"
                                           ? qsTr("constraint") : qsTr("interface contract"))
                                      .arg(contractItem.modelData.title)
                                      .arg(contractItem.modelData.appliesToPaths.length > 0
                                           ? contractItem.modelData.appliesToPaths.join(", ")
                                           : qsTr("every worker"))
                                color: panel.colors.textSecondary
                                font.pixelSize: panel.hostWindow.dp(8.5)
                                wrapMode: Text.WordWrap
                            }

                            // The exact text the Backend hands to the workers, byte for byte (the Backend only
                            // normalizes line endings). Read-only but selectable, so it can be compared with a
                            // task's output or copied.
                            TextEdit {
                                objectName: "runContractText"
                                Layout.fillWidth: true
                                Layout.leftMargin: panel.hostWindow.dp(8)
                                readOnly: true
                                selectByMouse: true
                                text: String(contractItem.modelData.text || "")
                                color: panel.colors.textPrimary
                                selectionColor: panel.colors.accent
                                selectedTextColor: panel.colors.textOnAccent
                                font.family: "monospace"
                                font.pixelSize: panel.hostWindow.dp(8.5)
                                wrapMode: TextEdit.Wrap
                            }
                        }
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("Run context")
                color: panel.colors.textPrimary
                font.pixelSize: panel.hostWindow.dp(13)
                font.weight: Font.DemiBold
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: runContextColumn.implicitHeight + panel.hostWindow.dp(20)
                radius: panel.colors.cornerRadiusSmall
                color: panel.colors.surface
                border.width: 1
                border.color: panel.colors.border

                ColumnLayout {
                    id: runContextColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: panel.hostWindow.dp(11)
                    anchors.rightMargin: panel.hostWindow.dp(11)
                    spacing: panel.hostWindow.dp(7)

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Objective")
                        color: panel.colors.textMuted
                        font.pixelSize: panel.hostWindow.dp(8.5)
                    }
                    Text {
                        id: objectiveText
                        Layout.fillWidth: true
                        text: panel.runData && panel.runData.task.length > 0
                              ? panel.runData.task : qsTr("No objective was recorded")
                        color: panel.colors.textPrimary
                        font.pixelSize: panel.hostWindow.dp(10)
                        wrapMode: Text.WordWrap
                        maximumLineCount: panel.contextExpanded ? 100000 : 4
                        elide: panel.contextExpanded ? Text.ElideNone : Text.ElideRight
                    }
                    Text {
                        id: projectContextText
                        Layout.fillWidth: true
                        visible: panel.runData && panel.runData.projectContext.length > 0
                        text: qsTr("Project context: %1").arg(panel.runData ? panel.runData.projectContext : "")
                        color: panel.colors.textSecondary
                        font.pixelSize: panel.hostWindow.dp(9)
                        wrapMode: Text.WordWrap
                        maximumLineCount: panel.contextExpanded ? 100000 : 3
                        elide: panel.contextExpanded ? Text.ElideNone : Text.ElideRight
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: panel.contextExpanded
                                 || objectiveText.truncated || projectContextText.truncated
                        spacing: panel.hostWindow.dp(10)

                        Text {
                            text: panel.contextExpanded ? qsTr("Show less ▲") : qsTr("Show more ▼")
                            color: contextToggleArea.containsMouse
                                   ? panel.colors.textPrimary : panel.colors.accent
                            font.pixelSize: panel.hostWindow.dp(9)

                            MouseArea {
                                id: contextToggleArea
                                anchors.fill: parent
                                anchors.margins: -panel.hostWindow.dp(4)
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: panel.contextExpanded = !panel.contextExpanded
                            }
                        }

                        Item { Layout.fillWidth: true }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: panel.runData && panel.runData.projectRoot.length > 0
                        text: qsTr("Workspace: %1").arg(panel.runData ? panel.runData.projectRoot : "")
                        color: panel.colors.textMuted
                        font.pixelSize: panel.hostWindow.dp(8.5)
                        elide: Text.ElideMiddle
                    }
                    Text {
                        Layout.fillWidth: true
                        text: panel.runData
                              ? qsTr("Workflow: %1 · max parallel agents: %2")
                                .arg(panel.runData.workflowId)
                                .arg(panel.runData.maxParallelism)
                              : ""
                        color: panel.colors.textMuted
                        font.pixelSize: panel.hostWindow.dp(8.5)
                    }
                }
            }

            // AG-17.7C-3: the stored audit of a conversion - only for a Run that was created by
            // one (an ordinary Run shows nothing); read once per selected Run.
            LegacyConversionRecordPanel {
                hostWindow: panel.hostWindow
                colors: panel.colors
                runsController: panel.runsController
                runId: panel.runData ? panel.runData.runId : ""
            }

            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Agent roster")
                    color: panel.colors.textPrimary
                    font.pixelSize: panel.hostWindow.dp(13)
                    font.weight: Font.DemiBold
                }
                Text {
                    text: panel.runData ? qsTr("%1 agents").arg(panel.runData.roster.length) : ""
                    color: panel.colors.textMuted
                    font.pixelSize: panel.hostWindow.dp(9)
                }
            }

            Text {
                Layout.fillWidth: true
                visible: panel.runData && panel.runData.roster.length === 0
                text: qsTr("This run uses the local/mock workflow and has no provider roster.")
                color: panel.colors.textMuted
                font.pixelSize: panel.hostWindow.dp(9)
                wrapMode: Text.WordWrap
            }

            Repeater {
                model: panel.runData ? panel.runData.roster : []
                delegate: Rectangle {
                    id: rosterItem
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.preferredHeight: rosterColumn.implicitHeight + panel.hostWindow.dp(16)
                    radius: panel.colors.cornerRadiusSmall
                    color: panel.colors.surface
                    border.width: 1
                    border.color: modelData.leadArchitect ? panel.colors.accent : panel.colors.border

                    ColumnLayout {
                        id: rosterColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: panel.hostWindow.dp(10)
                        anchors.rightMargin: panel.hostWindow.dp(10)
                        spacing: panel.hostWindow.dp(3)
                        Text {
                            Layout.fillWidth: true
                            text: (rosterItem.modelData.agentName.length > 0
                                   ? rosterItem.modelData.agentName : rosterItem.modelData.stageType)
                                  + (rosterItem.modelData.leadArchitect ? qsTr(" · Lead architect") : "")
                            color: panel.colors.textPrimary
                            font.pixelSize: panel.hostWindow.dp(10)
                            font.weight: Font.Medium
                            elide: Text.ElideRight
                        }
                        Text {
                            Layout.fillWidth: true
                            text: rosterItem.modelData.provider + " · " + rosterItem.modelData.model
                                  + (rosterItem.modelData.role.length > 0 ? " · " + rosterItem.modelData.role : "")
                            color: panel.colors.textSecondary
                            font.pixelSize: panel.hostWindow.dp(9)
                            elide: Text.ElideRight
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: rosterItem.modelData.instructions.length > 0
                            text: rosterItem.modelData.instructions
                            color: panel.colors.textMuted
                            font.pixelSize: panel.hostWindow.dp(8.5)
                            wrapMode: Text.WordWrap
                            maximumLineCount: 3
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: errorColumn.implicitHeight + panel.hostWindow.dp(20)
                visible: panel.runData && panel.runData.errorText.length > 0
                radius: panel.colors.cornerRadiusSmall
                color: panel.colors.errorSoft
                border.width: 1
                border.color: panel.colors.error
                ColumnLayout {
                    id: errorColumn
                    anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: panel.hostWindow.dp(10); anchors.rightMargin: panel.hostWindow.dp(10)
                    Text { Layout.fillWidth: true; text: qsTr("Run error"); color: panel.colors.error; font.pixelSize: panel.hostWindow.dp(10); font.weight: Font.DemiBold }
                    Text { Layout.fillWidth: true; text: panel.runData ? panel.runData.errorText : ""; color: panel.colors.textSecondary; font.pixelSize: panel.hostWindow.dp(9); wrapMode: Text.WordWrap }
                }
            }

            Rectangle {
                objectName: "plannerOutcomeCard"
                Layout.fillWidth: true
                Layout.preferredHeight: plannerOutcomeColumn.implicitHeight
                                        + panel.hostWindow.dp(20)
                visible: panel.plannerReportArtifact !== null
                radius: panel.colors.cornerRadiusSmall
                color: panel.colors.surface
                border.width: 1
                border.color: panel.plannerReportArtifact
                              && panel.plannerReportArtifact.name
                                 === "manifest-planning-no-changes.md"
                              ? panel.colors.success : panel.colors.warning

                ColumnLayout {
                    id: plannerOutcomeColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: panel.hostWindow.dp(10)
                    anchors.rightMargin: panel.hostWindow.dp(10)
                    spacing: panel.hostWindow.dp(6)

                    Text {
                        Layout.fillWidth: true
                        text: panel.plannerReportArtifact
                              && panel.plannerReportArtifact.name
                                 === "manifest-planning-no-changes.md"
                              ? qsTr("Planner: current task already satisfied")
                              : qsTr("Planner diagnostics")
                        color: panel.colors.textPrimary
                        font.pixelSize: panel.hostWindow.dp(10)
                        font.weight: Font.DemiBold
                    }
                    Text {
                        Layout.fillWidth: true
                        text: panel.plannerReportArtifact
                              && panel.plannerReportArtifact.name
                                 === "manifest-planning-no-changes.md"
                              ? qsTr("Execution was intentionally skipped. Review the evidence tied to inspected paths and any recommended next step; no next stage was launched automatically.")
                              : qsTr("Review the exact primary/final rejection codes and safe Backend validation findings.")
                        color: panel.colors.textSecondary
                        font.pixelSize: panel.hostWindow.dp(9)
                        wrapMode: Text.WordWrap
                    }
                    RunActionButton {
                        objectName: "viewPlannerReportButton"
                        hostWindow: panel.hostWindow
                        colors: panel.colors
                        text: qsTr("View planner report")
                        iconText: "↗"
                        primary: true
                        onClicked: panel.artifactRequested(
                                       panel.runData.runId,
                                       panel.plannerReportArtifact.artifactId)
                    }
                }
            }

            Rectangle {
                objectName: "manifestVerificationCard"
                Layout.fillWidth: true
                Layout.preferredHeight: verificationColumn.implicitHeight
                                        + panel.hostWindow.dp(20)
                visible: panel.runData !== null
                         && panel.runData.workflowId === "execution-manifest-v1"
                radius: panel.colors.cornerRadiusSmall
                color: panel.colors.surface
                border.width: 1
                // The card is intentionally never green from artifact
                // presence alone. Exact statuses live in the integrity-
                // checked record and unknown/not-run must stay neutral.
                border.color: panel.colors.information

                ColumnLayout {
                    id: verificationColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: panel.hostWindow.dp(10)
                    anchors.rightMargin: panel.hostWindow.dp(10)
                    spacing: panel.hostWindow.dp(6)

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Manifest verification")
                        color: panel.colors.textPrimary
                        font.pixelSize: panel.hostWindow.dp(10)
                        font.weight: Font.DemiBold
                    }
                    Text {
                        objectName: "manifestVerificationSummary"
                        Layout.fillWidth: true
                        text: panel.manifestVerificationArtifact !== null
                              ? qsTr("Recorded separately: proposal safety/schema validation, combined build/lint/test verification, and acceptance-criteria verification. Open the record for exact statuses and evidence.")
                              : qsTr("Verification not available or not run. This state is not treated as passed; older Runs are not assigned a result retroactively.")
                        color: panel.colors.textSecondary
                        font.pixelSize: panel.hostWindow.dp(9)
                        wrapMode: Text.WordWrap
                    }
                    RunActionButton {
                        objectName: "viewManifestVerificationButton"
                        visible: panel.manifestVerificationArtifact !== null
                        hostWindow: panel.hostWindow
                        colors: panel.colors
                        text: qsTr("View verification record")
                        iconText: "↗"
                        primary: true
                        onClicked: panel.artifactRequested(
                                       panel.runData.runId,
                                       panel.manifestVerificationArtifact.artifactId)
                    }
                }
            }

            Rectangle {
                objectName: "workspaceProvenanceCard"
                Layout.fillWidth: true
                Layout.preferredHeight: workspaceProvenanceColumn.implicitHeight
                                        + panel.hostWindow.dp(20)
                visible: panel.runData !== null
                         && panel.runData.workflowId === "execution-manifest-v1"
                radius: panel.colors.cornerRadiusSmall
                color: panel.colors.surface
                border.width: 1
                border.color: panel.colors.information

                ColumnLayout {
                    id: workspaceProvenanceColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: panel.hostWindow.dp(10)
                    anchors.rightMargin: panel.hostWindow.dp(10)
                    spacing: panel.hostWindow.dp(6)

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Workspace provenance")
                        color: panel.colors.textPrimary
                        font.pixelSize: panel.hostWindow.dp(10)
                        font.weight: Font.DemiBold
                    }
                    Text {
                        objectName: "workspaceProvenanceSummary"
                        Layout.fillWidth: true
                        text: panel.workspaceBaselineArtifact === null
                              ? qsTr("Workspace baseline is not available for this Run. No provenance result is inferred.")
                              : (panel.workspaceProvenanceArtifact === null
                                 ? qsTr("The pre-provider baseline is recorded. Post-run comparison is not available or not run.")
                                 : qsTr("The pre-provider baseline and latest post-run or post-apply comparison are recorded. Open them for the exact fingerprint, exclusions, proposal set, and manual/external drift classification."))
                        color: panel.colors.textSecondary
                        font.pixelSize: panel.hostWindow.dp(9)
                        wrapMode: Text.WordWrap
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: panel.hostWindow.dp(6)
                        RunActionButton {
                            objectName: "viewWorkspaceBaselineButton"
                            visible: panel.workspaceBaselineArtifact !== null
                            hostWindow: panel.hostWindow
                            colors: panel.colors
                            text: qsTr("View baseline")
                            iconText: "↗"
                            onClicked: panel.artifactRequested(
                                           panel.runData.runId,
                                           panel.workspaceBaselineArtifact.artifactId)
                        }
                        RunActionButton {
                            objectName: "viewWorkspaceProvenanceButton"
                            visible: panel.workspaceProvenanceArtifact !== null
                            hostWindow: panel.hostWindow
                            colors: panel.colors
                            text: qsTr("View provenance")
                            iconText: "↗"
                            primary: true
                            onClicked: panel.artifactRequested(
                                           panel.runData.runId,
                                           panel.workspaceProvenanceArtifact.artifactId)
                        }
                        Item { Layout.fillWidth: true }
                    }
                }
            }

            Text { Layout.fillWidth: true; text: qsTr("Stages"); color: panel.colors.textPrimary; font.pixelSize: panel.hostWindow.dp(13); font.weight: Font.DemiBold }
            RunStageTimeline { Layout.fillWidth: true; hostWindow: panel.hostWindow; colors: panel.colors; stagesModel: panel.runData ? panel.runData.stages : [] }

            // Durable warnings of this Run: something the Run was meant to leave behind (a diagnostic artifact) could not
            // be stored, although the Run itself went on. Read from the database, so it is still here after a restart.
            ColumnLayout {
                objectName: "runWarningsSection"
                Layout.fillWidth: true
                visible: panel.runWarnings.length > 0
                spacing: panel.hostWindow.dp(4)
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Warnings (%1)").arg(panel.runWarnings.length)
                    color: panel.colors.textPrimary
                    font.pixelSize: panel.hostWindow.dp(13)
                    font.weight: Font.DemiBold
                }
                Repeater {
                    model: panel.runWarnings
                    delegate: Text {
                        id: warningItem
                        required property var modelData
                        objectName: "runWarningRow"
                        Layout.fillWidth: true
                        text: "⚠ " + warningItem.modelData.message
                        color: panel.colors.textSecondary
                        font.pixelSize: panel.hostWindow.dp(9)
                        wrapMode: Text.WordWrap
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(8)
                Text { Layout.fillWidth: true; text: qsTr("Artifacts"); color: panel.colors.textPrimary; font.pixelSize: panel.hostWindow.dp(13); font.weight: Font.DemiBold }
                Text { text: panel.runData ? qsTr("%1 files").arg(panel.runData.artifacts.length) : ""; color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(9) }
                RunActionButton {
                    visible: panel.contextSnapshotArtifact !== null
                    hostWindow: panel.hostWindow; colors: panel.colors
                    text: qsTr("View context")
                    iconText: "🔎"
                    primary: true
                    onClicked: panel.viewContextRequested(
                                   panel.runData.runId,
                                   panel.contextSnapshotArtifact.artifactId)
                }
                RunActionButton {
                    visible: panel.runData !== null && panel.runData.artifacts.length > 0
                    hostWindow: panel.hostWindow; colors: panel.colors
                    text: qsTr("Open folder")
                    iconText: "📂"
                    onClicked: panel.artifactsFolderRequested(
                                   panel.runData.runId,
                                   panel.runData.artifacts[0].artifactId)
                }
            }
            Text {
                Layout.fillWidth: true
                visible: panel.runData && panel.runData.artifacts.length === 0
                text: qsTr("No review artifacts were produced for this run.")
                color: panel.colors.textMuted
                font.pixelSize: panel.hostWindow.dp(9)
                wrapMode: Text.WordWrap
            }
            Repeater {
                model: panel.runData ? panel.runData.artifacts : []
                delegate: RunActionButton {
                    id: artifactItem
                    required property var modelData
                    Layout.fillWidth: true
                    hostWindow: panel.hostWindow; colors: panel.colors
                    text: modelData.name + " · " + modelData.type + " · " + modelData.size
                    iconText: modelData.type === "context_snapshot" ? "🔎" : "↗"
                    onClicked: modelData.type === "context_snapshot"
                               ? panel.viewContextRequested(panel.runData.runId, modelData.artifactId)
                               : panel.artifactRequested(panel.runData.runId, modelData.artifactId)
                }
            }

            Text { Layout.fillWidth: true; text: qsTr("Recent log"); color: panel.colors.textPrimary; font.pixelSize: panel.hostWindow.dp(13); font.weight: Font.DemiBold }
            Repeater {
                model: panel.runData ? panel.runData.logs : []
                delegate: RowLayout {
                    id: logItem
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: panel.hostWindow.dp(7)
                    Text { text: "•"; color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(10) }
                    Text { Layout.fillWidth: true; text: logItem.modelData; color: panel.colors.textSecondary; font.pixelSize: panel.hostWindow.dp(9); elide: Text.ElideRight }
                }
            }

            AgentSessionPanel {
                Layout.fillWidth: true
                visible: panel.agentSessionController !== null
                hostWindow: panel.hostWindow
                colors: panel.colors
                controller: panel.agentSessionController
                runsController: panel.runsController
                runId: panel.runData ? panel.runData.runId : ""
                runActive: panel.runData
                           && (panel.runData.status === "queued" || panel.runData.status === "running")
            }

        }
    }

    Rectangle {
        id: jumpToBottomButton
        objectName: "runDetailsJumpToBottom"
        visible: panel.showJumpToBottom
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: panel.hostWindow.dp(24)
        anchors.bottomMargin: panel.hostWindow.dp(24)
        width: panel.hostWindow.dp(36)
        height: width
        radius: width / 2
        z: 4
        color: jumpToBottomMouse.containsMouse
               ? panel.colors.buttonHover : panel.colors.surfaceRaised
        border.width: 1
        border.color: panel.colors.accent

        Text {
            anchors.centerIn: parent
            text: "↓"
            color: panel.colors.accent
            font.pixelSize: panel.hostWindow.dp(18)
            font.weight: Font.DemiBold
        }

        MouseArea {
            id: jumpToBottomMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: detailFlickable.jumpToBottom()
        }
    }
}
