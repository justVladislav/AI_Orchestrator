import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Overview_Objects"
import "../Runs/Runs_Objects"
import "../Projects/Projects_Objets"

Item {
    id: page

    required property var hostWindow
    required property var colors
    required property var backend

    signal projectSelected(string projectId)

    // ── Stable ID-based selection ────────────────────────────────────────────
    // selectedProjectId is the anchor; selectedProjectIndex and selectedProject
    // are derived so ComboBox/panel highlighting stays consistent after model
    // resets, reorders, creates, and archives.
    property string selectedProjectId: ""
    readonly property string activeProjectId: page.backend.workspace.activeProjectId

    readonly property real logicalWidth: width / Math.max(hostWindow.scaleFactor, 0.01)

    // ── Plain-JS mirrors of the live Qt Adapter models ───────────────────────
    // These are rebuilt whenever the underlying C++ model emits dataChanged or
    // modelReset; no business state is invented here.
    property var projectsSnapshot: []
    property var runsSnapshot:     []
    property var tasksSnapshot:    []
    property var providerAnalyticsSnapshot: []
    property bool runsSnapshotRebuildScheduled: false
    property var appliedFromDate: undefined
    property var appliedToDate: undefined
    property string dateRangeError: ""

    readonly property var filteredRunsSnapshot: runsSnapshot.filter(function(run) {
        return page.dateInAppliedRange(run.createdAt)
    })
    readonly property var filteredTasksSnapshot: tasksSnapshot.filter(function(task) {
        return page.dateInAppliedRange(task.updatedAt)
    })
    readonly property bool dateRangeActive: page.validDate(page.appliedFromDate)
                                             || page.validDate(page.appliedToDate)
    readonly property string dateRangeLabel: {
        if (!page.dateRangeActive)
            return qsTr("All time")
        const fromLabel = page.validDate(page.appliedFromDate)
                ? Qt.formatDate(page.appliedFromDate, "d MMM yyyy") : qsTr("Any date")
        const toLabel = page.validDate(page.appliedToDate)
                ? Qt.formatDate(page.appliedToDate, "d MMM yyyy") : qsTr("Today")
        return qsTr("%1 – %2").arg(fromLabel).arg(toLabel)
    }

    // ── Enriched per-project array consumed by sub-components ────────────────
    // Re-evaluated whenever projectsSnapshot or runsSnapshot changes because
    // both are plain JS array references reassigned by rebuildXxxSnapshot().
    readonly property var liveProjectsModel: {
        const result = []
        for (let i = 0; i < projectsSnapshot.length; ++i) {
            const p = projectsSnapshot[i]
            if (page.dateRangeActive && !page.dateInAppliedRange(p.createdAt))
                continue
            const projectRuns = filteredRunsSnapshot.filter(r => r.projectId === p.projectId)
            const projectTasks = filteredTasksSnapshot.filter(t => t.projectId === p.projectId)

            const completedRuns    = projectRuns.filter(r => r.status === "completed")
            const failedRuns       = projectRuns.filter(r => r.status === "failed")
            const interruptedRuns  = projectRuns.filter(r => r.status === "interrupted")
            const cancelledRuns    = projectRuns.filter(r => r.status === "cancelled")
            const cancellingRuns   = projectRuns.filter(r => r.status === "cancelling")
            const activeRuns       = projectRuns.filter(r => r.status === "running"
                                                          || r.status === "queued"
                                                          || r.status === "cancelling")

            const terminalCount = completedRuns.length + failedRuns.length
                                  + cancelledRuns.length + interruptedRuns.length
            const successRate   = terminalCount > 0
                                  ? Math.round(completedRuns.length / terminalCount * 100)
                                  : 0

            // Prefer a "running" run over "queued" or "cancelling".
            const runningRun = page.newestRun(projectRuns.filter(r => r.status === "running"))
            const activeRun  = runningRun || page.newestRun(activeRuns)
            const lastRun    = page.newestRun(projectRuns)

            const totalInputTokens  = projectRuns.reduce((s, r) => s + r.inputTokens,  0)
            const totalOutputTokens = projectRuns.reduce((s, r) => s + r.outputTokens, 0)

            // Keep attention task-only until another domain exposes a durable
            // actionable/acknowledged contract. Historical run status belongs
            // to activity and does not prove that user action is still pending.
            const attention = []
            const now = Date.now()
            const actionableTasks = projectTasks.filter(t => t.status !== "done" && t.status !== "cancelled")
            for (let taskIndex = 0; taskIndex < actionableTasks.length; ++taskIndex) {
                const task = actionableTasks[taskIndex]
                const overdue = task.dueAt && task.dueAt.getTime && task.dueAt.getTime() < now
                if (!task.requiresAttention && task.status !== "blocked" && !overdue)
                    continue
                attention.push({
                    kind: "task",
                    severity: task.status === "blocked" || overdue ? "warning" : "information",
                    title: task.title,
                    detail: task.status === "blocked" ? qsTr("Task is blocked")
                          : overdue ? qsTr("Task is overdue") : qsTr("Marked for attention"),
                    time: task.dueAt ? Qt.formatDate(task.dueAt, "yyyy-MM-dd")
                                     : page.formatRelativeTime(task.updatedAt),
                    taskData: task
                })
            }
            attention.sort(function(left, right) {
                const leftTask = left.taskData
                const rightTask = right.taskData
                const leftRank = leftTask.status === "blocked" ? 0
                               : leftTask.dueAt ? 1 : 2
                const rightRank = rightTask.status === "blocked" ? 0
                                : rightTask.dueAt ? 1 : 2
                if (leftRank !== rightRank)
                    return leftRank - rightRank
                return page.timestampValue(leftTask.dueAt || leftTask.updatedAt)
                     - page.timestampValue(rightTask.dueAt || rightTask.updatedAt)
            })

            // Short task label for the project row in OverviewProjectsPanel.
            let taskLabel = ""
            if (activeRuns.length > 0)
                taskLabel = qsTr("%1 active").arg(activeRuns.length)
            else if (projectRuns.length > 0)
                taskLabel = qsTr("%1 run(s)").arg(projectRuns.length)
            else
                taskLabel = qsTr("No runs yet")

            // Display progress: active run's progress, then last run's, then 0.
            const displayProgress = activeRun ? activeRun.progress
                                  : lastRun   ? lastRun.progress
                                  : 0.0

            // Map project status to the color categories OverviewProjectsPanel knows.
            const displayStatus = p.status === "active"   ? "active"
                                : p.status === "archived" ? "archived"
                                : p.status

            // Last meaningful activity time for "X min ago" display.
            const lastActivityDate = lastRun
                ? (lastRun.finishedAt || lastRun.startedAt || lastRun.createdAt)
                : p.updatedAt

            const activity = []
            for (let runIndex = 0; runIndex < projectRuns.length; ++runIndex) {
                const run = projectRuns[runIndex]
                activity.push({
                    title: run.title,
                    detail: qsTr("Run · %1").arg(run.status),
                    time: page.formatRelativeTime(run.updatedAt),
                    timestamp: page.timestampValue(run.updatedAt),
                    color: run.status === "completed" ? page.colors.success
                         : run.status === "failed" ? page.colors.error
                         : run.status === "running" ? page.colors.information
                         : page.colors.textMuted
                })
            }
            for (let activityTaskIndex = 0; activityTaskIndex < projectTasks.length; ++activityTaskIndex) {
                const task = projectTasks[activityTaskIndex]
                activity.push({
                    title: task.title,
                    detail: qsTr("Task · %1").arg(task.status),
                    time: page.formatRelativeTime(task.updatedAt),
                    timestamp: page.timestampValue(task.updatedAt),
                    color: task.requiresAttention ? page.colors.warning
                         : task.status === "done" ? page.colors.success
                         : page.colors.accent
                })
            }
            activity.sort((left, right) => right.timestamp - left.timestamp)

            result.push({
                projectId:          p.projectId,
                name:               p.name,
                status:             displayStatus,
                updated:            page.formatRelativeTime(lastActivityDate),
                taskLabel:          taskLabel,
                progress:           displayProgress,
                runCount:           projectRuns.length,
                runningCount:       projectRuns.filter(r => r.status === "running").length,
                queuedCount:        projectRuns.filter(r => r.status === "queued").length,
                completedCount:     completedRuns.length,
                failedCount:        failedRuns.length,
                cancelledCount:     cancelledRuns.length,
                interruptedCount:   interruptedRuns.length,
                terminalCount:      terminalCount,
                successRate:        successRate,
                totalInputTokens:   totalInputTokens,
                totalOutputTokens:  totalOutputTokens,
                totalTokens:        totalInputTokens + totalOutputTokens,
                taskCount:          projectTasks.length,
                activeTaskCount:    actionableTasks.length,
                activeRunTitle:     activeRun ? activeRun.title  : "",
                activeRunStatus:    activeRun ? activeRun.status : (lastRun ? lastRun.status : ""),
                attention:          attention,
                activity:           activity.slice(0, 5)
            })
        }
        return result
    }

    // The currently selected project entry, falling back to the first when the
    // stored ID is stale (project archived/deleted between sessions).
    readonly property var selectedProject: {
        if (liveProjectsModel.length === 0)
            return null
        for (let i = 0; i < liveProjectsModel.length; ++i) {
            if (liveProjectsModel[i].projectId === selectedProjectId)
                return liveProjectsModel[i]
        }
        return liveProjectsModel[0]
    }

    // Index into liveProjectsModel for ComboBox/panel highlight synchronisation.
    readonly property int selectedProjectIndex: {
        if (!selectedProject || liveProjectsModel.length === 0)
            return -1
        for (let i = 0; i < liveProjectsModel.length; ++i) {
            if (liveProjectsModel[i].projectId === selectedProject.projectId)
                return i
        }
        return 0
    }

    // ── Helpers ──────────────────────────────────────────────────────────────

    function timestampValue(date) {
        return date && date.getTime ? date.getTime() : 0
    }

    function validDate(date) {
        return date !== null && date !== undefined && date.getTime
                && !isNaN(date.getTime())
    }

    function startOfDay(date) {
        return new Date(date.getFullYear(), date.getMonth(), date.getDate(), 0, 0, 0, 0)
    }

    function nextDayStart(date) {
        return new Date(date.getFullYear(), date.getMonth(), date.getDate() + 1, 0, 0, 0, 0)
    }

    function dateInAppliedRange(date) {
        if (!page.validDate(date))
            return !page.dateRangeActive
        const timestamp = date.getTime()
        return (!page.validDate(page.appliedFromDate)
                || timestamp >= page.startOfDay(page.appliedFromDate).getTime())
            && (!page.validDate(page.appliedToDate)
                || timestamp < page.nextDayStart(page.appliedToDate).getTime())
    }

    function applyDateRange() {
        const from = fromDatePicker._effectiveDate()
        const to = toDatePicker._effectiveDate()
        if (from && to && page.startOfDay(from).getTime() > page.startOfDay(to).getTime()) {
            page.dateRangeError = qsTr("The From date must not be later than the To date.")
            return
        }
        page.dateRangeError = ""
        page.appliedFromDate = from || undefined
        page.appliedToDate = to || undefined
        Qt.callLater(page.ensureSelectionMatchesDateRange)
    }

    function clearDateRange() {
        fromDatePicker.selectedDate = new Date(NaN)
        toDatePicker.selectedDate = new Date(NaN)
        page.appliedFromDate = undefined
        page.appliedToDate = undefined
        page.dateRangeError = ""
        Qt.callLater(page.ensureSelectionMatchesDateRange)
    }

    function ensureSelectionMatchesDateRange() {
        const visibleProjects = page.liveProjectsModel
        const selectedIsVisible = visibleProjects.some(function(project) {
            return project.projectId === page.selectedProjectId
        })
        if (!selectedIsVisible) {
            const activeIsVisible = visibleProjects.some(function(project) {
                return project.projectId === page.activeProjectId
            })
            page.selectedProjectId = activeIsVisible
                    ? page.activeProjectId
                    : (visibleProjects.length > 0 ? visibleProjects[0].projectId : "")
        }
        page.refreshAnalytics()
    }

    function newestRun(runs) {
        // Repository order is currently oldest-first, but Overview must not
        // rely on adapter ordering. Resolve the latest run explicitly so a
        // newer cancelled/failed run cannot be hidden behind older progress.
        let newest = null
        for (let index = 0; index < runs.length; ++index) {
            if (!newest
                    || page.timestampValue(runs[index].createdAt)
                       > page.timestampValue(newest.createdAt)) {
                newest = runs[index]
            }
        }
        return newest
    }

    function formatRelativeTime(date) {
        if (!date)
            return qsTr("—")
        const diffMs = Date.now() - date.getTime()
        if (diffMs < 0)
            return Qt.formatDateTime(date, "yyyy-MM-dd HH:mm")
        const diffSec = Math.floor(diffMs / 1000)
        if (diffSec < 60)  return qsTr("Just now")
        const diffMin = Math.floor(diffSec  / 60)
        if (diffMin < 60)  return qsTr("%1 min ago").arg(diffMin)
        const diffH   = Math.floor(diffMin  / 60)
        if (diffH   < 24)  return qsTr("%1 h ago").arg(diffH)
        const diffDay = Math.floor(diffH    / 24)
        if (diffDay === 1) return qsTr("Yesterday")
        if (diffDay <  7)  return qsTr("%1 days ago").arg(diffDay)
        return Qt.formatDateTime(date, "yyyy-MM-dd")
    }

    function formatTokens(count) {
        if (count >= 1000000) return (count / 1000000).toFixed(1) + "M"
        if (count >= 1000)    return (count / 1000).toFixed(1) + "K"
        return String(count)
    }

    // Wording of a Backend price state. priceState and costText come from the Qt layer (the amount is
    // formatted there): this function only chooses the localized phrase. It never reads or formats a
    // raw amount, and an unknown price is never shown as "$0".
    //   known   - the plain amount
    //   partial - a lower bound: "At least <amount>"
    //   unknown - no amount at all: "Cost unavailable"
    //   none    - no data (no executions)
    function costWording(priceState, costText) {
        if (priceState === "known")
            return costText
        if (priceState === "partial")
            return qsTr("At least %1").arg(costText)
        if (priceState === "unknown")
            return qsTr("Cost unavailable")
        return "—"
    }

    function refreshAnalytics() {
        if (!page.selectedProjectId) {
            page.backend.executionAnalytics.clear()
            return
        }
        if (!page.dateRangeActive) {
            page.backend.executionAnalytics.refreshProject(page.selectedProjectId)
            return
        }
        const from = page.validDate(page.appliedFromDate)
                ? page.startOfDay(page.appliedFromDate) : new Date(0)
        const to = page.validDate(page.appliedToDate)
                ? page.nextDayStart(page.appliedToDate) : new Date(253402300799999)
        page.backend.executionAnalytics.refreshProjectRange(page.selectedProjectId, from, to)
    }

    // Everything price-related comes from the Qt layer: the wording (priceState + costText), the
    // incomplete flag and the chart share. The share is only a number between EXACT amounts - with a
    // partial/unknown provider shareBasis is "incomplete" and the chart shows no bar.
    function rebuildProviderAnalyticsSnapshot() {
        const raw = []
        for (let i = 0; i < analyticsProvidersMirror.count; ++i) {
            const item = analyticsProvidersMirror.itemAt(i)
            if (!item)
                continue
            const tokens = Number(item.inputTokens) + Number(item.outputTokens)
            const modelRows = []
            const sourceModels = item.models || []
            for (let modelIndex = 0; modelIndex < sourceModels.length; ++modelIndex) {
                const model = sourceModels[modelIndex]
                modelRows.push({
                    name: model.modelName,
                    executionCount: Number(model.executionCount),
                    inputLabel: page.formatTokens(Number(model.inputTokens)),
                    outputLabel: page.formatTokens(Number(model.outputTokens)),
                    costLabel: page.costWording(model.priceState, model.costText),
                    priceIncomplete: Boolean(model.priceIncomplete),
                    averageLatencyMs: Number(model.averageLatencyMs)
                })
            }
            raw.push({
                name: item.providerName,
                tokensLabel: page.formatTokens(tokens),
                costLabel: page.costWording(item.priceState, item.costText),
                priceIncomplete: item.priceIncomplete,
                share: Number(item.costShare),
                shareBasis: item.shareBasis,
                executionCount: Number(item.executionCount),
                models: modelRows
            })
        }
        page.providerAnalyticsSnapshot = raw
    }

    // Called by OverviewProjectSelector / OverviewProjectsPanel (index-based).
    function selectProjectByIndex(index) {
        if (index < 0 || index >= liveProjectsModel.length)
            return
        const id = liveProjectsModel[index].projectId
        if (id === selectedProjectId)
            return
        selectedProjectId = id
        projectSelected(id)
    }

    // ── Model rebuild functions ───────────────────────────────────────────────

    function rebuildProjectsSnapshot() {
        const items = []
        for (let i = 0; i < projectsMirror.count; ++i) {
            const it = projectsMirror.itemAt(i)
            if (!it) continue
            items.push({
                projectId: it.projectId,
                name:      it.name,
                status:    it.status,
                createdAt: it.createdAt,
                updatedAt: it.updatedAt
            })
        }
        page.projectsSnapshot = items

        // Auto-select first project when none is selected yet or the stored ID
        // is no longer present in the live model (project deleted/archived away).
        const stillValid = items.some(p => p.projectId === page.selectedProjectId)
        if (!stillValid) {
            const activeIsVisible = items.some(p => p.projectId === page.activeProjectId)
            page.selectedProjectId = activeIsVisible
                    ? page.activeProjectId
                    : (items.length > 0 ? items[0].projectId : "")
        }
    }

    Connections {
        target: page.backend.workspace
        function onActiveProjectIdChanged() {
            if (page.liveProjectsModel.some(project =>
                    project.projectId === page.activeProjectId)) {
                page.selectedProjectId = page.activeProjectId
            }
        }
    }

    Connections {
        target: page.backend.executionAnalytics
        function onSummaryChanged() { Qt.callLater(page.rebuildProviderAnalyticsSnapshot) }
    }

    Connections {
        target: page.backend.chats
        function onConversationsChanged() {
            if (page.visible)
                page.refreshAnalytics()
        }
    }

    function rebuildRunsSnapshot() {
        const items = []
        for (let i = 0; i < runsMirror.count; ++i) {
            const it = runsMirror.itemAt(i)
            if (!it) continue
            items.push({
                runId:       it.runId,
                projectId:   it.projectId,
                title:       it.title,
                status:      it.status,
                progress:    it.progress,
                errorText:   it.errorMessage,
                inputTokens:  it.inputTokens  || 0,
                outputTokens: it.outputTokens || 0,
                createdAt:   it.createdAt,
                startedAt:   it.startedAt,
                finishedAt:  it.finishedAt,
                // Convenience field for "last touched" time
                updatedAt:   it.finishedAt || it.startedAt || it.createdAt
            })
        }
        page.runsSnapshot = items
    }

    function scheduleRunsSnapshotRebuild() {
        if (page.runsSnapshotRebuildScheduled)
            return
        page.runsSnapshotRebuildScheduled = true
        Qt.callLater(function() {
            page.runsSnapshotRebuildScheduled = false
            page.rebuildRunsSnapshot()
        })
    }

    function rebuildTasksSnapshot() {
        const items = []
        for (let i = 0; i < tasksMirror.count; ++i) {
            const it = tasksMirror.itemAt(i)
            if (!it) continue
            items.push({ taskId: it.taskId, projectId: it.projectId, title: it.title,
                description: it.description, status: it.status, priority: it.priority,
                requiresAttention: it.requiresAttention, dueAt: it.dueAt,
                dueLabel: it.dueAt ? Qt.formatDateTime(it.dueAt, "yyyy-MM-dd HH:mm") : "",
                plannedRunAt: it.plannedRunAt, updatedAt: it.updatedAt })
        }
        page.tasksSnapshot = items
    }

    // ── Non-visual mirrors ───────────────────────────────────────────────────
    // Hidden Repeaters expose C++ model roles to JS without a business-logic layer.

    Repeater {
        id: projectsMirror
        model: page.backend.projects.model
        onCountChanged: page.rebuildProjectsSnapshot()
        delegate: Item {
            required property string projectId
            required property string name
            required property string status
            required property var    updatedAt
            Component.onCompleted: page.rebuildProjectsSnapshot()
            onStatusChanged:       page.rebuildProjectsSnapshot()
        }
    }

    Repeater {
        id: runsMirror
        model: page.backend.runs.model
        onCountChanged: page.scheduleRunsSnapshotRebuild()
        delegate: Item {
            required property string runId
            required property string projectId
            required property string title
            required property string status
            required property real   progress
            required property string errorMessage
            required property var    inputTokens
            required property var    outputTokens
            required property var    createdAt
            required property var    startedAt
            required property var    finishedAt
            Component.onCompleted: page.scheduleRunsSnapshotRebuild()
            // One dataChanged can update several roles. Collapse all resulting
            // QML property signals into one JS snapshot rebuild per event loop.
            onStatusChanged:       page.scheduleRunsSnapshotRebuild()
            onProgressChanged:     page.scheduleRunsSnapshotRebuild()
            onErrorMessageChanged: page.scheduleRunsSnapshotRebuild()
            onInputTokensChanged:  page.scheduleRunsSnapshotRebuild()
            onOutputTokensChanged: page.scheduleRunsSnapshotRebuild()
            onStartedAtChanged:    page.scheduleRunsSnapshotRebuild()
            onFinishedAtChanged:   page.scheduleRunsSnapshotRebuild()
        }
    }

    Repeater {
        id: tasksMirror
        model: page.backend.tasks.model
        onCountChanged: page.rebuildTasksSnapshot()
        delegate: Item {
            required property string taskId
            required property string projectId
            required property string title
            required property string description
            required property string status
            required property string priority
            required property bool requiresAttention
            required property var dueAt
            required property var plannedRunAt
            required property var updatedAt
            Component.onCompleted: page.rebuildTasksSnapshot()
            onStatusChanged: page.rebuildTasksSnapshot()
            onRequiresAttentionChanged: page.rebuildTasksSnapshot()
        }
    }

    Connections {
        target: page.backend.runs
        function onRunFinished(runId, status) {
            if (page.visible)
                page.refreshAnalytics()
        }
    }

    Repeater {
        id: analyticsProvidersMirror
        model: page.backend.executionAnalytics.providers
        onCountChanged: page.rebuildProviderAnalyticsSnapshot()
        delegate: Item {
            required property string providerId
            required property string providerName
            required property var inputTokens
            required property var outputTokens
            required property string priceState
            required property string costText
            required property bool priceIncomplete
            required property real costShare
            required property string shareBasis
            required property var executionCount
            required property var models
            Component.onCompleted: page.rebuildProviderAnalyticsSnapshot()
        }
    }

    onSelectedProjectIdChanged: refreshAnalytics()

    // Ensure all runs are visible whenever the Overview page comes into view,
    // in case RunsPage had filtered the model to a specific project earlier.
    onVisibleChanged: {
        if (visible) {
            page.backend.runs.refresh("")
            page.refreshAnalytics()
        }
    }

    // ── UI ───────────────────────────────────────────────────────────────────

    Flickable {
        id: scrollView
        anchors.fill: parent
        contentWidth: width
        contentHeight: dashboardLayout.implicitHeight + page.colors.contentMargin * 2
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: RunScrollBar {
            id: overviewScrollBar
            hostWindow: page.hostWindow
            colors: page.colors
        }

        ColumnLayout {
            id: dashboardLayout
            x: page.colors.contentMargin
            y: page.colors.contentMargin
            width: Math.max(0, scrollView.width - page.colors.contentMargin * 2
                            - overviewScrollBar.width)
            spacing: page.hostWindow.dp(16)

            // ── Header row ───────────────────────────────────────────────────
            RowLayout {
                Layout.fillWidth: true
                spacing: page.hostWindow.dp(16)

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: page.hostWindow.dp(4)

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Overview")
                        color: page.colors.textPrimary
                        font.pixelSize: page.hostWindow.dp(28)
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Project progress, usage and decisions that need attention.")
                        color: page.colors.textSecondary
                        font.pixelSize: page.hostWindow.dp(13)
                        wrapMode: Text.WordWrap
                    }
                    Text {
                        Layout.fillWidth: true
                        text: page.backend.status
                        color: page.colors.textMuted
                        font.pixelSize: page.hostWindow.dp(10)
                        elide: Text.ElideRight
                    }
                }

                OverviewProjectSelector {
                    Layout.preferredWidth: page.hostWindow.dp(250)
                    Layout.alignment: Qt.AlignTop
                    hostWindow: page.hostWindow
                    colors: page.colors
                    projectsModel: page.liveProjectsModel
                    currentIndex: page.selectedProjectIndex
                    onProjectSelected: index => page.selectProjectByIndex(index)
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: dateRangeLayout.implicitHeight + page.hostWindow.dp(22)
                radius: page.colors.cornerRadiusMedium
                color: page.colors.surfaceAlternative
                border.width: 1
                border.color: page.dateRangeError.length > 0
                              ? page.colors.error : page.colors.border

                ColumnLayout {
                    id: dateRangeLayout
                    anchors.fill: parent
                    anchors.margins: page.hostWindow.dp(11)
                    spacing: page.hostWindow.dp(7)

                    RowLayout {
                        Layout.fillWidth: true

                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Analytics date range")
                            color: page.colors.textPrimary
                            font.pixelSize: page.hostWindow.dp(12)
                            font.weight: Font.DemiBold
                        }
                        Text {
                            text: page.dateRangeLabel
                            color: page.colors.textMuted
                            font.pixelSize: page.hostWindow.dp(10)
                        }
                    }

                    Flow {
                        Layout.fillWidth: true
                        Layout.preferredHeight: childrenRect.height
                        spacing: page.hostWindow.dp(8)

                        RowLayout {
                            spacing: page.hostWindow.dp(6)
                            Text {
                                text: qsTr("From")
                                color: page.colors.textSecondary
                                font.pixelSize: page.hostWindow.dp(10)
                            }
                            ProjectDatePicker {
                                id: fromDatePicker
                                objectName: "overviewFromDatePicker"
                                hostWindow: page.hostWindow
                                colors: page.colors
                                placeholderText: qsTr("Any date")
                            }
                        }

                        RowLayout {
                            spacing: page.hostWindow.dp(6)
                            Text {
                                text: qsTr("To")
                                color: page.colors.textSecondary
                                font.pixelSize: page.hostWindow.dp(10)
                            }
                            ProjectDatePicker {
                                id: toDatePicker
                                objectName: "overviewToDatePicker"
                                hostWindow: page.hostWindow
                                colors: page.colors
                                placeholderText: qsTr("Today")
                            }
                        }

                        RunActionButton {
                            hostWindow: page.hostWindow
                            colors: page.colors
                            text: qsTr("Apply")
                            primary: true
                            onClicked: page.applyDateRange()
                        }

                        RunActionButton {
                            hostWindow: page.hostWindow
                            colors: page.colors
                            text: qsTr("Clear")
                            enabled: page.dateRangeActive
                                     || fromDatePicker._hasDate() || toDatePicker._hasDate()
                            onClicked: page.clearDateRange()
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: page.dateRangeError.length > 0
                        text: page.dateRangeError
                        color: page.colors.error
                        font.pixelSize: page.hostWindow.dp(10)
                        wrapMode: Text.WordWrap
                    }
                }
            }

            // ── Empty state (no projects at all) ─────────────────────────────
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: page.hostWindow.dp(200)
                visible: page.liveProjectsModel.length === 0
                radius: page.colors.cornerRadiusMedium
                color: page.colors.surfaceAlternative
                border.width: 1
                border.color: page.colors.border

                ColumnLayout {
                    anchors.centerIn: parent
                    spacing: page.hostWindow.dp(10)
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: "□"
                        color: page.colors.textMuted
                        font.pixelSize: page.hostWindow.dp(36)
                    }
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: page.dateRangeActive
                              ? qsTr("No projects in this date range")
                              : qsTr("No projects yet")
                        color: page.colors.textPrimary
                        font.pixelSize: page.hostWindow.dp(14)
                        font.weight: Font.Medium
                    }
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: page.dateRangeActive
                              ? qsTr("No projects were created during the selected period. Clear or change the date range.")
                              : qsTr("Create a project from the Projects page to see metrics here.")
                        color: page.colors.textMuted
                        font.pixelSize: page.hostWindow.dp(11)
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                    }
                }
            }

            // ── KPI metric cards ─────────────────────────────────────────────
            GridLayout {
                Layout.fillWidth: true
                visible: page.liveProjectsModel.length > 0
                columns: page.logicalWidth >= 1000 ? 4 : 2
                columnSpacing: page.hostWindow.dp(12)
                rowSpacing: page.hostWindow.dp(12)

                OverviewMetricCard {
                    Layout.fillWidth: true
                    hostWindow: page.hostWindow; colors: page.colors
                    iconText: "%"; title: qsTr("Progress")
                    value: page.selectedProject
                           ? (page.selectedProject.hasOwnProperty("activeRunStatus")
                              && (page.selectedProject.activeRunStatus === "running"
                                  || page.selectedProject.activeRunStatus === "queued")
                              ? Math.round(page.selectedProject.progress * 100) + "%"
                              : (page.selectedProject.runCount > 0
                                 ? Math.round(page.selectedProject.progress * 100) + "%"
                                 : "—"))
                           : "—"
                    detail: page.selectedProject
                            ? (page.selectedProject.activeRunStatus === "running"
                               ? qsTr("Run in progress")
                               : page.selectedProject.taskLabel)
                            : qsTr("No project selected")
                    accentColor: page.colors.accent
                }

                OverviewMetricCard {
                    Layout.fillWidth: true
                    hostWindow: page.hostWindow; colors: page.colors
                    iconText: "R"; title: qsTr("Runs")
                    value: page.selectedProject ? String(page.selectedProject.runCount) : "0"
                    detail: page.selectedProject && page.selectedProject.terminalCount > 0
                            ? qsTr("%1% successful").arg(page.selectedProject.successRate)
                            : qsTr("No completed runs")
                    accentColor: page.colors.success
                }

                // Cost: the Backend's price state (known / partial / unknown / none) and its
                // pre-formatted amount; QML only words them. An unknown price is never "$0".
                OverviewMetricCard {
                    objectName: "projectCostCard"
                    Layout.fillWidth: true
                    hostWindow: page.hostWindow; colors: page.colors
                    iconText: "$"; title: qsTr("Project cost")
                    value: page.costWording(page.backend.executionAnalytics.priceState,
                                            page.backend.executionAnalytics.costText)
                    detail: page.backend.executionAnalytics.priceState === "none"
                            ? qsTr("No model executions yet")
                            : page.backend.executionAnalytics.priceState === "known"
                              ? qsTr("Across %1 model executions")
                                .arg(page.backend.executionAnalytics.executionCount)
                              : page.backend.executionAnalytics.priceState === "partial"
                                ? qsTr("%1 of %2 executions have an incomplete price")
                                  .arg(page.backend.executionAnalytics.unknownPriceExecutionCount)
                                  .arg(page.backend.executionAnalytics.executionCount)
                                : qsTr("%1 executions · set model pricing")
                                  .arg(page.backend.executionAnalytics.executionCount)
                    accentColor: page.colors.warning
                    incomplete: page.backend.executionAnalytics.priceIncomplete
                }

                OverviewMetricCard {
                    Layout.fillWidth: true
                    hostWindow: page.hostWindow; colors: page.colors
                    iconText: "T"; title: qsTr("Tokens")
                    value: page.backend.executionAnalytics.executionCount > 0
                           ? page.formatTokens(Number(page.backend.executionAnalytics.inputTokens)
                                               + Number(page.backend.executionAnalytics.outputTokens))
                           : page.selectedProject
                             ? page.formatTokens(page.selectedProject.totalTokens)
                             : "0"
                    detail: page.backend.executionAnalytics.executionCount > 0
                            ? qsTr("%1 in · %2 out")
                              .arg(page.formatTokens(page.backend.executionAnalytics.inputTokens))
                              .arg(page.formatTokens(page.backend.executionAnalytics.outputTokens))
                            : page.selectedProject && page.selectedProject.totalTokens > 0
                              ? qsTr("%1 in · %2 out")
                                .arg(page.formatTokens(page.selectedProject.totalInputTokens))
                                .arg(page.formatTokens(page.selectedProject.totalOutputTokens))
                              : qsTr("No token data yet")
                    accentColor: page.colors.information
                }
            }

            // ── Dashboard panels ─────────────────────────────────────────────
            GridLayout {
                Layout.fillWidth: true
                visible: page.liveProjectsModel.length > 0
                columns: page.logicalWidth >= 980 ? 2 : 1
                columnSpacing: page.hostWindow.dp(12)
                rowSpacing: page.hostWindow.dp(12)

                // Provider cost chart — empty array → shows "not available yet".
                OverviewProviderChart {
                    Layout.fillWidth: true
                    Layout.preferredWidth: page.hostWindow.dp(650)
                    hostWindow: page.hostWindow
                    colors: page.colors
                    providerModel: page.providerAnalyticsSnapshot
                    onProviderRequested: providerData => {
                        providerDetailsPopup.providerData = providerData
                        providerDetailsPopup.open()
                    }
                }

                OverviewAttentionPanel {
                    Layout.fillWidth: true
                    Layout.preferredWidth: page.hostWindow.dp(320)
                    hostWindow: page.hostWindow
                    colors: page.colors
                    projectName: page.selectedProject ? page.selectedProject.name : ""
                    attentionModel: page.selectedProject ? page.selectedProject.attention : []
                    onItemRequested: itemData => {
                        if (itemData.kind === "task") {
                            taskDetailsPopup.taskData = itemData.taskData
                            taskDetailsPopup.open()
                        }
                    }
                }

                OverviewProjectsPanel {
                    Layout.fillWidth: true
                    Layout.preferredWidth: page.hostWindow.dp(650)
                    hostWindow: page.hostWindow
                    colors: page.colors
                    projectsModel: page.liveProjectsModel
                    selectedIndex: page.selectedProjectIndex
                    onProjectSelected: index => page.selectProjectByIndex(index)
                }

                OverviewProjectStage {
                    Layout.fillWidth: true
                    Layout.preferredWidth: page.hostWindow.dp(320)
                    hostWindow: page.hostWindow
                    colors: page.colors
                    projectName:      page.selectedProject ? page.selectedProject.name           : ""
                    progress:         page.selectedProject ? page.selectedProject.progress        : 0.0
                    activeRunTitle:   page.selectedProject ? page.selectedProject.activeRunTitle  : ""
                    activeRunStatus:  page.selectedProject ? page.selectedProject.activeRunStatus : ""
                    runCount:         page.selectedProject ? page.selectedProject.runCount        : 0
                    completedCount:   page.selectedProject ? page.selectedProject.completedCount  : 0
                    failedCount:      page.selectedProject ? page.selectedProject.failedCount     : 0
                    cancelledCount:   page.selectedProject ? page.selectedProject.cancelledCount  : 0
                    queuedCount:      page.selectedProject ? page.selectedProject.queuedCount     : 0
                    activityModel:    page.selectedProject ? page.selectedProject.activity        : []
                }
            }
        }
    }

    OverviewProviderDetailsPopup {
        id: providerDetailsPopup
        hostWindow: page.hostWindow
        colors: page.colors
        x: Math.max(0, (page.width - width) / 2)
        y: Math.max(0, (page.height - height) / 2)
    }

    OverviewTaskDetailsPopup {
        id: taskDetailsPopup
        hostWindow: page.hostWindow
        colors: page.colors
        x: Math.max(0, (page.width - width) / 2)
        y: Math.max(0, (page.height - height) / 2)
    }
}
