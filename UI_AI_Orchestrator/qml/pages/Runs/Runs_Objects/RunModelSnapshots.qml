import QtQuick

// Read-only projections of the live adapter models. The Runs page owns
// selection, filters and commands; this component only mirrors role data.
Item {
    id: snapshots

    required property var backend
    readonly property string selectedRunId: snapshots.backend.runs.selectedRunId
    signal selectionReconciliationRequested()
    visible: false

    property var projectsSnapshot: []
    property var runsSnapshot: []
    property var stagesSnapshot: []
    property var artifactsSnapshot: []
    property var warningsSnapshot: []
    readonly property var agentChoicesSnapshot: snapshots.backend.runs.agentChoices
    property var tasksSnapshot: []
    property var draftsSnapshot: []
    property bool runsSnapshotRebuildScheduled: false

    function manualResolutionLabel(resolution) {
        if (resolution === "unresolved") return qsTranslate("RunsPage", "Unresolved")
        if (resolution === "verified_awaiting_apply") return qsTranslate("RunsPage", "Verified · awaiting Apply")
        if (resolution === "partially_applied") return qsTranslate("RunsPage", "Partially applied")
        if (resolution === "resolved_manually_applied") return qsTranslate("RunsPage", "Resolved manually · Applied")
        return ""
    }

    // AG-17.7C-3: the history marker of a Run that was created by a legacy roster conversion. The
    // Backend decides which Runs these are (RunListModel's legacyConversion role); this only words
    // the three values it can take - an ordinary Run has none.
    function conversionLabel(marker) {
        if (marker === "explicit_user") return qsTranslate("RunsPage", "Converted")
        if (marker === "not_required_safe_retry") return qsTranslate("RunsPage", "Converted · auto Retry")
        if (marker === "unreadable") return qsTranslate("RunsPage", "Conversion record unreadable")
        return ""
    }

    function statusLabel(status) {
        switch (status) {
        case "prepared": return qsTranslate("RunsPage", "Prepared")
        case "queued": return qsTranslate("RunsPage", "Queued")
        case "running": return qsTranslate("RunsPage", "Running")
        case "cancelling": return qsTranslate("RunsPage", "Cancelling")
        case "completed": return qsTranslate("RunsPage", "Completed")
        case "failed": return qsTranslate("RunsPage", "Failed")
        case "cancelled": return qsTranslate("RunsPage", "Cancelled")
        case "interrupted": return qsTranslate("RunsPage", "Interrupted")
        default: return status
        }
    }

    function formatDateTime(value) {
        return value ? Qt.formatDateTime(value, "yyyy-MM-dd HH:mm") : "—"
    }

    function formatDuration(start, end) {
        if (!start)
            return "—"
        const endTime = end ? end.getTime() : Date.now()
        let totalSeconds = Math.max(0, Math.round((endTime - start.getTime()) / 1000))
        const hours = Math.floor(totalSeconds / 3600)
        totalSeconds -= hours * 3600
        const minutes = Math.floor(totalSeconds / 60)
        const seconds = totalSeconds - minutes * 60
        const pad = n => String(n).padStart(2, "0")
        return hours > 0 ? (pad(hours) + ":" + pad(minutes) + ":" + pad(seconds))
                          : (pad(minutes) + ":" + pad(seconds))
    }

    function formatTokens(count) {
        if (count >= 1000000)
            return (count / 1000000).toFixed(1) + "M"
        if (count >= 1000)
            return (count / 1000).toFixed(1) + "K"
        return String(count)
    }

    function formatBytes(count) {
        // Artifact byteSize is exact Backend metadata; formatting is a visual
        // concern and therefore stays in QML without altering persisted data.
        if (count >= 1024 * 1024)
            return (count / (1024 * 1024)).toFixed(1) + " MB"
        if (count >= 1024)
            return (count / 1024).toFixed(1) + " KB"
        return String(count) + " B"
    }

    function projectNameFor(projectId) {
        for (let i = 0; i < projectsSnapshot.length; ++i) {
            if (projectsSnapshot[i].projectId === projectId)
                return projectsSnapshot[i].name
        }
        return projectId
    }

    function projectFor(projectId) {
        for (let i = 0; i < projectsSnapshot.length; ++i) {
            if (projectsSnapshot[i].projectId === projectId)
                return projectsSnapshot[i]
        }
        return null
    }

    function providerNameFor(providerId) {
        for (let i = 0; i < agentChoicesSnapshot.length; ++i) {
            if (agentChoicesSnapshot[i].providerId === providerId)
                return agentChoicesSnapshot[i].providerName
        }
        return providerId
    }

    function modelNameFor(modelId) {
        for (let i = 0; i < agentChoicesSnapshot.length; ++i) {
            if (agentChoicesSnapshot[i].modelId === modelId)
                return agentChoicesSnapshot[i].modelName
        }
        return modelId
    }

    // Derives a short human-readable activity log purely from the persisted
    // stage timeline and the run's own error message - there is no separate
    // log store in the Application Layer yet.
    function buildRunLogs(run, stages) {
        const lines = []
        for (let i = 0; i < stages.length; ++i) {
            const stage = stages[i]
            if (stage.state === "pending")
                continue
            let line = stage.name + ": " + snapshots.statusLabel(stage.state)
            if (stage.state === "failed" && stage.detail.length > 0)
                line += " (" + stage.detail + ")"
            lines.push(line)
        }
        if (run && run.errorText.length > 0)
            lines.push(qsTranslate("RunsPage", "Run error: %1").arg(run.errorText))
        return lines
    }

    function rebuildProjectsSnapshot() {
        const items = []
        for (let i = 0; i < projectsMirror.count; ++i) {
            const it = projectsMirror.itemAt(i)
            if (!it)
                continue
            items.push({
                projectId: it.projectId,
                name: it.name,
                description: it.description,
                rootPath: it.rootPath,
                status: it.status
            })
        }
        snapshots.projectsSnapshot = items
        snapshots.scheduleRunsSnapshotRebuild()
    }

    function rebuildTasksSnapshot() {
        const items = []
        for (let i = 0; i < tasksMirror.count; ++i) {
            const it = tasksMirror.itemAt(i)
            if (!it)
                continue
            items.push({
                taskId: it.taskId,
                projectId: it.projectId,
                title: it.title,
                description: it.description,
                status: it.status,
                priority: it.priority,
                requiresAttention: it.requiresAttention,
                dueAt: it.dueAt,
                plannedRunAt: it.plannedRunAt
            })
        }
        snapshots.tasksSnapshot = items
    }

    function rebuildDraftsSnapshot() {
        const items = []
        for (let i = 0; i < draftsMirror.count; ++i) {
            const it = draftsMirror.itemAt(i)
            if (!it)
                continue
            items.push({
                draftId: it.draftId,
                name: it.name,
                projectId: it.projectId,
                title: it.title,
                task: it.task,
                launchMode: it.launchMode,
                executionProfileId: it.executionProfileId,
                roster: it.roster,
                maxParallelism: it.maxParallelism,
                workspacePath: it.workspacePath,
                updatedAt: it.updatedAt,
                // PC-3D: where a plan draft came from (empty/-1/false for a manual draft).
                groupId: it.groupId,
                groupTitle: it.groupTitle,
                groupOrigin: it.groupOrigin,
                groupOrdinal: it.groupOrdinal,
                groupDraftCount: it.groupDraftCount,
                planKey: it.planKey,
                dependsOnTitles: it.dependsOnTitles,
                sourceRunCount: it.sourceRunCount,
                edited: it.edited
            })
        }
        snapshots.draftsSnapshot = items
    }

    function rebuildStagesSnapshot() {
        const items = []
        for (let i = 0; i < stagesMirror.count; ++i) {
            const it = stagesMirror.itemAt(i)
            if (!it)
                continue
            let detail = it.errorMessage.length > 0 ? it.errorMessage : it.description
            // Only fill in a fallback live-status caption when the backend has
            // no real description/error for this stage yet - never overrides
            // actual persisted text.
            if (detail.length === 0) {
                if (it.status === "pending")
                    detail = qsTranslate("RunsPage", "Waiting for next stage")
                else if (it.status === "running")
                    detail = qsTranslate("RunsPage", "Waiting for provider response")
            }
            items.push({
                name: it.title,
                state: it.status,
                detail: detail,
                duration: snapshots.formatDuration(it.startedAt, it.finishedAt)
            })
        }
        snapshots.stagesSnapshot = items
        snapshots.scheduleRunsSnapshotRebuild()
    }

    function rebuildArtifactsSnapshot() {
        const items = []
        for (let i = 0; i < artifactsMirror.count; ++i) {
            const it = artifactsMirror.itemAt(i)
            if (!it)
                continue
            items.push({
                artifactId: it.artifactId,
                runId: it.runId,
                stageId: it.stageId,
                name: it.name,
                type: it.kind,
                mediaType: it.mediaType,
                size: snapshots.formatBytes(Number(it.byteSize)),
                byteSize: it.byteSize,
                sha256: it.sha256,
                createdAt: it.createdAt
            })
        }
        snapshots.artifactsSnapshot = items
        snapshots.scheduleRunsSnapshotRebuild()
    }

    function rebuildWarningsSnapshot() {
        const items = []
        for (let i = 0; i < warningsMirror.count; ++i) {
            const it = warningsMirror.itemAt(i)
            if (!it)
                continue
            items.push({
                warningId: it.warningId,
                stageId: it.stageId,
                code: it.code,
                message: it.message,
                artifactName: it.artifactName,
                createdAt: it.createdAt
            })
        }
        snapshots.warningsSnapshot = items
        snapshots.scheduleRunsSnapshotRebuild()
    }

    function rebuildRunsSnapshot() {
        const items = []
        for (let i = 0; i < runsMirror.count; ++i) {
            const it = runsMirror.itemAt(i)
            if (!it)
                continue
            const isSelected = it.runId === snapshots.selectedRunId
            const stages = isSelected ? snapshots.stagesSnapshot : []
            const totalTokens = it.inputTokens + it.outputTokens
            const project = snapshots.projectFor(it.projectId)
            const effectiveWorkspacePath = it.workspacePath.length > 0
                                            ? it.workspacePath
                                            : (project ? project.rootPath : "")
            const roster = []
            for (let assignmentIndex = 0; assignmentIndex < it.assignments.length; ++assignmentIndex) {
                const assignment = it.assignments[assignmentIndex]
                roster.push({
                    stageType: assignment.stageType,
                    providerId: assignment.providerId,
                    modelId: assignment.modelId,
                    provider: snapshots.providerNameFor(assignment.providerId),
                    model: snapshots.modelNameFor(assignment.modelId),
                    agentName: assignment.agentName,
                    role: assignment.role,
                    instructions: assignment.instructions,
                    leadArchitect: assignment.leadArchitect,
                    dependencies: assignment.dependencies,
                    workspaceAccess: assignment.workspaceAccess,
                    maxAttempts: assignment.maxAttempts,
                    fallbackProviderId: assignment.fallbackProviderId,
                    fallbackModelId: assignment.fallbackModelId,
                    timeoutMs: Number(assignment.timeoutMs),
                    position: Number(assignment.position)
                })
            }
            items.push({
                runId: it.runId,
                projectId: it.projectId,
                title: it.title,
                task: it.task,
                workflowId: it.workflowId,
                maxParallelism: Number(it.maxParallelism),
                projectContext: project ? project.description : "",
                // A run that chose its own workspace keeps showing that exact
                // folder (reproducible, matches what the run actually used)
                // rather than the project's current rootPath, which may have
                // since changed.
                projectRoot: effectiveWorkspacePath,
                // Display-only workspace snapshot. Proposal Apply resolves the
                // trusted root from ChangeSet -> Run inside Backend.
                workspacePath: effectiveWorkspacePath,
                roster: roster,
                status: it.status,
                statusLabel: snapshots.statusLabel(it.status),
                manualHandoffResolution: it.manualHandoffResolution,
                manualHandoffResolutionLabel:
                    snapshots.manualResolutionLabel(it.manualHandoffResolution),
                legacyConversion: it.legacyConversion,
                legacyConversionLabel: snapshots.conversionLabel(it.legacyConversion),
                projectName: snapshots.projectNameFor(it.projectId),
                provider: it.primaryProviderId.length > 0
                          ? snapshots.providerNameFor(it.primaryProviderId)
                          : qsTranslate("RunsPage", "Local test"),
                model: it.primaryModelId.length > 0
                       ? snapshots.modelNameFor(it.primaryModelId)
                       : it.workflowId,
                progress: it.progress,
                updatedAt: snapshots.formatDateTime(it.finishedAt || it.startedAt || it.createdAt),
                // Raw timestamps, alongside the baked duration string below:
                // RunDetailsPanel.liveDuration() recomputes from these plus
                // its own ticking nowMs, instead of relying on this snapshot
                // being rebuilt every second just to keep one label current.
                // createdAt is also RunListPanel's date-grouping fallback
                // when a run has neither started nor finished yet.
                createdAt: it.createdAt,
                startedAt: it.startedAt,
                finishedAt: it.finishedAt,
                duration: snapshots.formatDuration(it.startedAt, it.finishedAt),
                tokens: snapshots.formatTokens(totalTokens),
                inputTokens: it.inputTokens,
                outputTokens: it.outputTokens,
                // Real-Run observation (2026-09-15): computed unconditionally
                // for every row, these 4 extra reads were enough to blow the
                // Mock workflow's deliberately narrow (~320ms) cancellation
                // race in AIOrchestrator_QmlFrontend_tests. Gated behind
                // isSelected like artifacts/logs below - Run Details is the
                // only consumer, so no row but the selected one needs them.
                cacheCreationInputTokens: isSelected ? Number(it.cacheCreationInputTokens || 0) : 0,
                cacheReadInputTokens: isSelected ? Number(it.cacheReadInputTokens || 0) : 0,
                // Package 4: the Backend-owned computeUsageTokenBreakdown()
                // projection - never re-derived here from inputTokens minus
                // the two cache roles above.
                nonCacheInputTokens: isSelected ? Number(it.nonCacheInputTokens || 0) : 0,
                providerBudgetInputTokens: isSelected ? Number(it.providerBudgetInputTokens) : 0,
                providerBudgetOutputTokens: isSelected ? Number(it.providerBudgetOutputTokens) : 0,
                providerBudgetMaxInputTokens: Number(it.providerBudgetMaxInputTokens),
                providerBudgetMaxOutputTokens: Number(it.providerBudgetMaxOutputTokens),
                providerBudgetMaxTurns: Number(it.providerBudgetMaxTurns),
                providerBudgetTurnsRaisedFrom: Number(it.providerBudgetTurnsRaisedFrom || 0),
                providerBudgetMaxToolCalls: Number(it.providerBudgetMaxToolCalls),
                providerBudgetMaxToolResultBytes: Number(it.providerBudgetMaxToolResultBytes),
                providerBudgetMaxWallTimeMs: Number(it.providerBudgetMaxWallTimeMs),
                providerBudgetMaxRequestTimeMs: Number(it.providerBudgetMaxRequestTimeMs),
                providerTurnsStarted: Number(it.providerTurnsStarted),
                providerTurnsCompleted: Number(it.providerTurnsCompleted),
                providerToolCalls: Number(it.providerToolCalls),
                providerToolResultBytes: Number(it.providerToolResultBytes),
                providerUsageAuthoritative: Boolean(it.providerUsageAuthoritative),
                providerBudgetStopCode: String(it.providerBudgetStopCode || ""),
                // PC-2: the Run's frozen interface contracts, read only for the selected row like the
                // other Run Details fields.
                interfaceContracts: isSelected ? (it.interfaceContracts || []) : [],
                interfaceContractsFingerprint: isSelected
                    ? String(it.interfaceContractsFingerprint || "") : "",
                errorText: it.errorMessage,
                stagesLabel: qsTranslate("RunsPage", "%1 / %2 stages").arg(stages.filter(function (s) { return s.state === "completed" }).length).arg(stages.length),
                stages: stages,
                artifacts: isSelected ? snapshots.artifactsSnapshot : [],
                warnings: isSelected ? snapshots.warningsSnapshot : [],
                logs: isSelected ? snapshots.buildRunLogs({ errorText: it.errorMessage }, stages) : []
            })
        }
        snapshots.runsSnapshot = items
        snapshots.selectionReconciliationRequested()
    }

    function scheduleRunsSnapshotRebuild() {
        if (snapshots.runsSnapshotRebuildScheduled)
            return
        snapshots.runsSnapshotRebuildScheduled = true
        Qt.callLater(function() {
            snapshots.runsSnapshotRebuildScheduled = false
            snapshots.rebuildRunsSnapshot()
        })
    }

    // Non-visual mirrors: they expose the C++ models' role data to JS. No
    // business state is invented here - every field is read from the adapter.
    Repeater {
        id: projectsMirror
        model: snapshots.backend.projects.model
        onCountChanged: Qt.callLater(snapshots.rebuildProjectsSnapshot)
        delegate: Item {
            required property string projectId
            required property string name
            required property string description
            required property string rootPath
            required property string status
            Component.onCompleted: Qt.callLater(snapshots.rebuildProjectsSnapshot)
        }
    }

    Repeater {
        id: runsMirror
        model: snapshots.backend.runs.model
        onCountChanged: snapshots.scheduleRunsSnapshotRebuild()
        delegate: Item {
            required property string runId
            required property string projectId
            required property string title
            required property string task
            required property string status
            required property string workflowId
            required property real progress
            required property string errorMessage
            required property var inputTokens
            required property var outputTokens
            required property var cacheCreationInputTokens
            required property var cacheReadInputTokens
            required property var createdAt
            required property var startedAt
            required property var finishedAt
            required property string primaryProviderId
            required property string primaryModelId
            required property var maxParallelism
            required property var assignments
            required property string workspacePath
            required property var providerBudgetMaxInputTokens
            required property var providerBudgetMaxOutputTokens
            required property var providerBudgetMaxTurns
            required property var providerBudgetTurnsRaisedFrom
            required property var providerBudgetMaxToolCalls
            required property var providerBudgetMaxToolResultBytes
            required property var providerBudgetMaxWallTimeMs
            required property var providerBudgetMaxRequestTimeMs
            required property var providerTurnsStarted
            required property var providerTurnsCompleted
            required property var providerToolCalls
            required property var providerToolResultBytes
            required property bool providerUsageAuthoritative
            required property string providerBudgetStopCode
            required property var providerBudgetInputTokens
            required property var providerBudgetOutputTokens
            required property string manualHandoffResolution
            required property string legacyConversion
            // PC-2: immutable after enqueue, so no onXxxChanged handler is needed.
            required property var interfaceContracts
            required property string interfaceContractsFingerprint
            Component.onCompleted: snapshots.scheduleRunsSnapshotRebuild()
            // RunListModel now updates an existing run's row in place
            // (dataChanged) instead of a full model reset, so these fields -
            // the ones that actually change after a run is created - need
            // their own handlers to keep runsSnapshot (what the UI actually
            // renders) in sync; a full reset used to make this automatic via
            // onCountChanged, but recreating every delegate on every event
            // was the direct cost that made the UI freeze during a run.
            onStatusChanged: snapshots.scheduleRunsSnapshotRebuild()
            onProgressChanged: snapshots.scheduleRunsSnapshotRebuild()
            onErrorMessageChanged: snapshots.scheduleRunsSnapshotRebuild()
            onInputTokensChanged: snapshots.scheduleRunsSnapshotRebuild()
            onOutputTokensChanged: snapshots.scheduleRunsSnapshotRebuild()
            // cacheCreationInputTokens/cacheReadInputTokens/
            // providerBudgetInputTokens/providerBudgetOutputTokens are
            // deliberately NOT wired to their own onXxxChanged handler here:
            // RunManager persists the whole providerBudgetUsage struct (and
            // run.usage alongside it) as one atomic update per turn, so
            // onProviderTurnsCompletedChanged below already fires - and
            // rebuildRunsSnapshot() re-reads every current property, these
            // included, regardless of which one's change triggered it. A
            // real-Run smoke-test regression (AIOrchestrator_QmlFrontend_tests'
            // deliberately narrow ~320ms Mock-workflow cancellation race,
            // 2026-09-15) proved 4 extra onChanged handlers alone - with no
            // other change - were enough added overhead to blow that window.
            onProviderTurnsStartedChanged: snapshots.scheduleRunsSnapshotRebuild()
            onProviderTurnsCompletedChanged: snapshots.scheduleRunsSnapshotRebuild()
            onProviderToolCallsChanged: snapshots.scheduleRunsSnapshotRebuild()
            onProviderToolResultBytesChanged: snapshots.scheduleRunsSnapshotRebuild()
            onProviderUsageAuthoritativeChanged: snapshots.scheduleRunsSnapshotRebuild()
            onProviderBudgetStopCodeChanged: snapshots.scheduleRunsSnapshotRebuild()
            onManualHandoffResolutionChanged: snapshots.scheduleRunsSnapshotRebuild()
            onLegacyConversionChanged: snapshots.scheduleRunsSnapshotRebuild()
            onStartedAtChanged: snapshots.scheduleRunsSnapshotRebuild()
            onFinishedAtChanged: snapshots.scheduleRunsSnapshotRebuild()
        }
    }

    Repeater {
        id: tasksMirror
        model: snapshots.backend.tasks.model
        onCountChanged: Qt.callLater(snapshots.rebuildTasksSnapshot)
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
            Component.onCompleted: Qt.callLater(snapshots.rebuildTasksSnapshot)
            onStatusChanged: Qt.callLater(snapshots.rebuildTasksSnapshot)
        }
    }

    Repeater {
        id: draftsMirror
        model: snapshots.backend.runDrafts.drafts
        onCountChanged: Qt.callLater(snapshots.rebuildDraftsSnapshot)
        delegate: Item {
            required property string draftId
            required property string name
            required property string projectId
            required property string title
            required property string task
            required property string launchMode
            required property string executionProfileId
            required property var roster
            required property var maxParallelism
            required property string workspacePath
            required property var updatedAt
            required property string groupId
            required property string groupTitle
            required property string groupOrigin
            required property int groupOrdinal
            required property int groupDraftCount
            required property string planKey
            required property var dependsOnTitles
            required property int sourceRunCount
            required property bool edited
            Component.onCompleted: Qt.callLater(snapshots.rebuildDraftsSnapshot)
        }
    }

    // RunController::selectRun already filters stageModel to the selected run,
    // so this mirror is always the timeline for backend.runs.selectedRunId.
    Repeater {
        id: stagesMirror
        model: snapshots.backend.runs.stageModel
        onCountChanged: Qt.callLater(snapshots.rebuildStagesSnapshot)
        delegate: Item {
            required property string title
            required property string description
            required property string status
            required property string errorMessage
            required property var startedAt
            required property var finishedAt
            Component.onCompleted: Qt.callLater(snapshots.rebuildStagesSnapshot)
        }
    }

    // ArtifactListModel is already scoped by RunController to selectedRunId.
    // Only safe metadata roles are mirrored; no filesystem path enters QML.
    Repeater {
        id: artifactsMirror
        model: snapshots.backend.runs.artifactModel
        onCountChanged: Qt.callLater(snapshots.rebuildArtifactsSnapshot)
        delegate: Item {
            required property string artifactId
            required property string runId
            required property var stageId
            required property string kind
            required property string name
            required property string mediaType
            required property var byteSize
            required property string sha256
            required property var createdAt
            Component.onCompleted: Qt.callLater(snapshots.rebuildArtifactsSnapshot)
        }
    }

    // RunWarningListModel is scoped to selectedRunId the same way; the warnings are durable (read from the database).
    Repeater {
        id: warningsMirror
        model: snapshots.backend.runs.warningModel
        onCountChanged: Qt.callLater(snapshots.rebuildWarningsSnapshot)
        delegate: Item {
            required property string warningId
            required property string runId
            required property var stageId
            required property string code
            required property string message
            required property string artifactName
            required property var createdAt
            Component.onCompleted: Qt.callLater(snapshots.rebuildWarningsSnapshot)
        }
    }
}
