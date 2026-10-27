import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import "Runs_Objects"

Item {
    id: page

    required property var hostWindow
    required property var colors
    // Qt Adapter composition root supplied by PageHost.
    required property var backend

    signal projectSelectionRequested(string projectId)
    signal runSelectionRequested(string runId)
    signal runStartRequested(string projectId)
    signal runCancelRequested(string runId)
    signal runRetryRequested(string runId)
    signal artifactOpenRequested(string runId, string artifactId)
    signal comparisonRequested(string runId)
    signal refreshRequested()

    // Selection is owned by RunController (backend.runs.selectedRunId) so it
    // always resolves by stable run ID, never by list position.
    readonly property string selectedRunId: page.backend.runs.selectedRunId
    // The selected workspace project is persisted by Backend and restored on
    // restart. Runs uses it as its initial filter and New Run default.
    readonly property string activeProjectId: page.backend.workspace.activeProjectId
    property string projectFilter: "all"
    // Ticked once a second while any run is active - see the Timer below.
    // Only RunDetailsPanel's liveDuration() reads this, so a tick no longer
    // has to rebuild runsSnapshot (every run's roster/provider/model names/
    // stages/artifacts/logs) just to keep one elapsed-time label current.
    property real nowMs: Date.now()
    property string statusFilter: "all"
    property string manualResolutionFilter: "all"
    property string searchText: ""
    property string notificationText: ""

    // Required properties are assigned as part of component construction. Qt
    // 6.11 AOT may evaluate dependent bindings one step earlier, so keep the
    // initial value safe until PageHost supplies hostWindow.
    readonly property real logicalWidth: width / Math.max(hostWindow ? hostWindow.scaleFactor : 1.0, 0.01)

    // The dedicated non-visual component owns the adapter-model projections.
    // Keep the original empty-list defaults while the child is being created.
    property var projectsSnapshot: snapshots ? (snapshots.projectsSnapshot || []) : []
    property var runsSnapshot: snapshots ? (snapshots.runsSnapshot || []) : []
    property var stagesSnapshot: snapshots ? (snapshots.stagesSnapshot || []) : []
    property var artifactsSnapshot: snapshots ? (snapshots.artifactsSnapshot || []) : []
    property var warningsSnapshot: snapshots ? (snapshots.warningsSnapshot || []) : []
    readonly property var agentChoicesSnapshot: snapshots ? (snapshots.agentChoicesSnapshot || []) : []
    property var tasksSnapshot: snapshots ? (snapshots.tasksSnapshot || []) : []
    property var draftsSnapshot: snapshots ? (snapshots.draftsSnapshot || []) : []

    RunModelSnapshots {
        id: snapshots
        backend: page.backend
        onSelectionReconciliationRequested: Qt.callLater(page.ensureSelectionMatchesProjectFilter)
    }

    readonly property var projectTasks: tasksSnapshot.filter(function(task) {
        return page.projectFilter !== "all"
                && task.projectId === page.projectFilter
                && task.status !== "done"
                && task.status !== "cancelled"
    })

    readonly property var projectOptions: {
        const options = [{ "label": qsTr("All projects"), "value": "all" }]
        for (let i = 0; i < projectsSnapshot.length; ++i) {
            const project = projectsSnapshot[i]
            let label = project.name
            if (project.projectId === page.activeProjectId) {
                label += " — " + qsTr("current")
            } else if (project.status === "archived") {
                label += " — " + qsTr("archived")
            } else if (project.status === "unavailable") {
                label += " — " + qsTr("unavailable")
            }
            options.push({ "label": label, "value": project.projectId })
        }
        return options
    }
    // RunListPanel's history header names whichever scope filteredRuns is
    // actually restricted to, instead of a generic "Run history" label.
    readonly property string projectFilterLabel: {
        for (let i = 0; i < projectOptions.length; ++i) {
            if (projectOptions[i].value === page.projectFilter)
                return projectOptions[i].label
        }
        return qsTr("All projects")
    }
    readonly property var statusOptions: [
        { "label": qsTr("All statuses"), "value": "all" },
        { "label": qsTr("Prepared"), "value": "prepared" },
        { "label": qsTr("Queued"), "value": "queued" },
        { "label": qsTr("Running"), "value": "running" },
        { "label": qsTr("Cancelling"), "value": "cancelling" },
        { "label": qsTr("Completed"), "value": "completed" },
        { "label": qsTr("Failed"), "value": "failed" },
        { "label": qsTr("Cancelled"), "value": "cancelled" },
        { "label": qsTr("Interrupted"), "value": "interrupted" }
    ]
    readonly property var manualResolutionOptions: [
        { "label": qsTr("All resolutions"), "value": "all" },
        { "label": qsTr("Unresolved"), "value": "unresolved" },
        { "label": qsTr("Verified · awaiting Apply"), "value": "verified_awaiting_apply" },
        { "label": qsTr("Partially applied"), "value": "partially_applied" },
        { "label": qsTr("Resolved manually · Applied"), "value": "resolved_manually_applied" }
    ]

    readonly property var filteredRuns: runsSnapshot.filter(function (run) {
        const query = page.searchText.trim().toLowerCase()
        const matchesSearch = query.length === 0
                              || run.title.toLowerCase().indexOf(query) >= 0
                              || run.runId.toLowerCase().indexOf(query) >= 0
        return matchesSearch
                && (page.projectFilter === "all" || run.projectId === page.projectFilter)
                && (page.statusFilter === "all" || run.status === page.statusFilter)
                && (page.manualResolutionFilter === "all"
                    || run.manualHandoffResolution === page.manualResolutionFilter)
    })

    // An empty key is the page's explicit calendar mode. Selecting a key
    // replaces the full-size calendar with the existing list/detail panes.
    property string selectedHistoryDateKey: ""

    function runReferenceDate(run) {
        return run.finishedAt || run.startedAt || run.createdAt || null
    }

    function historyDateKey(value) {
        return value ? Qt.formatDate(value, "yyyy-MM-dd") : ""
    }

    // Calendar cells and RunListPanel consume the same already-filtered run
    // projection, so project/status/search filters cannot drift between views.
    readonly property var historyByDay: {
        const map = {}
        for (let i = 0; i < page.filteredRuns.length; ++i) {
            const run = page.filteredRuns[i]
            if (run.status === "prepared" || run.status === "queued" || run.status === "running"
                    || run.status === "cancelling")
                continue
            const key = page.historyDateKey(page.runReferenceDate(run))
            if (key.length === 0)
                continue
            if (!map[key])
                map[key] = []
            map[key].push(run)
        }
        for (const key in map) {
            map[key].sort(function (a, b) {
                const da = page.runReferenceDate(a)
                const db = page.runReferenceDate(b)
                return (db ? db.getTime() : 0) - (da ? da.getTime() : 0)
            })
        }
        return map
    }

    function showHistoryDate(key) {
        page.selectedHistoryDateKey = key
        const dayRuns = page.historyByDay[key] || []
        // Details must never keep showing a run from a different calendar day.
        page.selectRun(dayRuns.length > 0 ? dayRuns[0].runId : "")
    }

    function showHistoryCalendar() {
        page.selectedHistoryDateKey = ""
    }

    // Every fresh Run - freshly started, started with a custom roster, or
    // retried - is created "now", so it always belongs on today's calendar
    // cell. Unlike showHistoryDate(), this does not derive the selection from
    // historyByDay: a just-created Run is still queued/running and therefore
    // deliberately excluded from that history bucket (see historyByDay above),
    // so looking it up there would immediately clear the selection instead of
    // showing the run the user just created. Jump out of calendar mode and
    // select the run directly so it is visible under RunListPanel's "Active
    // runs" section without the user having to find today's cell by hand.
    function focusRunToday(runId) {
        page.selectedHistoryDateKey = page.historyDateKey(new Date())
        page.selectRun(runId)
    }

    readonly property var selectedRun: {
        for (let i = 0; i < runsSnapshot.length; ++i) {
            if (runsSnapshot[i].runId === selectedRunId)
                return runsSnapshot[i]
        }
        return null
    }

    readonly property int runningCount: runsSnapshot.filter(function (r) { return r.status === "running" }).length
    readonly property int queuedCount: runsSnapshot.filter(function (r) { return r.status === "queued" }).length
    readonly property int completedCount: runsSnapshot.filter(function (r) { return r.status === "completed" }).length
    readonly property int failedCount: runsSnapshot.filter(function (r) { return r.status === "failed" }).length
    readonly property int cancelledCount: runsSnapshot.filter(function (r) {
        return r.status === "cancelled" || r.status === "interrupted"
    }).length
    readonly property int terminalCount: completedCount + failedCount + cancelledCount
    readonly property int successRate: terminalCount > 0 ? Math.round(completedCount / terminalCount * 100) : 0
    readonly property real totalTokens: runsSnapshot.reduce(function (sum, r) { return sum + r.inputTokens + r.outputTokens }, 0)

    // Deliberately derived from the full unfiltered runsSnapshot (not
    // filteredRuns) and independent of calendar/list view mode, so the page
    // title always shows what is genuinely running right now regardless of
    // the current project/status/search filters or whether the calendar is
    // showing. Runs is calendar-first by default, so without this the only
    // "active runs" indication (RunListPanel's own section) is invisible
    // until a specific date is selected.
    readonly property var activeRunsNow: runsSnapshot.filter(function (r) {
        return r.status === "queued" || r.status === "running" || r.status === "cancelling"
    })

    function statusLabel(status) {
        return snapshots.statusLabel(status)
    }

    function formatTokens(count) {
        return snapshots.formatTokens(count)
    }

    function deleteDraft(draftId) {
        if (page.backend.runDrafts.removeDraft(draftId)) {
            page.notify(qsTr("Draft deleted"))
        } else {
            page.notify(page.backend.runDrafts.lastError)
        }
    }

    function openDrafts() {
        if (page.draftsSnapshot.length === 0) {
            page.notify(qsTr("No drafts saved yet"))
            return
        }
        draftsDialogLoader.active = true
    }

    function ensureSelectionMatchesProjectFilter() {
        // A selected calendar day is authoritative over the general
        // "newest run for this project" reconciliation below: never let a
        // snapshot rebuild re-select a run from a different day than the
        // one currently showing.
        if (page.selectedHistoryDateKey.length > 0) {
            let selectedIsKnown = page.selectedRunId.length === 0
            let selectedIsActive = false
            for (let runIndex = 0; runIndex < page.runsSnapshot.length; ++runIndex) {
                const selectedCandidate = page.runsSnapshot[runIndex]
                if (selectedCandidate.runId !== page.selectedRunId)
                    continue
                selectedIsKnown = true
                selectedIsActive = selectedCandidate.status === "prepared"
                        || selectedCandidate.status === "queued"
                        || selectedCandidate.status === "running"
                        || selectedCandidate.status === "cancelling"
                break
            }

            // Backend selection can point at a freshly enqueued Run before
            // the immutable list projection contains it. Once present, active
            // Runs remain visible above the selected day's terminal history,
            // so the calendar must not replace their selection either.
            if (!selectedIsKnown || selectedIsActive)
                return

            const dayRuns = page.historyByDay[page.selectedHistoryDateKey] || []
            const stillInDay = dayRuns.some(function(r) { return r.runId === page.selectedRunId })
            if (!stillInDay) {
                page.backend.runs.selectRun(dayRuns.length > 0 ? dayRuns[0].runId : "")
            }
            return
        }

        if (page.projectFilter === "all")
            return

        let selectedWasPresent = page.selectedRunId.length === 0
        for (let i = 0; i < page.runsSnapshot.length; ++i) {
            const run = page.runsSnapshot[i]
            if (run.runId === page.selectedRunId) {
                selectedWasPresent = true
                if (run.projectId === page.projectFilter)
                    return
            }
        }

        // The controller owns selection and may already point at a newly
        // created run while the preceding immutable list snapshot is still
        // visible. Absence is therefore not proof that the ID is invalid.
        if (!selectedWasPresent)
            return

        // Backend lists oldest first; pick the newest run for the active
        // project, or clear details when that project has no runs yet.
        for (let i = page.runsSnapshot.length - 1; i >= 0; --i) {
            if (page.runsSnapshot[i].projectId === page.projectFilter) {
                page.backend.runs.selectRun(page.runsSnapshot[i].runId)
                return
            }
        }
        page.backend.runs.selectRun("")
    }

    function applyProjectFilter(projectId) {
        const nextFilter = projectId.length > 0 ? projectId : "all"
        if (page.projectFilter !== nextFilter && nextFilter !== "all") {
            // An explicit project switch is the one place where an existing
            // selection may be invalidated intentionally. Clearing it here
            // lets the next completed projection choose that project's newest
            // run without confusing it with an async list-update race.
            for (let i = 0; i < page.runsSnapshot.length; ++i) {
                const run = page.runsSnapshot[i]
                if (run.runId === page.selectedRunId
                        && run.projectId !== nextFilter) {
                    page.backend.runs.selectRun("")
                    break
                }
            }
        }
        page.projectFilter = nextFilter
        page.ensureSelectionMatchesProjectFilter()
    }

    Component.onCompleted: {
        page.applyProjectFilter(page.activeProjectId)
        // Compile/instantiate the large dialog after the page's first frame.
        // Opening it later is then only a visibility change, never a burst of
        // QML object creation on the click that the user perceives as a freeze.
        Qt.callLater(function() { newRunDialogLoader.active = true })
    }

    // Provider calls may legitimately remain quiet for several minutes while
    // a reasoning model prepares its first byte. Keep the displayed elapsed
    // duration alive so the selected Run never looks frozen between backend
    // StageStarted and the next provider event. Only ticks page.nowMs (one
    // property write) rather than calling rebuildRunsSnapshot() - that used
    // to rescan every run's roster/provider/model names/stages/artifacts/
    // logs once a second for the whole duration of any active run, purely to
    // keep this one number current; RunDetailsPanel.liveDuration() is the
    // only thing that actually needs a per-second tick.
    Timer {
        interval: 1000
        repeat: true
        running: page.runningCount > 0 || page.queuedCount > 0
        onTriggered: page.nowMs = Date.now()
    }

    Connections {
        target: page.backend.workspace
        function onActiveProjectIdChanged() {
            page.applyProjectFilter(page.activeProjectId)
        }
    }

    function selectRun(runId) {
        page.backend.runs.selectRun(runId)
        runSelectionRequested(runId)
    }

    function startRun(projectId, title, task, executionProfileId, projectTaskId,
                      workspacePath, sourceRunId, allowEvidenceBackedNoChanges,
                      automaticVerificationRepair) {
        // Backend atomically links a genuinely new Run to projectTaskId in
        // the same aggregate insert as the Run itself (see
        // RunManager::enqueueInternal); a Retry/Edit & rerun instead
        // atomically copies its source Run's existing task links, so the
        // dialog's current task is never sent for that path.
        const isNewRun = !sourceRunId || sourceRunId.length === 0
        const runId = page.backend.runs.startRun(
            projectId, title, task, executionProfileId, workspacePath || "",
            sourceRunId || "", isNewRun ? (projectTaskId || "") : "",
            Boolean(allowEvidenceBackedNoChanges),
            automaticVerificationRepair === undefined
                ? true : Boolean(automaticVerificationRepair))
        if (runId.length > 0) {
            if (newRunDialogLoader.item)
                newRunDialogLoader.item.close()
            page.focusRunToday(runId)
            // The synchronous command only guarantees durable enqueue. The
            // worker publishes Running/terminal states asynchronously.
            page.notify(qsTr("Run queued"))
        } else {
            if (newRunDialogLoader.item)
                newRunDialogLoader.item.errorText = page.backend.runs.lastError
        }
    }

    function startRunWithRoster(projectId, title, task, roster, maxParallelism,
                                projectTaskId, workspacePath, sourceRunId,
                                allowEvidenceBackedNoChanges,
                                automaticVerificationRepair, workflowId, providerBudget,
                                interfaceContracts) {
        const isNewRun = !sourceRunId || sourceRunId.length === 0
        const runId = page.backend.runs.startRunWithRoster(
                    projectId, title, task, roster, maxParallelism,
                    workspacePath || "", sourceRunId || "",
                    isNewRun ? (projectTaskId || "") : "",
                    Boolean(allowEvidenceBackedNoChanges),
                    automaticVerificationRepair === undefined
                        ? true : Boolean(automaticVerificationRepair),
                    workflowId || "", providerBudget || ({}), interfaceContracts || [])
        if (runId.length === 0) {
            if (newRunDialogLoader.item)
                newRunDialogLoader.item.errorText = page.backend.runs.lastError
            return
        }
        if (newRunDialogLoader.item)
            newRunDialogLoader.item.close()
        page.focusRunToday(runId)
        page.notify(qsTr("Run queued"))
    }

    function openNewRunDialog(projectTaskId) {
        newRunDialogLoader.active = true
        Qt.callLater(function () {
            if (newRunDialogLoader.item) {
                newRunDialogLoader.item.preferredTaskId = projectTaskId || ""
                newRunDialogLoader.item.open()
            }
        })
    }

    // AG-17.7C-2: the conversion mode of Edit & rerun for a legacy roster Run. `conversion` is the
    // Backend's own preview (RunController.rerunDraft().conversion or legacyConversionPreview());
    // the page only hands it to the dialog and relays the user's one explicit confirmation.
    function openConversionDialog(conversion) {
        legacyConversionDialogLoader.active = true
        Qt.callLater(function () {
            if (legacyConversionDialogLoader.item) {
                legacyConversionDialogLoader.item.preview = conversion
                legacyConversionDialogLoader.item.open()
            }
        })
    }

    function convertLegacyRun(sourceRunId, title, task, workspacePath, maxParallelism,
                              allowEvidenceBackedNoChanges, automaticVerificationRepair,
                              expectedConfirmationFingerprint) {
        // The fingerprint is opaque: it goes back to the Backend exactly as the preview showed it.
        // There is no roster, workflow, parent or "confirmed" flag in this call - the Backend
        // rebuilds all of them from the immutable source Run.
        const result = page.backend.runs.startConvertedRerun(
                    sourceRunId, title, task, workspacePath || "", maxParallelism,
                    Boolean(allowEvidenceBackedNoChanges),
                    Boolean(automaticVerificationRepair),
                    expectedConfirmationFingerprint)
        const dialog = legacyConversionDialogLoader.item
        if (result.accepted) {
            if (dialog)
                dialog.close()
            page.focusRunToday(result.runId)
            page.notify(qsTr("Converted Run queued"))
            return
        }
        if (!dialog)
            return
        dialog.errorText = result.errorMessage
        if (result.confirmationStale) {
            // What was shown is no longer what would be created: show the Backend's current
            // preview and let the user confirm THAT, keeping what they typed.
            const fresh = page.backend.runs.legacyConversionPreview(sourceRunId)
            if (fresh && fresh.sourceRunId)
                dialog.preview = fresh
        }
    }

    function openEditRunDialog(runId) {
        const source = page.backend.runs.rerunDraft(runId)
        if (!source || !source.sourceRunId) {
            page.notify(page.backend.runs.lastError)
            return
        }
        // A legacy lone workspace lead has NO legacy Edit & rerun variant (the legacy runner
        // cannot execute it): the Backend marks it `required` and only the conversion dialog opens.
        if (source.conversion && source.conversion.required) {
            page.openConversionDialog(source.conversion)
            return
        }
        newRunDialogLoader.active = true
        Qt.callLater(function () {
            if (newRunDialogLoader.item) {
                newRunDialogLoader.item.prefillData = source
                newRunDialogLoader.item.open()
            }
        })
    }

    function saveDraft(projectId, title, task, launchMode,
                       executionProfileId, roster, maxParallelism, workspacePath,
                       automaticVerificationRepair, workflowId, providerBudget,
                       interfaceContracts) {
        const draftId = page.backend.runDrafts.createDraft(
                    title, projectId, title, task, launchMode,
                    executionProfileId || "", roster, maxParallelism, workspacePath || "",
                    automaticVerificationRepair === undefined
                        ? true : Boolean(automaticVerificationRepair),
                    workflowId || "", providerBudget || ({}), interfaceContracts || [])
        if (draftId.length > 0) {
            page.notify(qsTr("Draft saved"))
        } else {
            if (newRunDialogLoader.item)
                newRunDialogLoader.item.errorText = page.backend.runDrafts.lastError
        }
    }

    // PC-3D: a saved draft is changed in place from New Run ("Update this draft"). The grouped drafts of a plan
    // keep their group, order and dependencies (the Backend refuses to change them); only the editable content moves,
    // and the "edited" marker follows from the Backend's own comparison.
    function updateDraft(draftId, projectId, title, task, launchMode,
                         executionProfileId, roster, maxParallelism, workspacePath,
                         automaticVerificationRepair, workflowId, providerBudget,
                         interfaceContracts) {
        const updated = page.backend.runDrafts.updateDraft(
                    draftId, title, projectId, title, task, launchMode,
                    executionProfileId || "", roster, maxParallelism, workspacePath || "",
                    automaticVerificationRepair === undefined
                        ? true : Boolean(automaticVerificationRepair),
                    workflowId || "", providerBudget || ({}), interfaceContracts || [])
        if (updated) {
            page.notify(qsTr("Draft updated"))
        } else if (newRunDialogLoader.item) {
            newRunDialogLoader.item.errorText = page.backend.runDrafts.lastError
        }
    }

    // Opens the New Run dialog prefilled from a saved draft. Unlike
    // openEditRunDialog(), the resulting dialog always creates a brand-new
    // Run through the normal runAccepted/rosterRunAccepted path (a draft has
    // no sourceRunId), and the draft itself is left untouched so it can be
    // reopened again later - reopening never deletes it.
    function openDraftInDialog(draftId) {
        const source = page.backend.runDrafts.openDraft(draftId)
        if (!source || !source.isDraft) {
            page.notify(page.backend.runDrafts.lastError)
            return
        }
        newRunDialogLoader.active = true
        Qt.callLater(function () {
            if (newRunDialogLoader.item) {
                newRunDialogLoader.item.prefillData = source
                newRunDialogLoader.item.open()
            }
        })
    }

    function viewArtifactContext(artifactId) {
        const text = page.backend.runs.readArtifactText(artifactId)
        if (text.length === 0 && page.backend.runs.lastError.length > 0) {
            page.notify(page.backend.runs.lastError)
            return
        }
        artifactContentDialogLoader.active = true
        Qt.callLater(function () {
            if (artifactContentDialogLoader.item) {
                artifactContentDialogLoader.item.contentText = text
                artifactContentDialogLoader.item.open()
            }
        })
    }

    function cancelRun(runId) {
        if (page.backend.runs.cancelRun(runId)) {
            page.runCancelRequested(runId)
            page.notify(qsTr("Cancellation requested"))
        } else {
            page.notify(page.backend.runs.lastError)
        }
    }

    function retryRun(runId) {
        const newRunId = page.backend.runs.retryRun(runId)
        if (newRunId.length > 0) {
            page.focusRunToday(newRunId)
            page.runRetryRequested(newRunId)
            page.notify(qsTr("Retry started"))
        } else {
            page.notify(page.backend.runs.lastError)
        }
    }

    function notify(message) {
        notificationText = message
        notificationTimer.restart()
    }

    Timer { id: notificationTimer; interval: 2800; onTriggered: page.notificationText = "" }

    Flickable {
        id: scrollView
        anchors.fill: parent
        contentWidth: width
        contentHeight: contentColumn.implicitHeight + page.colors.contentMargin * 2
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: RunScrollBar {
            id: runsPageScrollBar
            hostWindow: page.hostWindow
            colors: page.colors
        }

        ColumnLayout {
            id: contentColumn
            x: page.colors.contentMargin
            y: page.colors.contentMargin
            width: Math.max(0, scrollView.width - page.colors.contentMargin * 2
                            - runsPageScrollBar.width)
            spacing: page.hostWindow.dp(14)

            RowLayout {
                Layout.fillWidth: true
                spacing: page.hostWindow.dp(12)
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: page.hostWindow.dp(4)
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Runs")
                        color: page.colors.textPrimary
                        font.pixelSize: page.hostWindow.dp(28)
                        font.weight: Font.DemiBold
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Track workflow execution, stages, artifacts and failures.")
                        color: page.colors.textSecondary
                        font.pixelSize: page.hostWindow.dp(13)
                    }
                }

                // Runs defaults to calendar-first (see RunCalendarView below), which
                // hides RunListPanel's own "Active runs" section until a specific date
                // is selected. This pill is always visible regardless of calendar/list
                // mode or the project/status/search filters, so the user can tell what
                // is running right now without leaving whatever view they are on.
                Rectangle {
                    id: activeRunsPill
                    visible: page.activeRunsNow.length > 0
                    Layout.preferredHeight: page.hostWindow.dp(32)
                    Layout.preferredWidth: activeRunsRow.implicitWidth + page.hostWindow.dp(22)
                    radius: height / 2
                    color: page.colors.accentSoft
                    border.width: 1
                    border.color: page.colors.accent

                    RowLayout {
                        id: activeRunsRow
                        anchors.centerIn: parent
                        spacing: page.hostWindow.dp(7)

                        Rectangle {
                            id: activeRunsDot
                            Layout.preferredWidth: page.hostWindow.dp(8)
                            Layout.preferredHeight: page.hostWindow.dp(8)
                            radius: width / 2
                            color: page.colors.accent

                            SequentialAnimation on opacity {
                                loops: Animation.Infinite
                                running: activeRunsPill.visible
                                NumberAnimation { from: 1.0; to: 0.35; duration: 700 }
                                NumberAnimation { from: 0.35; to: 1.0; duration: 700 }
                            }
                        }

                        Text {
                            Layout.maximumWidth: page.hostWindow.dp(260)
                            text: page.activeRunsNow.length === 1
                                  ? qsTr("Running: %1").arg(page.activeRunsNow[0].title)
                                  : qsTr("%1 running").arg(page.activeRunsNow.length)
                            color: page.colors.accent
                            font.pixelSize: page.hostWindow.dp(11)
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (page.activeRunsNow.length === 1)
                                page.focusRunToday(page.activeRunsNow[0].runId)
                            else
                                activeRunsPopup.open()
                        }
                    }

                    Popup {
                        id: activeRunsPopup
                        y: activeRunsPill.height + page.hostWindow.dp(6)
                        width: page.hostWindow.dp(300)
                        modal: false
                        focus: true
                        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                        padding: page.hostWindow.dp(6)

                        background: Rectangle {
                            radius: page.colors.cornerRadiusMedium
                            color: page.colors.popupBackground
                            border.width: 1
                            border.color: page.colors.border
                        }

                        contentItem: ColumnLayout {
                            spacing: page.hostWindow.dp(2)

                            Repeater {
                                model: page.activeRunsNow

                                delegate: Rectangle {
                                    id: activeRunRow
                                    required property var modelData
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: page.hostWindow.dp(40)
                                    radius: page.colors.cornerRadiusSmall
                                    color: activeRunRowMouse.containsMouse
                                           ? page.colors.buttonHover : "transparent"

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.margins: page.hostWindow.dp(8)
                                        spacing: page.hostWindow.dp(8)

                                        Rectangle {
                                            Layout.preferredWidth: page.hostWindow.dp(6)
                                            Layout.preferredHeight: page.hostWindow.dp(6)
                                            radius: width / 2
                                            color: page.colors.accent
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 0
                                            Text {
                                                Layout.fillWidth: true
                                                text: activeRunRow.modelData.title
                                                color: page.colors.textPrimary
                                                font.pixelSize: page.hostWindow.dp(11)
                                                font.weight: Font.Medium
                                                elide: Text.ElideRight
                                            }
                                            Text {
                                                text: page.statusLabel(activeRunRow.modelData.status)
                                                color: page.colors.textMuted
                                                font.pixelSize: page.hostWindow.dp(9)
                                            }
                                        }
                                    }

                                    MouseArea {
                                        id: activeRunRowMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            activeRunsPopup.close()
                                            page.focusRunToday(activeRunRow.modelData.runId)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                RunActionButton {
                    objectName: "runsDraftsButton"
                    hostWindow: page.hostWindow; colors: page.colors
                    text: page.draftsSnapshot.length > 0
                          ? qsTr("Drafts (%1)").arg(page.draftsSnapshot.length)
                          : qsTr("Drafts")
                    iconText: "▤"
                    onClicked: page.openDrafts()
                }
                RunActionButton {
                    hostWindow: page.hostWindow; colors: page.colors
                    text: qsTr("Refresh"); iconText: "↻"
                    onClicked: {
                        // backend.runs.model is a single shared instance across
                        // every page (RunController owns one model, not one per
                        // page). Project narrowing is already applied entirely
                        // client-side via filteredRuns, so this must always
                        // reload every project - passing the current project
                        // filter here used to leave the shared model narrowed
                        // to one project until something else (e.g.
                        // OverviewPage's own onVisibleChanged) widened it back.
                        page.backend.runs.refresh("")
                        // The handoff read model is separate from the shared
                        // Run list. Refresh its Backend-owned eligibility too,
                        // so a just-finished selected Run never requires an
                        // application restart before Create becomes available.
                        if (page.backend.manualHandoff)
                            page.backend.manualHandoff.refresh()
                        page.refreshRequested()
                        page.notify(qsTr("Refreshed"))
                    }
                }
                RunActionButton {
                    hostWindow: page.hostWindow; colors: page.colors
                    text: qsTr("New run"); iconText: "+"; primary: true
                    enabled: page.projectsSnapshot.length > 0
                    onClicked: page.openNewRunDialog()
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: page.hostWindow.dp(58)
                radius: page.colors.cornerRadiusMedium
                color: page.colors.surfaceAlternative
                border.width: 1
                border.color: page.colors.border

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: page.hostWindow.dp(9)
                    spacing: page.hostWindow.dp(9)
                    Basic.TextField {
                        Layout.fillWidth: true
                        placeholderText: qsTr("Search runs")
                        text: page.searchText
                        font.pixelSize: page.hostWindow.dp(12)
                        color: page.colors.textPrimary
                        placeholderTextColor: page.colors.textMuted
                        leftPadding: page.hostWindow.dp(12)
                        onTextChanged: page.searchText = text
                        background: Rectangle {
                            radius: page.colors.cornerRadiusSmall
                            color: page.colors.inputBackground
                            border.width: parent.activeFocus ? 2 : 1
                            border.color: parent.activeFocus ? page.colors.inputBorderFocused : page.colors.inputBorder
                        }
                    }
                    RunComboBox {
                        objectName: "runsProjectFilterCombo"
                        Layout.preferredWidth: page.hostWindow.dp(205)
                        hostWindow: page.hostWindow; colors: page.colors
                        options: page.projectOptions
                        selectedValue: page.projectFilter
                        accessibleLabel: qsTr("Filter runs by project")
                        onValueSelected: value => {
                            page.applyProjectFilter(value === "all" ? "" : value)
                            page.projectSelectionRequested(value === "all" ? "" : value)
                        }
                    }
                    RunComboBox {
                        Layout.preferredWidth: page.hostWindow.dp(165)
                        hostWindow: page.hostWindow; colors: page.colors
                        options: page.statusOptions
                        selectedValue: page.statusFilter
                        accessibleLabel: qsTr("Filter runs by status")
                        onValueSelected: value => page.statusFilter = value
                    }
                    RunComboBox {
                        objectName: "runsManualResolutionFilterCombo"
                        Layout.preferredWidth: page.hostWindow.dp(220)
                        hostWindow: page.hostWindow; colors: page.colors
                        options: page.manualResolutionOptions
                        selectedValue: page.manualResolutionFilter
                        accessibleLabel: qsTr("Filter runs by manual resolution")
                        onValueSelected: value => page.manualResolutionFilter = value
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: tasksColumn.implicitHeight + page.hostWindow.dp(20)
                visible: page.projectFilter !== "all"
                radius: page.colors.cornerRadiusMedium
                color: page.colors.accentSoft
                border.width: 1
                border.color: page.colors.accent

                ColumnLayout {
                    id: tasksColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: page.hostWindow.dp(12)
                    anchors.rightMargin: page.hostWindow.dp(12)
                    spacing: page.hostWindow.dp(7)

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Project tasks")
                            color: page.colors.textPrimary
                            font.pixelSize: page.hostWindow.dp(12)
                            font.weight: Font.DemiBold
                        }
                        Text {
                            text: qsTr("%1 open").arg(page.projectTasks.length)
                            color: page.colors.accent
                            font.pixelSize: page.hostWindow.dp(9)
                            font.weight: Font.Medium
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: page.projectTasks.length === 0
                        text: qsTr("No open tasks for this project. You can still create a standalone run.")
                        color: page.colors.textMuted
                        font.pixelSize: page.hostWindow.dp(9)
                        wrapMode: Text.WordWrap
                    }

                    Repeater {
                        model: page.projectTasks.slice(0, 3)
                        delegate: Rectangle {
                            id: runTaskItem
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.preferredHeight: page.hostWindow.dp(36)
                            radius: page.colors.cornerRadiusSmall
                            color: runTaskMouse.containsMouse ? page.colors.surfaceRaised : page.colors.surface
                            border.width: 1
                            border.color: runTaskItem.modelData.requiresAttention
                                          ? page.colors.warning : page.colors.border

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: page.hostWindow.dp(9)
                                anchors.rightMargin: page.hostWindow.dp(9)
                                spacing: page.hostWindow.dp(8)
                                Rectangle {
                                    Layout.preferredWidth: page.hostWindow.dp(5)
                                    Layout.preferredHeight: page.hostWindow.dp(5)
                                    radius: width / 2
                                    color: runTaskItem.modelData.status === "blocked"
                                           ? page.colors.error : page.colors.accent
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: runTaskItem.modelData.title
                                    color: page.colors.textPrimary
                                    font.pixelSize: page.hostWindow.dp(9.5)
                                    elide: Text.ElideRight
                                }
                                Text {
                                    text: qsTr("Create run")
                                    color: page.colors.accent
                                    font.pixelSize: page.hostWindow.dp(8.5)
                                }
                            }
                            MouseArea {
                                id: runTaskMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: page.openNewRunDialog(runTaskItem.modelData.taskId)
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: page.projectTasks.length > 3
                        text: qsTr("+ %1 more tasks").arg(page.projectTasks.length - 3)
                        color: page.colors.textMuted
                        font.pixelSize: page.hostWindow.dp(8.5)
                    }
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: page.logicalWidth >= 880 ? 4 : 2
                columnSpacing: page.hostWindow.dp(10)
                rowSpacing: page.hostWindow.dp(10)
                RunMetricCard {
                    Layout.fillWidth: true; hostWindow: page.hostWindow; colors: page.colors
                    title: qsTr("Running"); value: String(page.runningCount)
                    detail: qsTr("%1 queued").arg(page.queuedCount); accentColor: page.colors.information
                }
                RunMetricCard {
                    Layout.fillWidth: true; hostWindow: page.hostWindow; colors: page.colors
                    title: qsTr("Success rate")
                    value: page.terminalCount > 0 ? (page.successRate + "%") : "—"
                    detail: qsTr("All time"); accentColor: page.colors.success
                }
                RunMetricCard {
                    Layout.fillWidth: true; hostWindow: page.hostWindow; colors: page.colors
                    title: qsTr("Tokens"); value: page.formatTokens(page.totalTokens)
                    detail: qsTr("All time"); accentColor: page.colors.accent
                }
                RunMetricCard {
                    Layout.fillWidth: true; hostWindow: page.hostWindow; colors: page.colors
                    title: qsTr("Failed"); value: String(page.failedCount)
                    detail: qsTr("%1 cancelled").arg(page.cancelledCount); accentColor: page.colors.warning
                }
            }

            // Anchor-based two-pane area: the run list is pinned to the
            // parent's top-left at a fixed width and stays content-sized
            // (collapsing history shrinks it), while Run Overview anchors
            // its left edge to the list's right edge and its right edge to
            // the parent — it always absorbs exactly the remaining width.
            // Keep the split view until the logical content width is genuinely
            // narrow. Using dp() here prevents high-DPI scaling from switching
            // to the stacked layout too early.
            Item {
                id: listDetailArea
                readonly property bool twoColumns: width >= page.hostWindow.dp(760)
                readonly property real paneSpacing: page.hostWindow.dp(12)
                readonly property bool calendarMode: page.selectedHistoryDateKey.length === 0
                Layout.fillWidth: true
                implicitHeight: calendarMode
                                ? page.hostWindow.dp(650)
                                : twoColumns
                                  ? Math.max(runListPanel.height, runDetailsPanel.height)
                                  : runListPanel.height + paneSpacing + runDetailsPanel.height

                // The initial history browser is a peer of the two-pane view,
                // not a child squeezed into the narrow RunListPanel column.
                Rectangle {
                    id: calendarSurface
                    anchors.fill: parent
                    visible: listDetailArea.calendarMode
                    radius: page.colors.cornerRadiusMedium
                    color: page.colors.surfaceAlternative
                    border.width: 1
                    border.color: page.colors.border
                    clip: true

                    RunCalendarView {
                        anchors.fill: parent
                        anchors.margins: page.hostWindow.dp(20)
                        hostWindow: page.hostWindow
                        colors: page.colors
                        runsByDay: page.historyByDay
                        selectedDateKey: page.selectedHistoryDateKey
                        expanded: true
                        onDateSelected: key => page.showHistoryDate(key)
                    }
                }

                RunListPanel {
                    id: runListPanel
                    visible: !listDetailArea.calendarMode
                    anchors.top: listDetailArea.twoColumns
                                 ? parent.top : runDetailsPanel.bottom
                    anchors.topMargin: listDetailArea.twoColumns
                                       ? 0 : listDetailArea.paneSpacing
                    anchors.left: parent.left
                    width: listDetailArea.twoColumns
                           ? page.hostWindow.dp(340) : listDetailArea.width
                    hostWindow: page.hostWindow
                    colors: page.colors
                    runsModel: page.filteredRuns
                    projectFilterLabel: page.projectFilterLabel
                    selectedDateKey: page.selectedHistoryDateKey
                    selectedRunId: page.selectedRunId
                    onRunSelected: runId => page.selectRun(runId)
                    onCalendarRequested: page.showHistoryCalendar()
                }

                RunDetailsPanel {
                    id: runDetailsPanel
                    visible: !listDetailArea.calendarMode
                    anchors.top: parent.top
                    anchors.left: listDetailArea.twoColumns
                                  ? runListPanel.right : parent.left
                    anchors.leftMargin: listDetailArea.twoColumns
                                        ? listDetailArea.paneSpacing : 0
                    anchors.right: parent.right
                    height: page.hostWindow.dp(650)
                    hostWindow: page.hostWindow
                    colors: page.colors
                    runData: page.selectedRun
                    selectedDateHasNoRuns: page.selectedHistoryDateKey.length > 0
                                           && (page.historyByDay[page.selectedHistoryDateKey] || []).length === 0
                    agentSessionController: page.backend.agentSessions
                    manifestController: page.backend.manifest
                    manualHandoffController: page.backend.manualHandoff
                    outcomeBriefController: page.backend.outcomeBrief
                    followUpPlanController: page.backend.followUpPlan
                    runsController: page.backend.runs
                    nowMs: page.nowMs
                    onCancelRequested: runId => page.cancelRun(runId)
                    onRetryRequested: runId => page.retryRun(runId)
                    onArtifactRequested: (runId, artifactId) => {
                        page.artifactOpenRequested(runId, artifactId)
                        if (!page.backend.runs.openArtifact(artifactId))
                            page.notify(page.backend.runs.lastError)
                    }
                    onArtifactsFolderRequested: (runId, artifactId) => {
                        if (!page.backend.runs.openArtifactsFolder(artifactId))
                            page.notify(page.backend.runs.lastError)
                    }
                    onViewContextRequested: (runId, artifactId) => page.viewArtifactContext(artifactId)
                    onEditRequested: runId => page.openEditRunDialog(runId)
                    onCompareRequested: runId => page.comparisonRequested(runId)
                    onOpenDraftsRequested: page.openDrafts()
                }
            }
        }
    }

    Loader {
        id: newRunDialogLoader
        anchors.fill: parent
        active: false
        sourceComponent: Component {
            NewRunDialog {
                hostWindow: page.hostWindow
                colors: page.colors
                backend: page.backend
                projectsModel: page.backend.projects.model
                projectsSnapshot: page.projectsSnapshot
                executionProfilesModel: page.backend.executionProfiles.runnableProfiles
                taskChoices: page.tasksSnapshot
                agentChoices: page.agentChoicesSnapshot
                agentChoicesLoading: page.backend.runs.agentChoicesLoading
                defaultExecutionProfileId:
                    page.backend.settings.defaultExecutionProfileId.length > 0
                    ? page.backend.settings.defaultExecutionProfileId
                    : page.backend.executionProfiles.defaultProfileId
                defaultTimeoutMs: Number(page.backend.settings.defaultTimeoutMs)
                defaultMaxParallelAgents: page.backend.settings.defaultMaxParallelAgents
                preferredProjectId: page.activeProjectId
            }
        }
    }

    Connections {
        target: newRunDialogLoader.item
        ignoreUnknownSignals: true
        function onRunAccepted(projectId, title, task, executionProfileId,
                               projectTaskId, workspacePath, sourceRunId,
                               allowEvidenceBackedNoChanges,
                               automaticVerificationRepair) {
            page.startRun(projectId, title, task, executionProfileId,
                          projectTaskId, workspacePath, sourceRunId,
                          allowEvidenceBackedNoChanges,
                          automaticVerificationRepair)
        }
        function onRosterRunAccepted(projectId, title, task, roster, maxParallelism,
                                     projectTaskId, workspacePath, sourceRunId,
                                     allowEvidenceBackedNoChanges,
                                     automaticVerificationRepair, workflowId, providerBudget,
                                     interfaceContracts) {
            page.startRunWithRoster(projectId, title, task, roster, maxParallelism,
                                    projectTaskId, workspacePath, sourceRunId,
                                    allowEvidenceBackedNoChanges,
                                    automaticVerificationRepair, workflowId, providerBudget,
                                    interfaceContracts)
        }
        function onSaveDraftRequested(projectId, title, task, launchMode,
                                      executionProfileId, roster, maxParallelism, workspacePath,
                                      automaticVerificationRepair, workflowId, providerBudget,
                                      interfaceContracts) {
            page.saveDraft(projectId, title, task, launchMode,
                           executionProfileId, roster, maxParallelism, workspacePath,
                           automaticVerificationRepair, workflowId, providerBudget,
                           interfaceContracts)
        }
        function onUpdateDraftRequested(draftId, projectId, title, task, launchMode,
                                        executionProfileId, roster, maxParallelism, workspacePath,
                                        automaticVerificationRepair, workflowId, providerBudget,
                                        interfaceContracts) {
            page.updateDraft(draftId, projectId, title, task, launchMode,
                             executionProfileId, roster, maxParallelism, workspacePath,
                             automaticVerificationRepair, workflowId, providerBudget,
                             interfaceContracts)
        }
        // A proposal-roster Run keeps its legacy Edit & rerun; converting it is a separate,
        // explicit choice made from that dialog. The preview is re-read so the confirmation
        // fingerprint is the Backend's current one, not a copy carried inside the draft.
        function onConversionRequested(sourceRunId) {
            const fresh = page.backend.runs.legacyConversionPreview(sourceRunId)
            if (!fresh || !fresh.sourceRunId) {
                page.notify(page.backend.runs.lastError)
                return
            }
            page.openConversionDialog(fresh)
        }
    }

    Loader {
        id: legacyConversionDialogLoader
        objectName: "legacyConversionDialogLoader"
        anchors.fill: parent
        active: false
        sourceComponent: Component {
            LegacyConversionDialog {
                hostWindow: page.hostWindow
                colors: page.colors
                onClosed: legacyConversionDialogLoader.active = false
            }
        }
    }

    Connections {
        target: legacyConversionDialogLoader.item
        ignoreUnknownSignals: true
        function onConvertRequested(sourceRunId, title, task, workspacePath, maxParallelism,
                                    allowEvidenceBackedNoChanges, automaticVerificationRepair,
                                    expectedConfirmationFingerprint) {
            page.convertLegacyRun(sourceRunId, title, task, workspacePath, maxParallelism,
                                  allowEvidenceBackedNoChanges, automaticVerificationRepair,
                                  expectedConfirmationFingerprint)
        }
    }

    // A Retry the Backend refused because converting that legacy Run drops settings the user must
    // confirm: no Run was created, so the conversion dialog opens for it.
    Connections {
        target: page.backend.runs
        function onLegacyConversionRequested(sourceRunId) {
            page.openEditRunDialog(sourceRunId)
        }
    }

    Loader {
        id: draftsDialogLoader
        objectName: "draftsDialogLoader"
        anchors.fill: parent
        active: false
        sourceComponent: Component {
            DraftsDialog {
                hostWindow: page.hostWindow
                colors: page.colors
                draftsModel: page.draftsSnapshot
                projectsSnapshot: page.projectsSnapshot
                // A card is read completely only when it is expanded.
                detailsFor: function (draftId) { return page.backend.runDrafts.openDraft(draftId) }
                onOpenRequested: draftId => page.openDraftInDialog(draftId)
                onDeleteRequested: draftId => page.deleteDraft(draftId)
                onClosed: draftsDialogLoader.active = false
            }
        }
        onActiveChanged: if (active) Qt.callLater(function () {
            if (draftsDialogLoader.item)
                draftsDialogLoader.item.open()
        })
    }

    Loader {
        id: artifactContentDialogLoader
        anchors.fill: parent
        active: false
        sourceComponent: Component {
            ArtifactContentDialog {
                hostWindow: page.hostWindow
                colors: page.colors
            }
        }
    }

    Rectangle {
        visible: page.notificationText.length > 0
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: page.hostWindow.dp(18)
        width: notificationLabel.implicitWidth + page.hostWindow.dp(28)
        height: page.hostWindow.dp(38)
        radius: height / 2
        color: page.colors.surfaceRaised
        border.width: 1
        border.color: page.colors.borderStrong
        Text {
            id: notificationLabel
            anchors.centerIn: parent
            text: page.notificationText
            color: page.colors.textPrimary
            font.pixelSize: page.hostWindow.dp(11)
        }
    }
}
