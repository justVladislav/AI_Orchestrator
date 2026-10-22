pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import QtCore

Dialog {
    id: dialog
    objectName: "newRunDialog"

    required property var hostWindow
    required property var colors
    required property var backend
    // Bound directly to backend.projects.model so the project picker always
    // reflects the live Qt Adapter list without a separate JS mirror.
    required property var projectsModel
    // Enabled-only projection from ExecutionProfileController.
    required property var executionProfilesModel
    property string defaultExecutionProfileId: ""
    property int defaultTimeoutMs: 300000
    property int defaultMaxParallelAgents: 4
    // Continue on Projects persists this stable ID. The dialog only uses it
    // as a convenience default; the user can still choose another project.
    property string preferredProjectId: ""
    // Set by the page after a failed backend call; kept visible until the
    // next attempt so the dialog does not close on a rejected command.
    property string errorText: ""
    // Expand the Task editor to dp(220) height for long-form context.
    property bool taskExpanded: false
    property var agentChoices: []
    readonly property int agentCount: agentsModel.count
    property bool agentChoicesLoading: false
    property var fallbackChoices: []
    property var taskChoices: []
    property string preferredTaskId: ""
    // Set by RunsPage from RunController.rerunDraft(). This is a Backend-owned
    // command snapshot, not a reconstruction from display-model fields.
    property var prefillData: null
    property bool invalidPrefillTarget: false
    // PC-3D: a per-Run roster draft that has no roster yet (a draft made from a next-run plan) opens in roster mode
    // with unselected model slots instead of switching to Mock. While any slot is unselected the Run cannot start.
    property bool rosterNeedsModels: false
    property bool rosterModelsMissing: false
    // The Backend's description of a plan draft (planGroup of RunDraftController.openDraft), or null.
    readonly property var planGroup: dialog.prefillData && dialog.prefillData.planGroup
                                     ? dialog.prefillData.planGroup : null
    readonly property string planProvenanceText: {
        const group = dialog.planGroup
        if (!group)
            return ""
        let text = qsTr("From the plan “%1” (%2), draft %3.")
                   .arg(group.planTitle)
                   .arg(group.origin === "chat_message" ? qsTr("chat reply") : qsTr("pasted"))
                   .arg(group.ordinal + 1)
        if (group.dependsOnTitles.length > 0)
            text += " " + qsTr("Suggested to start after: %1 (advisory, nothing is enforced).")
                    .arg(group.dependsOnTitles.join(", "))
        if (group.edited)
            text += " " + qsTr("Edited since it was created.")
        return text
    }
    // Plain JS mirror of backend.projects.model (adds rootPath, which the
    // ComboBox's textRole/valueRole pair can't surface) — used only to show
    // the resolved workspace folder for the selected project.
    property var projectsSnapshot: []
    // Empty keeps "the current" workspace, i.e. the selected project's own
    // rootPath; non-empty is a per-run override picked via Browse….
    property string workspaceOverridePath: ""

    // AG-17.6: a per-Run roster can execute through the converged Manifest workflow:
    // the lead plans a frozen task graph and the AVAILABLE workers execute it. The
    // roster is a pool, not mandatory occupancy - a worker may get no task. Only the
    // Backend decides what a roster means; this flag just selects the workflow.
    readonly property string manifestWorkflowId: "execution-manifest-v1"
    readonly property bool manifestRoster: productionCheck.checked
                                           && customRosterCheck.checked
                                           && manifestRosterCheck.checked

    // AG-11 New Run indication: plain-JS mirror of executionProfilesModel's
    // profileId/assignments roles, used only to look up a saved profile's
    // model ids for the memory-budget preview below (the ComboBox itself
    // only needs name/profileId via textRole/valueRole).
    property var profilesSnapshot: []
    // {available, totalSegmentCount, includedSegmentCount, estimatedTokens,
    // budgetTokens} from ProjectMemoryController.memoryBudgetEstimate() -
    // never computed locally, so this always matches the real Run's own
    // assembleRunContext() selection.
    property var memoryBudgetSnapshot: ({
        available: false, totalSegmentCount: 0, includedSegmentCount: 0,
        estimatedTokens: 0, budgetTokens: 0
    })

    function currentModelIdsForBudget() {
        const ids = []
        if (!productionCheck.checked)
            return ids
        if (customRosterCheck.checked) {
            for (let index = 0; index < agentsModel.count; ++index) {
                const row = agentsModel.get(index)
                const choice = dialog.agentChoices[row.modelChoiceIndex]
                if (choice && choice.modelId)
                    ids.push(choice.modelId)
                if (!dialog.manifestRoster && !row.workspaceAccess && row.fallbackChoiceIndex >= 0) {
                    const fallback = dialog.agentChoices[row.fallbackChoiceIndex]
                    if (fallback && fallback.modelId)
                        ids.push(fallback.modelId)
                }
            }
            return ids
        }
        const profileId = String(profileCombo.currentValue || "")
        for (let index = 0; index < dialog.profilesSnapshot.length; ++index) {
            const profile = dialog.profilesSnapshot[index]
            if (profile.profileId !== profileId)
                continue
            for (let a = 0; a < profile.assignments.length; ++a) {
                const assignment = profile.assignments[a]
                if (assignment.modelId)
                    ids.push(assignment.modelId)
                if (assignment.fallbackModelId)
                    ids.push(assignment.fallbackModelId)
            }
            break
        }
        return ids
    }

    function refreshMemoryBudget() {
        const projectId = String(projectCombo.currentValue || "")
        if (projectId.length === 0) {
            dialog.memoryBudgetSnapshot = {
                available: false, totalSegmentCount: 0, includedSegmentCount: 0,
                estimatedTokens: 0, budgetTokens: 0
            }
            return
        }
        dialog.memoryBudgetSnapshot = dialog.backend.projectMemory.memoryBudgetEstimate(
            projectId, dialog.currentModelIdsForBudget())
    }

    function projectRootPathFor(projectId) {
        if (projectId.length === 0)
            return ""
        for (let index = 0; index < dialog.projectsSnapshot.length; ++index) {
            if (dialog.projectsSnapshot[index].projectId === projectId)
                return dialog.projectsSnapshot[index].rootPath || ""
        }
        return ""
    }

    signal runAccepted(string projectId, string title, string task,
                       string executionProfileId, string projectTaskId,
                       string workspacePath, string sourceRunId,
                       bool allowEvidenceBackedNoChanges,
                       bool automaticVerificationRepair)
    signal rosterRunAccepted(string projectId, string title, string task,
                              var roster, int maxParallelism, string projectTaskId,
                              string workspacePath, string sourceRunId,
                              bool allowEvidenceBackedNoChanges,
                              bool automaticVerificationRepair, string workflowId,
                              var providerBudget, var interfaceContracts)
    // AG-17.7C-2: the Backend offers to re-launch this legacy roster Run on the Manifest workflow
    // (rerunDraft().conversion). Choosing it leaves this dialog for the conversion dialog, where the
    // Backend's preview - what is not carried over - is shown and confirmed. This dialog only
    // forwards the source Run id; it never builds or edits the converted roster.
    readonly property bool conversionOffered: dialog.prefillData !== null
                                              && dialog.prefillData !== undefined
                                              && !Boolean(dialog.prefillData.isDraft)
                                              && dialog.prefillData.conversion !== undefined
                                              && Boolean(dialog.prefillData.conversion.available)
                                              && !Boolean(dialog.prefillData.conversion.required)
    signal conversionRequested(string sourceRunId)
    // Saves the current in-progress configuration without starting a run.
    // The page owns the actual backend.runDrafts.createDraft() call, mirroring
    // how runAccepted/rosterRunAccepted are handled, so this dialog never
    // talks to the backend adapter directly for lifecycle state.
    signal saveDraftRequested(string projectId, string title, string task,
                              string launchMode, string executionProfileId, var roster,
                              int maxParallelism, string workspacePath,
                              bool automaticVerificationRepair, string workflowId,
                              var providerBudget, var interfaceContracts)
    // PC-3D: the same fields, written back into the draft this dialog was opened from.
    signal updateDraftRequested(string draftId, string projectId, string title, string task,
                                string launchMode, string executionProfileId, var roster,
                                int maxParallelism, string workspacePath,
                                bool automaticVerificationRepair, string workflowId,
                                var providerBudget, var interfaceContracts)

    ListModel { id: agentsModel; objectName: "rosterModel" }
    Settings {
        id: agentContextSuggestionStore
        category: "NewRunAgentContextSuggestions"
        property string history: "[]"
    }

    // Any roster mutation (model/fallback choice, workspace access, add/
    // remove a row) goes through ListModel, which always emits one of these
    // - covers every roster edit path without duplicating a refresh call at
    // each individual control.
    Connections {
        target: agentsModel
        function onDataChanged() {
            dialog.refreshMemoryBudget()
            dialog.refreshRoleWarning()
            dialog.refreshRosterModelsMissing()
        }
        function onRowsInserted() {
            dialog.refreshMemoryBudget()
            dialog.refreshRoleWarning()
            dialog.refreshRosterModelsMissing()
        }
        function onRowsRemoved() {
            dialog.refreshMemoryBudget()
            dialog.refreshRoleWarning()
            dialog.refreshRosterModelsMissing()
        }
    }

    // PC-3D: true while a draft that came without a roster still has a model slot nobody has chosen. The Run can
    // not start (hasInvalidExecutionTarget) and the roster says why.
    function refreshRosterModelsMissing() {
        let missing = false
        if (dialog.rosterNeedsModels) {
            for (let index = 0; index < agentsModel.count; ++index) {
                if (!dialog.agentChoices[agentsModel.get(index).modelChoiceIndex]) {
                    missing = true
                    break
                }
            }
        }
        dialog.rosterModelsMissing = missing
    }

    function anyModelChosen() {
        for (let index = 0; index < agentsModel.count; ++index) {
            if (dialog.agentChoices[agentsModel.get(index).modelChoiceIndex])
                return true
        }
        return false
    }
    onManifestRosterChanged: dialog.refreshRoleWarning()

    // A Manifest lead assigns every task by the worker's role (the role becomes the member id it is
    // shown), so two workers with one role can be told apart only by their model - exactly what went
    // wrong when every worker kept the default role. A warning, not an error: interchangeable
    // workers with one role are legitimate. Empty outside a Manifest roster.
    property string duplicateRoleWarning: ""
    function refreshRoleWarning() {
        if (!dialog.manifestRoster) {
            dialog.duplicateRoleWarning = ""
            return
        }
        const seen = {}
        const repeated = []
        for (let index = 1; index < agentsModel.count; ++index) {
            const role = String(agentsModel.get(index).role || "").trim()
            const key = role.toLowerCase()
            if (key.length === 0)
                continue
            if (seen[key] === true && repeated.indexOf(role) < 0)
                repeated.push(role)
            seen[key] = true
        }
        dialog.duplicateRoleWarning = repeated.length === 0 ? "" : qsTr(
            "Workers share a role (%1): the lead can tell them apart only by their model. Give every worker its own role.")
            .arg(repeated.join(", "))
    }

    // The second roster field is "Instructions". A Manifest lead has none: the Planner prompt never
    // carries a lead's instructions (only the workers' reach the lead and their own sessions), so
    // offering the field would silently discard what is typed. Every other row keeps it.
    function rosterInstructionsApplicable(index) {
        return !(dialog.manifestRoster && index === 0)
    }

    Repeater {
        id: profilesMirror
        model: dialog.executionProfilesModel
        onCountChanged: dialog.rebuildProfilesSnapshot()
        delegate: Item {
            id: profileMirrorDelegate
            required property string profileId
            required property var assignments
            Component.onCompleted: dialog.rebuildProfilesSnapshot()
        }
    }

    function rebuildProfilesSnapshot() {
        const items = []
        for (let i = 0; i < profilesMirror.count; ++i) {
            const it = profilesMirror.itemAt(i)
            if (!it) continue
            items.push({ profileId: it.profileId, assignments: it.assignments })
        }
        dialog.profilesSnapshot = items
        dialog.refreshMemoryBudget()
    }

    onAgentChoicesChanged: {
        dialog.fallbackChoices = [{
            "providerId": "", "modelId": "", "label": qsTr("No fallback")
        }].concat(dialog.agentChoices)
    }

    function resizeRoster(count) {
        while (agentsModel.count < count) {
            const index = agentsModel.count
            agentsModel.append({
                "agentName": index === 0 ? qsTr("Lead Architect") : qsTr("Agent %1").arg(index + 1),
                "role": index === 0 ? qsTr("Software Architect") : qsTr("Implementation specialist"),
                "instructions": "",
                "modelChoiceIndex": bulkModelCombo.currentIndex >= 0 ? bulkModelCombo.currentIndex : 0,
                "maxAttempts": 2,
                "fallbackChoiceIndex": -1,
                "fallbackMissing": false,
                "workspaceAccess": false,
                "stageType": index === 0 ? "architect" : "agent-" + (index + 1),
                "position": index,
                "leadArchitect": index === 0,
                "dependencies": index === 0 ? [] : ["architect"],
                "timeoutMs": dialog.defaultTimeoutMs
            })
        }
        while (agentsModel.count > count)
            agentsModel.remove(agentsModel.count - 1)
    }

    // The stable id the Planner is shown for a Manifest worker (and the Backend freezes
    // as its memberId): a readable ASCII slug of its role, unique within the roster,
    // never the reserved lead id.
    function manifestMemberId(role, index, used) {
        let base = String(role || "").toLowerCase()
                       .replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "")
        if (base.length === 0 || base === "architect")
            base = "worker-" + (index + 1)
        base = base.substring(0, 40).replace(/-+$/g, "")
        let candidate = base
        let counter = 2
        while (used[candidate] || candidate === "architect") {
            candidate = base + "-" + counter
            ++counter
        }
        used[candidate] = true
        return candidate
    }

    // The workflow a roster launch selects: the Manifest workflow for a Manifest roster,
    // empty (the legacy fixed-stage runner) for every other launch.
    function workflowIdForLaunch() {
        return dialog.manifestRoster ? dialog.manifestWorkflowId : ""
    }

    // Explicit provider limits exist only for a per-Run roster; a saved profile carries its own.
    readonly property bool providerLimitsApplicable: productionCheck.checked
                                                     && customRosterCheck.checked

    // The optional-key map the Backend resolves and validates: empty = the Backend defaults with an
    // automatic turn limit; present keys are explicit limits, and a present maxTurns pins the turn
    // limit. Always empty where limits do not apply, so a hidden section can never leak a value.
    function providerBudgetPayload() {
        if (!dialog.providerLimitsApplicable || !customProviderLimitsCheck.checked)
            return ({})
        const budget = {
            maxInputTokens: Number(limitInputTokens.value),
            maxOutputTokens: Number(limitOutputTokens.value),
            maxToolCalls: Number(limitToolCalls.value),
            maxToolResultBytes: Number(limitToolResultMiB.value) * 1048576,
            maxWallTimeMs: Number(limitWallMinutes.value) * 60000,
            maxRequestTimeMs: Number(limitRequestSeconds.value) * 1000
        }
        if (fixedTurnLimitCheck.checked)
            budget.maxTurns = Number(limitTurns.value)
        return budget
    }

    // The inverse, for Edit & rerun and a reopened draft: the map the Backend produced (only the
    // limits somebody set). An empty map switches the section off, so a Run on the Backend
    // defaults reopens as exactly that rather than as typed values.
    function applyProviderBudget(budget) {
        const value = budget || ({})
        customProviderLimitsCheck.checked = Object.keys(value).length > 0
        limitInputTokens.value = Number(value.maxInputTokens || 500000)
        limitOutputTokens.value = Number(value.maxOutputTokens || 64000)
        limitToolCalls.value = Number(value.maxToolCalls || 128)
        limitToolResultMiB.value = Math.max(1, Math.round(
            Number(value.maxToolResultBytes || 8388608) / 1048576))
        limitWallMinutes.value = Math.max(1, Math.round(
            Number(value.maxWallTimeMs || 900000) / 60000))
        limitRequestSeconds.value = Math.max(1, Math.round(
            Number(value.maxRequestTimeMs || 240000) / 1000))
        fixedTurnLimitCheck.checked = value.maxTurns !== undefined && value.maxTurns !== null
        limitTurns.value = Number(value.maxTurns || 24)
    }

    // PC-2: interface contracts exist only for a per-Run Manifest roster (the Manifest workflow is the one
    // that has worker prompts to carry them). Always empty elsewhere, so a hidden editor can never leak a
    // contract, and the Backend refuses contracts on any other workflow rather than dropping them.
    function interfaceContractsPayload() {
        if (!dialog.manifestRoster)
            return []
        return contractsEditor.payload()
    }

    // The inverse, for Edit & rerun and a reopened draft: the list the Backend froze or saved.
    function applyInterfaceContracts(contracts) {
        contractsEditor.apply(contracts || [])
    }

    function rosterPayload() {
        const result = []
        const usedMemberIds = {}
        for (let index = 0; index < agentsModel.count; ++index) {
            const row = agentsModel.get(index)
            const choice = dialog.agentChoices[row.modelChoiceIndex]
            if (!choice)
                return []
            if (dialog.manifestRoster) {
                // Row 0 plans (the lead); every other row is an available worker that
                // always has workspace access, one attempt and no provider fallback.
                const lead = index === 0
                result.push({
                    "stageType": lead ? "architect"
                                      : dialog.manifestMemberId(row.role, index, usedMemberIds),
                    "agentName": row.agentName,
                    "role": row.role,
                    // A Manifest lead has no instructions field (see
                    // rosterInstructionsApplicable); a value left in the row by an earlier
                    // mode or a reopened Run must not leak into the payload.
                    "instructions": dialog.rosterInstructionsApplicable(index) ? row.instructions : "",
                    "providerId": choice.providerId,
                    "modelId": choice.modelId,
                    "timeoutMs": row.timeoutMs,
                    "position": index,
                    "leadArchitect": lead,
                    "dependencies": lead ? [] : ["architect"],
                    "workspaceAccess": !lead,
                    "maxAttempts": 1,
                    "fallbackProviderId": "",
                    "fallbackModelId": ""
                })
                continue
            }
            // A workspace-access agent runs through the shared tool-use turn
            // loop (one primary attempt plus one narrow format-repair retry),
            // not the general attempt/fallback loop - the backend rejects the
            // combination outright (run.workspace_access_no_retry), so this
            // payload never carries attempts/fallback for such a row
            // regardless of what those (disabled) controls still hold.
            const fallback = !row.workspaceAccess && row.fallbackChoiceIndex >= 0
                    ? dialog.agentChoices[row.fallbackChoiceIndex] : null
            result.push({
                "stageType": row.stageType,
                "agentName": row.agentName,
                "role": row.role,
                "instructions": row.instructions,
                "providerId": choice.providerId,
                "modelId": choice.modelId,
                "timeoutMs": row.timeoutMs,
                "position": row.position,
                "leadArchitect": row.leadArchitect,
                "dependencies": row.dependencies,
                "workspaceAccess": row.workspaceAccess,
                "maxAttempts": row.workspaceAccess ? 1 : row.maxAttempts,
                "fallbackProviderId": fallback ? fallback.providerId : "",
                "fallbackModelId": fallback ? fallback.modelId : ""
            })
        }
        return result
    }

    // Mirrors the exact mode decision submit() makes when choosing between
    // runAccepted/rosterRunAccepted, so a saved draft reopens into the same
    // toggle state (see prefillRosterFrom()/onOpened's launchMode branch).
    function currentLaunchMode() {
        if (productionCheck.checked && customRosterCheck.checked)
            return "custom_roster"
        if (productionCheck.checked)
            return "saved_execution_profile"
        return "mock"
    }

    // The fields a saved draft carries, or null (errorText set) when the dialog cannot be saved yet.
    function draftFields() {
        if (projectCombo.currentIndex < 0 || titleField.text.trim().length === 0) {
            dialog.errorText = qsTr("Select a project and enter a title before saving a draft.")
            return null
        }
        const launchMode = dialog.currentLaunchMode()
        let roster = []
        if (launchMode === "custom_roster") {
            roster = dialog.rosterPayload()
            if (roster.length !== agentsModel.count) {
                // A draft that came without a roster (a next-run plan draft) may be saved as it is until the
                // models are chosen; a half-chosen roster may not.
                if (dialog.rosterNeedsModels && !dialog.anyModelChosen()) {
                    roster = []
                } else {
                    dialog.errorText = qsTr("Select a valid provider/model for every agent before saving.")
                    return null
                }
            }
        }
        return {
            "projectId": String(projectCombo.currentValue),
            "title": titleField.text.trim(),
            "task": taskField.text.trim(),
            "launchMode": launchMode,
            "executionProfileId": launchMode === "saved_execution_profile"
                                  ? String(profileCombo.currentValue) : "",
            "roster": roster,
            "maxParallelism": maxParallelismSpin.value,
            "workspacePath": dialog.workspaceOverridePath,
            "automaticVerificationRepair": automaticVerificationRepairCheck.checked,
            "workflowId": launchMode === "custom_roster" ? dialog.workflowIdForLaunch() : "",
            "providerBudget": launchMode === "custom_roster" ? dialog.providerBudgetPayload() : ({}),
            "interfaceContracts": launchMode === "custom_roster" ? dialog.interfaceContractsPayload() : []
        }
    }

    function saveDraft() {
        const fields = dialog.draftFields()
        if (fields === null)
            return
        dialog.saveDraftRequested(fields.projectId, fields.title, fields.task, fields.launchMode,
                                   fields.executionProfileId, fields.roster, fields.maxParallelism,
                                   fields.workspacePath, fields.automaticVerificationRepair,
                                   fields.workflowId, fields.providerBudget, fields.interfaceContracts)
    }

    // Writes the current fields back into the draft this dialog was opened from.
    function updateDraft() {
        if (!dialog.prefillData || !dialog.prefillData.isDraft)
            return
        const fields = dialog.draftFields()
        if (fields === null)
            return
        dialog.updateDraftRequested(String(dialog.prefillData.draftId), fields.projectId, fields.title,
                                     fields.task, fields.launchMode, fields.executionProfileId, fields.roster,
                                     fields.maxParallelism, fields.workspacePath,
                                     fields.automaticVerificationRepair, fields.workflowId,
                                     fields.providerBudget, fields.interfaceContracts)
    }

    function hasInvalidExecutionTarget() {
        if (!productionCheck.checked)
            return false
        if (!customRosterCheck.checked)
            return profileCombo.currentIndex < 0
        for (let index = 0; index < agentsModel.count; ++index) {
            const row = agentsModel.get(index)
            if (!dialog.agentChoices[row.modelChoiceIndex])
                return true
            if (row.fallbackMissing)
                return true
        }
        return false
    }

    function projectTaskOptions() {
        const options = [{ "label": qsTr("Custom objective (no linked task)"), "value": "" }]
        const projectId = String(projectCombo.currentValue || "")
        for (let index = 0; index < dialog.taskChoices.length; ++index) {
            const task = dialog.taskChoices[index]
            if (task.projectId === projectId
                    && task.status !== "done" && task.status !== "cancelled") {
                options.push({
                    "label": task.title,
                    "value": task.taskId,
                    "description": task.description,
                    "status": task.status
                })
            }
        }
        return options
    }

    function applySelectedTask() {
        const taskId = String(taskCombo.currentValue || "")
        if (taskId.length === 0)
            return
        for (let index = 0; index < dialog.taskChoices.length; ++index) {
            const task = dialog.taskChoices[index]
            if (task.taskId !== taskId)
                continue
            titleField.text = task.title
            taskField.text = task.description.length > 0 ? task.description : task.title
            return
        }
    }

    function selectPreferredTask() {
        // A planned task carries its own project. Align the project picker
        // first, otherwise projectTaskOptions() filters the task out and the
        // dialog silently skips inheriting its title and description.
        if (dialog.preferredTaskId.length > 0) {
            for (let index = 0; index < dialog.taskChoices.length; ++index) {
                const task = dialog.taskChoices[index]
                if (task.taskId !== dialog.preferredTaskId)
                    continue
                const projectIndex = projectCombo.indexOfValue(task.projectId)
                if (projectIndex >= 0)
                    projectCombo.currentIndex = projectIndex
                break
            }
        }
        taskCombo.model = dialog.projectTaskOptions()
        const index = dialog.preferredTaskId.length > 0
                ? taskCombo.indexOfValue(dialog.preferredTaskId) : -1
        taskCombo.currentIndex = index >= 0 ? index : 0
        if (index >= 0)
            dialog.applySelectedTask()
    }

    function utf8ByteLength(text) {
        // TextEncoder is a browser Web API and is not available in Qt QML.
        // Count UTF-8 bytes explicitly while retaining the full user input.
        let bytes = 0
        for (let index = 0; index < text.length; ++index) {
            const codeUnit = text.charCodeAt(index)
            if (codeUnit < 0x80) {
                bytes += 1
            } else if (codeUnit < 0x800) {
                bytes += 2
            } else if (codeUnit >= 0xD800 && codeUnit <= 0xDBFF
                       && index + 1 < text.length) {
                const low = text.charCodeAt(index + 1)
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    bytes += 4
                    ++index
                } else {
                    bytes += 3
                }
            } else {
                bytes += 3
            }
        }
        return bytes
    }

    function submit() {
        // Tests and the visible button share this path, including validation,
        // trimming, project selection, and the unchanged public signal.
        dialog.invalidPrefillTarget = dialog.hasInvalidExecutionTarget()
        if (dialog.invalidPrefillTarget) {
            dialog.errorText = dialog.rosterModelsMissing
                    ? qsTr("Choose a model for the lead and for every worker before starting this Run.")
                    : qsTr("The original provider, model, or execution profile is no longer available. Select a valid replacement.")
            return
        }
        const sourceRunId = dialog.prefillData
                ? String(dialog.prefillData.sourceRunId || "") : ""
        if (projectCombo.currentIndex < 0
                || titleField.text.trim().length === 0
                || taskField.text.trim().length === 0
                || (productionCheck.checked && !customRosterCheck.checked
                    && profileCombo.currentIndex < 0))
            return
        if (productionCheck.checked && customRosterCheck.checked) {
            const roster = dialog.rosterPayload()
            if (roster.length !== agentsModel.count)
                return
            if (dialog.manifestRoster && roster.length < 2) {
                dialog.errorText = qsTr("A Manifest roster needs a lead and at least one available worker.")
                return
            }
            dialog.rosterRunAccepted(String(projectCombo.currentValue),
                                     titleField.text.trim(), taskField.text.trim(), roster,
                                     maxParallelismSpin.value,
                                     String(taskCombo.currentValue || ""),
                                     dialog.workspaceOverridePath,
                                     sourceRunId,
                                     productionCheck.checked
                                     && allowNoChangesCheck.checked,
                                     automaticVerificationRepairCheck.checked,
                                     dialog.workflowIdForLaunch(),
                                     dialog.providerBudgetPayload(),
                                     dialog.interfaceContractsPayload())
            return
        }
        dialog.runAccepted(String(projectCombo.currentValue),
                           titleField.text.trim(),
                           taskField.text.trim(),
                           productionCheck.checked
                           ? String(profileCombo.currentValue) : "",
                           String(taskCombo.currentValue || ""),
                           dialog.workspaceOverridePath,
                           sourceRunId,
                           productionCheck.checked
                           && allowNoChangesCheck.checked,
                           automaticVerificationRepairCheck.checked)
    }

    function selectPreferredProject() {
        const preferredIndex = dialog.preferredProjectId.length > 0
                ? projectCombo.indexOfValue(dialog.preferredProjectId)
                : -1
        projectCombo.currentIndex = preferredIndex >= 0
                ? preferredIndex
                : (projectCombo.count > 0 ? 0 : -1)
    }

    anchors.centerIn: parent
    // The dialog is centred on its parent (the page area beside the navigation), so its size
    // is bounded by that area, not by the whole window.
    readonly property real areaWidth: parent ? parent.width : hostWindow.width
    readonly property real areaHeight: parent ? parent.height : hostWindow.height
    width: Math.min(hostWindow.dp(1040),
                    Math.max(hostWindow.dp(360),
                             dialog.areaWidth - hostWindow.dp(32)))
    height: Math.min(hostWindow.dp(760),
                     dialog.areaHeight - hostWindow.dp(32))
    // Wide: two columns (what to run | how to run it) with the prompt filling the free height.
    // Narrow: one column in the same order, scrolling.
    readonly property bool wideLayout: dialog.width >= hostWindow.dp(860)
    modal: true
    // Only the Close button and Cancel leave the dialog. A click outside it (for example the click
    // that brings the application window back to the front) must never throw away a half-written
    // run, and neither must a stray Escape.
    closePolicy: Popup.NoAutoClose
    title: dialog.prefillData
           ? (dialog.prefillData.isDraft ? qsTr("Continue draft") : qsTr("Edit & rerun"))
           : qsTr("New run")
    standardButtons: Dialog.NoButton
    font.pixelSize: hostWindow.dp(13)

    function prefillRosterFrom(prefill) {
        const roster = prefill.roster || []
        agentsModel.clear()
        const manifest = String(prefill.workflowId || "") === dialog.manifestWorkflowId
        manifestRosterCheck.checked = manifest
        if (roster.length === 0) {
            // A per-Run roster draft with no roster yet (a draft made from a next-run plan). It stays what it is:
            // real providers, a per-Run roster, the Manifest workflow it was saved with - so its contracts stay
            // visible and are not dropped by a switch to Mock. The model slots start UNSELECTED: the Run cannot
            // start until the user has chosen a model for the lead and every worker.
            productionCheck.checked = true
            customRosterCheck.checked = true
            const slots = manifest ? 2 : 1
            agentCountSpin.value = slots
            dialog.resizeRoster(slots)
            for (let slot = 0; slot < agentsModel.count; ++slot)
                agentsModel.setProperty(slot, "modelChoiceIndex", -1)
            maxParallelismSpin.value = Math.min(prefill.maxParallelism || dialog.defaultMaxParallelAgents, slots)
            dialog.rosterNeedsModels = true
            dialog.refreshRosterModelsMissing()
            return
        }
        productionCheck.checked = true
        customRosterCheck.checked = true
        for (let index = 0; index < roster.length; ++index) {
            const entry = roster[index]
            let choiceIndex = -1
            for (let c = 0; c < dialog.agentChoices.length; ++c) {
                if (dialog.agentChoices[c].providerId === entry.providerId
                        && dialog.agentChoices[c].modelId === entry.modelId) {
                    choiceIndex = c
                    break
                }
            }
            let fallbackChoiceIndex = -1
            for (let fallbackIndex = 0; fallbackIndex < dialog.agentChoices.length; ++fallbackIndex) {
                if (dialog.agentChoices[fallbackIndex].providerId === entry.fallbackProviderId
                        && dialog.agentChoices[fallbackIndex].modelId === entry.fallbackModelId) {
                    fallbackChoiceIndex = fallbackIndex
                    break
                }
            }
            if (choiceIndex < 0
                    || ((entry.fallbackProviderId || entry.fallbackModelId)
                        && fallbackChoiceIndex < 0))
                dialog.invalidPrefillTarget = true
            agentsModel.append({
                "agentName": entry.agentName,
                "role": entry.role,
                "instructions": entry.instructions,
                "modelChoiceIndex": choiceIndex,
                "maxAttempts": entry.maxAttempts === undefined ? 1 : entry.maxAttempts,
                "fallbackChoiceIndex": fallbackChoiceIndex,
                "fallbackMissing": (entry.fallbackProviderId || entry.fallbackModelId)
                                   && fallbackChoiceIndex < 0,
                "workspaceAccess": entry.workspaceAccess || false,
                "stageType": entry.stageType,
                "position": entry.position,
                "leadArchitect": entry.leadArchitect,
                "dependencies": entry.dependencies || [],
                "timeoutMs": entry.timeoutMs || dialog.defaultTimeoutMs
            })
        }
        agentCountSpin.value = agentsModel.count
        maxParallelismSpin.value = Math.min(
                    prefill.maxParallelism || dialog.defaultMaxParallelAgents, agentsModel.count)
    }

    onOpened: {
        dialog.errorText = ""
        dialog.invalidPrefillTarget = false
        dialog.rosterNeedsModels = false
        dialog.rosterModelsMissing = false
        dialog.taskExpanded = false
        dialog.fallbackChoices = [{
            "providerId": "", "modelId": "", "label": qsTr("No fallback")
        }].concat(dialog.agentChoices)

        if (dialog.prefillData) {
            const prefill = dialog.prefillData
            allowNoChangesCheck.checked = Boolean(
                        prefill.allowEvidenceBackedNoChanges || false)
            automaticVerificationRepairCheck.checked =
                    prefill.automaticVerificationRepair === undefined
                    ? true : Boolean(prefill.automaticVerificationRepair)
            titleField.text = prefill.title
            taskField.text = prefill.task
            const projectIndex = projectCombo.indexOfValue(prefill.projectId)
            projectCombo.currentIndex = projectIndex >= 0
                    ? projectIndex : (projectCombo.count > 0 ? 0 : -1)
            taskCombo.model = dialog.projectTaskOptions()
            taskCombo.currentIndex = 0
            const projectRoot = dialog.projectRootPathFor(prefill.projectId)
            dialog.workspaceOverridePath = (prefill.workspacePath && prefill.workspacePath !== projectRoot)
                    ? prefill.workspacePath : ""
            // Restore the execution snapshot's concurrency for every
            // production launch mode. Custom-roster prefill will clamp it to
            // the restored roster size after rebuilding the rows.
            maxParallelismSpin.value = prefill.maxParallelism
                    || dialog.defaultMaxParallelAgents
            if (prefill.launchMode === "saved_execution_profile") {
                productionCheck.checked = true
                customRosterCheck.checked = false
                manifestRosterCheck.checked = false
                agentsModel.clear()
                const profileIndex = profileCombo.indexOfValue(prefill.executionProfileId)
                profileCombo.currentIndex = profileIndex
                if (profileIndex < 0)
                    dialog.invalidPrefillTarget = true
            } else if (prefill.launchMode === "custom_roster") {
                dialog.prefillRosterFrom(prefill)
            } else {
                productionCheck.checked = false
                customRosterCheck.checked = false
                manifestRosterCheck.checked = false
                agentsModel.clear()
            }
            // Only a per-Run roster carries explicit limits; the Backend sent exactly the ones set.
            dialog.applyProviderBudget(prefill.launchMode === "custom_roster"
                                       ? prefill.providerBudget : ({}))
            dialog.applyInterfaceContracts(prefill.launchMode === "custom_roster"
                                           ? prefill.interfaceContracts : [])
            if (dialog.invalidPrefillTarget)
                dialog.errorText = qsTr("The original provider, model, or execution profile is no longer available. Select a valid replacement.")
            titleField.forceActiveFocus()
            Qt.callLater(dialog.refreshMemoryBudget)
            return
        }

        titleField.clear()
        taskField.clear()
        allowNoChangesCheck.checked = false
        automaticVerificationRepairCheck.checked = true
        dialog.applyProviderBudget({})
        dialog.applyInterfaceContracts([])
        dialog.workspaceOverridePath = ""
        dialog.selectPreferredProject()
        Qt.callLater(dialog.selectPreferredTask)
        const defaultIndex = dialog.defaultExecutionProfileId.length > 0
                ? profileCombo.indexOfValue(dialog.defaultExecutionProfileId) : -1
        profileCombo.currentIndex = defaultIndex >= 0
                ? defaultIndex : (profileCombo.count > 0 ? 0 : -1)
        titleField.forceActiveFocus()
        // Prefer a real provider run whenever the catalog can form either a
        // per-run roster or a saved execution profile. Mock remains an
        // explicit local test mode instead of the easy-to-miss default.
        productionCheck.checked = dialog.agentChoices.length > 0
                                  || profileCombo.count > 0
        customRosterCheck.checked = dialog.agentChoices.length > 0
        manifestRosterCheck.checked = false
        agentsModel.clear()
        agentCountSpin.value = 1
        dialog.resizeRoster(1)
        Qt.callLater(dialog.refreshMemoryBudget)
    }

    onClosed: dialog.prefillData = null

    onPreferredProjectIdChanged: {
        if (visible)
            dialog.selectPreferredProject()
    }

    onPreferredTaskIdChanged: {
        if (visible)
            Qt.callLater(dialog.selectPreferredTask)
    }

    background: Rectangle {
        radius: dialog.colors.cornerRadiusMedium
        color: dialog.colors.popupBackground
        border.width: 1
        border.color: dialog.colors.border
    }

    header: Rectangle {
        implicitHeight: dialog.hostWindow.dp(50)
        color: dialog.colors.surfaceAlternative

        Text {
            anchors.left: parent.left
            anchors.right: closeButton.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: dialog.hostWindow.dp(18)
            anchors.rightMargin: dialog.hostWindow.dp(8)
            text: dialog.title
            color: dialog.colors.textPrimary
            elide: Text.ElideRight
            font.pixelSize: dialog.hostWindow.dp(15)
            font.weight: Font.DemiBold
        }

        // The dialog never closes on a click outside it (closePolicy below), so leaving is always
        // an explicit act: this button or Cancel.
        Basic.Button {
            id: closeButton
            objectName: "newRunCloseButton"
            anchors.right: parent.right
            anchors.rightMargin: dialog.hostWindow.dp(10)
            anchors.verticalCenter: parent.verticalCenter
            width: dialog.hostWindow.dp(32)
            height: dialog.hostWindow.dp(32)
            text: qsTr("Close")
            Accessible.name: qsTr("Close without starting a run")
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Close without starting a run (the entered values are discarded)")
            onClicked: dialog.reject()
            contentItem: Text {
                text: "\u2715"
                color: closeButton.hovered ? dialog.colors.textPrimary : dialog.colors.textSecondary
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.pixelSize: dialog.hostWindow.dp(14)
            }
            background: Rectangle {
                radius: dialog.colors.cornerRadiusSmall
                color: closeButton.down ? dialog.colors.buttonPressed
                       : closeButton.hovered ? dialog.colors.buttonHover : "transparent"
            }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: dialog.colors.separator
        }
    }

    contentItem: ScrollView {
        id: formScroll
        contentWidth: availableWidth
        contentHeight: formGrid.height
        clip: true
        rightPadding: dialog.hostWindow.dp(12)
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

        // Wide: the prompt (title + task) owns the left column and fills its height; project,
        // workspace, linked task and the execution settings stack on the right. Narrow: one
        // column - project, prompt, execution.
        GridLayout {
            id: formGrid
            width: formScroll.availableWidth
            height: Math.max(implicitHeight, formScroll.availableHeight)
            columns: dialog.wideLayout ? 2 : 1
            columnSpacing: dialog.hostWindow.dp(18)
            rowSpacing: dialog.hostWindow.dp(13)

        ColumnLayout {
            id: contextSection
            objectName: "newRunContextSection"
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            // Project, workspace and linked task: one row across the dialog when wide.
            Layout.row: 0
            Layout.column: 0
            Layout.columnSpan: dialog.wideLayout ? 2 : 1
            spacing: dialog.hostWindow.dp(13)

        Text {
            objectName: "newRunPlanProvenance"
            Layout.fillWidth: true
            visible: dialog.planProvenanceText.length > 0
            text: dialog.planProvenanceText
            color: dialog.colors.accent
            wrapMode: Text.WordWrap
            font.pixelSize: dialog.hostWindow.dp(11)
        }

        GridLayout {
            Layout.fillWidth: true
            columns: dialog.wideLayout ? 3 : 1
            columnSpacing: dialog.hostWindow.dp(14)
            rowSpacing: dialog.hostWindow.dp(13)

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            Layout.preferredWidth: 1
            spacing: dialog.hostWindow.dp(6)

        Text {
            text: qsTr("Project")
            color: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(13)
            font.weight: Font.Medium
        }

        ComboBox {
            id: projectCombo
            objectName: "runProjectCombo"
            Layout.fillWidth: true
            Layout.preferredHeight: dialog.hostWindow.dp(38)
            // textRole/valueRole read straight from ProjectListModel's roles
            // (name/projectId) — no JS projection needed for this simple picker.
            model: dialog.projectsModel
            textRole: "name"
            valueRole: "projectId"
            font.pixelSize: dialog.hostWindow.dp(13)
            onActivated: {
                dialog.preferredTaskId = ""
                taskCombo.model = dialog.projectTaskOptions()
                taskCombo.currentIndex = 0
                // A different Project also means a different "current"
                // workspace; drop any override chosen for the previous
                // Project instead of silently carrying it over.
                dialog.workspaceOverridePath = ""
                dialog.refreshMemoryBudget()
            }
        }

        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            Layout.preferredWidth: 1
            spacing: dialog.hostWindow.dp(6)

        Text {
            text: qsTr("Workspace")
            color: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(13)
            font.weight: Font.Medium
        }

        // Lets the user keep the Project's own folder ("current") or pick a
        // different one for this run only; the choice is a per-run override,
        // never a change to the Project itself.
        RowLayout {
            Layout.fillWidth: true
            spacing: dialog.hostWindow.dp(6)

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: dialog.hostWindow.dp(34)
                radius: dialog.colors.cornerRadiusSmall
                color: dialog.colors.inputBackground
                border.width: 1
                border.color: dialog.colors.inputBorder

                Text {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: dialog.hostWindow.dp(10)
                    anchors.rightMargin: dialog.hostWindow.dp(10)
                    text: dialog.workspaceOverridePath.length > 0
                          ? dialog.workspaceOverridePath
                          : dialog.projectRootPathFor(String(projectCombo.currentValue || ""))
                    color: dialog.workspaceOverridePath.length > 0
                           ? dialog.colors.textPrimary : dialog.colors.textSecondary
                    elide: Text.ElideMiddle
                    font.pixelSize: dialog.hostWindow.dp(10)
                }
            }

            RunActionButton {
                text: qsTr("Browse…")
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                onClicked: workspaceFolderDialog.open()
            }

            RunActionButton {
                visible: dialog.workspaceOverridePath.length > 0
                text: qsTr("Use project path")
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                onClicked: dialog.workspaceOverridePath = ""
            }
        }

        FolderDialog {
            id: workspaceFolderDialog
            title: qsTr("Select workspace folder for this run")

            onAccepted: {
                // selectedFolder is a file:// URL; the backend expects a
                // plain local path.
                let path = workspaceFolderDialog.selectedFolder.toString()
                if (path.startsWith("file://"))
                    path = path.substring(7)
                // A Windows path yields "file:///C:/Users/..." -> after
                // stripping "file://" that leaves "/C:/Users/...": strip
                // that extra leading slash only when a drive letter follows.
                // A real POSIX path (e.g. "/Users/...") must keep its own
                // leading slash.
                if (/^\/[A-Za-z]:\//.test(path))
                    path = path.substring(1)
                dialog.workspaceOverridePath = decodeURIComponent(path)
            }
        }

        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            Layout.preferredWidth: 1
            spacing: dialog.hostWindow.dp(6)

        Text {
            text: qsTr("Project task")
            color: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(13)
            font.weight: Font.Medium
        }

        ComboBox {
            id: taskCombo
            objectName: "runTaskCombo"
            Layout.fillWidth: true
            Layout.preferredHeight: dialog.hostWindow.dp(38)
            textRole: "label"
            valueRole: "value"
            font.pixelSize: dialog.hostWindow.dp(12)
            onActivated: dialog.applySelectedTask()
        }

        }

        }

        }

        ColumnLayout {
            id: executionSection
            objectName: "newRunExecutionSection"
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            Layout.preferredWidth: dialog.wideLayout ? formGrid.width * 0.58 : formGrid.width
            Layout.row: dialog.wideLayout ? 1 : 2
            Layout.column: dialog.wideLayout ? 1 : 0
            spacing: dialog.hostWindow.dp(8)

        Text {
            Layout.fillWidth: true
            text: qsTr("Execution")
            color: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(13)
            font.weight: Font.Medium
        }

        Text {
            Layout.fillWidth: true
            text: productionCheck.checked
                  ? (customRosterCheck.checked
                     ? qsTr("Configure a lead Architect and up to 19 specialist agents. Their roles and provider/model assignments are snapshotted for reproducible retry.")
                     : qsTr("Start a production workflow with a saved execution profile. Provider/model assignments are snapshotted for reproducible retry."))
                  : qsTr("Start a deterministic Mock workflow. No API keys are required.")
            color: dialog.colors.textSecondary
            wrapMode: Text.WordWrap
            font.pixelSize: dialog.hostWindow.dp(11)
        }

            ColumnLayout {
            Layout.fillWidth: true
            spacing: dialog.hostWindow.dp(6)

            RowLayout {
            Layout.fillWidth: true
            spacing: dialog.hostWindow.dp(10)

            CheckBox {
                id: productionCheck
                objectName: "useExecutionProfileCheck"
                text: qsTr("Run with real providers")
                enabled: profileCombo.count > 0 || dialog.agentChoices.length > 0
                font.pixelSize: dialog.hostWindow.dp(12)
                onToggled: dialog.refreshMemoryBudget()
            }

            BusyIndicator {
                visible: dialog.agentChoicesLoading
                running: visible
                Layout.preferredWidth: dialog.hostWindow.dp(22)
                Layout.preferredHeight: dialog.hostWindow.dp(22)
            }

            Text {
                visible: dialog.agentChoicesLoading
                text: qsTr("Loading providers…")
                color: dialog.colors.textMuted
                font.pixelSize: dialog.hostWindow.dp(10)
            }

            Item { Layout.fillWidth: true }
            }

            // The saved-profile choice gets its own row so it is never squeezed by the toggle above.
            RowLayout {
            Layout.fillWidth: true
            spacing: dialog.hostWindow.dp(10)

            CheckBox {
                id: customRosterCheck
                objectName: "customRosterCheck"
                text: qsTr("Configure agents for this run")
                visible: productionCheck.checked
                enabled: dialog.agentChoices.length > 0
                checked: true
                font.pixelSize: dialog.hostWindow.dp(11)
                onToggled: dialog.refreshMemoryBudget()
            }

            ComboBox {
                id: profileCombo
                objectName: "executionProfileCombo"
                Layout.fillWidth: true
                Layout.preferredHeight: dialog.hostWindow.dp(38)
                visible: productionCheck.checked && !customRosterCheck.checked
                enabled: visible
                model: dialog.executionProfilesModel
                textRole: "name"
                valueRole: "profileId"
                font.pixelSize: dialog.hostWindow.dp(12)
                onActivated: dialog.refreshMemoryBudget()
            }

            Text {
                visible: productionCheck.checked
                         && !customRosterCheck.checked
                         && profileCombo.count === 0
                Layout.fillWidth: true
                text: qsTr("Create an enabled profile in Providers and models first")
                color: dialog.colors.warning
                wrapMode: Text.WordWrap
                font.pixelSize: dialog.hostWindow.dp(9)
            }
        }
        }

        CheckBox {
            id: allowNoChangesCheck
            objectName: "allowEvidenceBackedNoChangesCheck"
            Layout.fillWidth: true
            text: qsTr("Audit run: allow evidence-backed no changes")
            visible: productionCheck.checked
            checked: false
            font.pixelSize: dialog.hostWindow.dp(10)
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Leave off for implementation runs. When enabled, planning may finish without creating Manifest tasks or change proposals if same-session inspection evidence shows the task is already satisfied.")
        }

        CheckBox {
            id: automaticVerificationRepairCheck
            objectName: "automaticVerificationRepairCheck"
            Layout.fillWidth: true
            text: qsTr("Automatically repair failed verification once")
            visible: productionCheck.checked
            checked: true
            font.pixelSize: dialog.hostWindow.dp(10)
            ToolTip.visible: hovered
            ToolTip.text: qsTr("When disabled, a failed combined verification ends the Run immediately without the additional paid repair wave. Structured findings remain available for continuation or manual handoff.")
        }

        // A roster agent can be given read-only workspace tools +
        // propose_change_set individually (see the per-agent "Workspace
        // access" toggle below) - the model inspects the project and
        // produces a proposal.v1 change proposal that is only ever staged
        // for review, never applied to the workspace by this run itself.
        ColumnLayout {
            Layout.fillWidth: true
            visible: productionCheck.checked && customRosterCheck.checked
            spacing: dialog.hostWindow.dp(8)

            CheckBox {
                id: manifestRosterCheck
                objectName: "manifestRosterCheck"
                Layout.fillWidth: true
                text: qsTr("Manifest: the lead plans, available workers execute")
                checked: false
                font.pixelSize: dialog.hostWindow.dp(11)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("The lead inspects the workspace and freezes a task plan; each task is assigned to one of the available workers. Several tasks may go to one worker and a worker may get none - an unused worker is normal, not an error.")
                onToggled: {
                    if (checked) {
                        dialog.resizeRoster(Math.max(2, agentsModel.count))
                        agentCountSpin.value = agentsModel.count
                    }
                    dialog.refreshMemoryBudget()
                }
            }

            Text {
                objectName: "newRunRosterSetupHint"
                Layout.fillWidth: true
                visible: dialog.rosterModelsMissing
                text: qsTr("This draft has no roster yet. Choose a model for the lead and for every worker below; the Run cannot start until you do. A plan never chooses models or cost.")
                color: dialog.colors.warning
                wrapMode: Text.WordWrap
                font.pixelSize: dialog.hostWindow.dp(10)
            }

            Text {
                objectName: "manifestRosterHint"
                Layout.fillWidth: true
                visible: manifestRosterCheck.checked
                text: qsTr("Row 1 is the lead (plans only). The other rows are the available workers. Role: a short name for each worker, for example recent-alerts. The lead assigns tasks by role, so give every worker its own role and use the same names in the task text. Instructions (workers only, optional): what the worker is good at - the lead reads them when assigning, and the worker receives them as its own instructions. Workers always get workspace access, one attempt and no provider fallback.")
                color: dialog.colors.textMuted
                wrapMode: Text.WordWrap
                font.pixelSize: dialog.hostWindow.dp(9)
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: dialog.hostWindow.dp(8)
                Text {
                    // A fixed width: the label changes with the mode, and it must not move the
                    // controls next to it.
                    Layout.preferredWidth: dialog.hostWindow.dp(104)
                    text: dialog.manifestRoster ? qsTr("Lead + workers") : qsTr("Agents")
                    color: dialog.colors.textPrimary
                    elide: Text.ElideRight
                    font.pixelSize: dialog.hostWindow.dp(12)
                }
                SpinBox {
                    id: agentCountSpin
                    objectName: "agentCountSpin"
                    from: dialog.manifestRoster ? 2 : 1
                    to: 20
                    value: 1
                    editable: true
                    onValueModified: dialog.resizeRoster(value)
                }
                Text {
                    text: qsTr("Parallel")
                    color: dialog.colors.textSecondary
                    font.pixelSize: dialog.hostWindow.dp(10)
                }
                SpinBox {
                    id: maxParallelismSpin
                    from: 1
                    to: 20
                    value: Math.min(dialog.defaultMaxParallelAgents,
                                    agentCountSpin.value)
                    editable: true
                }
                Item { Layout.fillWidth: true }
            }

            // Its own row: next to the count and parallelism it did not fit, and the
            // "Apply to all" button was cut off.
            RowLayout {
                Layout.fillWidth: true
                spacing: dialog.hostWindow.dp(8)
                visible: agentCountSpin.value > 1
                ComboBox {
                    id: bulkModelCombo
                    // A single agent already has its own model picker in the
                    // roster row below; a second, duplicate control here is
                    // just noise, so it only appears once there's more than
                    // one agent to bulk-assign.
                    visible: agentCountSpin.value > 1
                    Layout.fillWidth: true
                    Layout.minimumWidth: dialog.hostWindow.dp(120)
                    model: dialog.agentChoices
                    textRole: "label"
                    font.pixelSize: dialog.hostWindow.dp(10)
                }
                RunActionButton {
                    visible: agentCountSpin.value > 1
                    hostWindow: dialog.hostWindow
                    colors: dialog.colors
                    text: qsTr("Apply to all")
                    enabled: bulkModelCombo.currentIndex >= 0
                    onClicked: {
                        for (let index = 0; index < agentsModel.count; ++index)
                            agentsModel.setProperty(index, "modelChoiceIndex", bulkModelCombo.currentIndex)
                    }
                }
            }

            ScrollView {
                id: rosterScroll
                Layout.fillWidth: true
                // Wide: every row is shown and the dialog's own scroll area scrolls (no nested
                // scrollbars). Narrow: a bounded list that scrolls on its own.
                Layout.preferredHeight: dialog.wideLayout
                                        ? rosterColumn.implicitHeight + dialog.hostWindow.dp(4)
                                        : dialog.hostWindow.dp(190)
                contentWidth: availableWidth
                clip: true
                rightPadding: dialog.hostWindow.dp(12)
                ColumnLayout {
                    id: rosterColumn
                    width: rosterScroll.availableWidth
                    spacing: dialog.hostWindow.dp(6)
                    Repeater {
                        model: agentsModel
                        delegate: ColumnLayout {
                            id: agentDelegate
                            required property int index
                            required property string role
                            required property string instructions
                            required property int modelChoiceIndex
                            required property int maxAttempts
                            required property int fallbackChoiceIndex
                            required property bool fallbackMissing
                            required property bool workspaceAccess
                            Layout.fillWidth: true
                            spacing: dialog.hostWindow.dp(4)
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: dialog.hostWindow.dp(6)
                                Text {
                                    text: dialog.manifestRoster
                                          ? (agentDelegate.index === 0 ? qsTr("Lead") : qsTr("Worker %1").arg(agentDelegate.index))
                                          : (agentDelegate.index === 0 ? qsTr("Architect") : qsTr("Agent %1").arg(agentDelegate.index + 1))
                                    color: agentDelegate.index === 0 ? dialog.colors.accent : dialog.colors.textSecondary
                                    font.pixelSize: dialog.hostWindow.dp(10)
                                    Layout.preferredWidth: dialog.hostWindow.dp(70)
                                }
                                TextField {
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: dialog.hostWindow.dp(120)
                                    text: agentDelegate.role
                                    placeholderText: dialog.manifestRoster && agentDelegate.index > 0
                                                     ? qsTr("Role, e.g. recent-alerts")
                                                     : qsTr("Role")
                                    color: dialog.colors.textPrimary
                                    font.pixelSize: dialog.hostWindow.dp(10)
                                    onEditingFinished: agentsModel.setProperty(agentDelegate.index, "role", text.trim())
                                }
                                ComboBox {
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: dialog.hostWindow.dp(210)
                                    Layout.minimumWidth: dialog.hostWindow.dp(120)
                                    model: dialog.agentChoices
                                    textRole: "label"
                                    currentIndex: agentDelegate.modelChoiceIndex
                                    font.pixelSize: dialog.hostWindow.dp(9)
                                    onActivated: selectedIndex => agentsModel.setProperty(agentDelegate.index, "modelChoiceIndex", selectedIndex)
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: dialog.hostWindow.dp(6)
                                // A Manifest lead only plans, so it has no workspace toggle; a
                                // Manifest worker always has workspace access (required to be
                                // assignable), shown checked and fixed.
                                visible: !(dialog.manifestRoster && agentDelegate.index === 0)
                                Item { Layout.preferredWidth: dialog.hostWindow.dp(70) }
                                CheckBox {
                                    id: workspaceAccessCheck
                                    objectName: "workspaceAccessCheck"
                                    text: dialog.manifestRoster
                                          ? qsTr("Workspace access (required for workers)")
                                          : qsTr("Workspace access")
                                    enabled: !dialog.manifestRoster
                                    checked: dialog.manifestRoster || agentDelegate.workspaceAccess
                                    font.pixelSize: dialog.hostWindow.dp(9)
                                    // Read-only workspace tools + propose_change_set for
                                    // this agent, instead of a single-shot completion -
                                    // does not support retry attempts or a fallback
                                    // model (see the Attempts/Fallback row below, which
                                    // this disables - kept in its own row rather than
                                    // that one, since a control cannot sit inside the
                                    // row it disables).
                                    onToggled: agentsModel.setProperty(agentDelegate.index, "workspaceAccess", checked)
                                }
                                Item { Layout.fillWidth: true }
                            }
                            SuggestionTextField {
                                Layout.fillWidth: true
                                visible: dialog.rosterInstructionsApplicable(agentDelegate.index)
                                hostWindow: dialog.hostWindow
                                colors: dialog.colors
                                historyStore: agentContextSuggestionStore
                                text: agentDelegate.instructions
                                placeholderText: dialog.manifestRoster
                                                 ? qsTr("Instructions for this worker: the lead reads them when assigning, the worker receives them (optional)")
                                                 : (agentDelegate.index === 0
                                                    ? qsTr("Architect context or orchestration constraints (optional)")
                                                    : qsTr("Agent-specific task context (optional)"))
                                color: dialog.colors.textPrimary
                                font.pixelSize: dialog.hostWindow.dp(9)
                                onCommitted: value => agentsModel.setProperty(agentDelegate.index, "instructions", value)
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: dialog.hostWindow.dp(6)
                                // A workspace-access agent does not support
                                // retry attempts or a fallback model (see
                                // rosterPayload()'s run.workspace_access_no_retry
                                // note) - visibly disabled rather than hidden so
                                // it stays clear these values are ignored, not lost.
                                // A Manifest roster never uses attempts or a fallback (one attempt
                                // per task, no provider fallback), so the row is not offered at all.
                                visible: !dialog.manifestRoster
                                enabled: !agentDelegate.workspaceAccess
                                opacity: agentDelegate.workspaceAccess ? 0.5 : 1.0
                                Item { Layout.preferredWidth: dialog.hostWindow.dp(70) }
                                Text {
                                    text: qsTr("Attempts")
                                    color: dialog.colors.textSecondary
                                    font.pixelSize: dialog.hostWindow.dp(9)
                                }
                                SpinBox {
                                    from: 1
                                    to: 5
                                    value: agentDelegate.maxAttempts
                                    editable: true
                                    onValueModified: agentsModel.setProperty(agentDelegate.index, "maxAttempts", value)
                                }
                                Text {
                                    text: qsTr("Fallback")
                                    color: dialog.colors.textSecondary
                                    font.pixelSize: dialog.hostWindow.dp(9)
                                }
                                ComboBox {
                                    Layout.fillWidth: true
                                    model: dialog.fallbackChoices
                                    textRole: "label"
                                    currentIndex: agentDelegate.fallbackChoiceIndex + 1
                                    font.pixelSize: dialog.hostWindow.dp(9)
                                    onActivated: selectedIndex => {
                                        agentsModel.setProperty(
                                            agentDelegate.index, "fallbackChoiceIndex", selectedIndex - 1)
                                        agentsModel.setProperty(
                                            agentDelegate.index, "fallbackMissing", false)
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Text {
                objectName: "manifestRoleWarning"
                Layout.fillWidth: true
                visible: dialog.duplicateRoleWarning.length > 0
                text: dialog.duplicateRoleWarning
                color: dialog.colors.warning
                wrapMode: Text.WordWrap
                font.pixelSize: dialog.hostWindow.dp(9)
            }
        }

        // Provider limits of a per-Run roster (a saved profile carries its own). Off = the Backend
        // defaults, with the turn limit automatic; on = the values below are sent, and a fixed
        // turn limit pins it. QML only collects numbers: the Backend validates every value and
        // enforces the ceilings.
        ColumnLayout {
            objectName: "providerLimitsSection"
            Layout.fillWidth: true
            visible: dialog.providerLimitsApplicable
            spacing: dialog.hostWindow.dp(6)

            CheckBox {
                id: customProviderLimitsCheck
                objectName: "customProviderLimitsCheck"
                Layout.fillWidth: true
                text: qsTr("Set provider limits for this run")
                checked: false
                font.pixelSize: dialog.hostWindow.dp(11)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Off: the Backend defaults apply and the turn limit is raised automatically to what the Manifest needs (never above 64). On: the values below are used for this run.")
            }

            GridLayout {
                objectName: "providerLimitsGrid"
                Layout.fillWidth: true
                visible: customProviderLimitsCheck.checked
                columns: 4
                columnSpacing: dialog.hostWindow.dp(8)
                rowSpacing: dialog.hostWindow.dp(6)

                Text { text: qsTr("Input tokens"); color: dialog.colors.textMuted; font.pixelSize: dialog.hostWindow.dp(9) }
                SpinBox { id: limitInputTokens; objectName: "limitInputTokens"; Layout.fillWidth: true; from: 1; to: 1000000; value: 500000; editable: true }
                Text { text: qsTr("Output tokens"); color: dialog.colors.textMuted; font.pixelSize: dialog.hostWindow.dp(9) }
                SpinBox { id: limitOutputTokens; objectName: "limitOutputTokens"; Layout.fillWidth: true; from: 1; to: 128000; value: 64000; editable: true }

                Text { text: qsTr("Tool calls"); color: dialog.colors.textMuted; font.pixelSize: dialog.hostWindow.dp(9) }
                SpinBox { id: limitToolCalls; objectName: "limitToolCalls"; Layout.fillWidth: true; from: 1; to: 256; value: 128; editable: true }
                Text { text: qsTr("Tool output, MiB"); color: dialog.colors.textMuted; font.pixelSize: dialog.hostWindow.dp(9) }
                SpinBox { id: limitToolResultMiB; objectName: "limitToolResultMiB"; Layout.fillWidth: true; from: 1; to: 16; value: 8; editable: true }

                Text { text: qsTr("Run time, min"); color: dialog.colors.textMuted; font.pixelSize: dialog.hostWindow.dp(9) }
                SpinBox { id: limitWallMinutes; objectName: "limitWallMinutes"; Layout.fillWidth: true; from: 1; to: 30; value: 15; editable: true }
                Text { text: qsTr("Request timeout, s"); color: dialog.colors.textMuted; font.pixelSize: dialog.hostWindow.dp(9) }
                SpinBox { id: limitRequestSeconds; objectName: "limitRequestSeconds"; Layout.fillWidth: true; from: 1; to: 300; value: 240; editable: true }

                CheckBox {
                    id: fixedTurnLimitCheck
                    objectName: "fixedTurnLimitCheck"
                    text: qsTr("Fixed turn limit")
                    checked: false
                    font.pixelSize: dialog.hostWindow.dp(10)
                    ToolTip.visible: hovered
                    // The twice-reserved floor is a Manifest rule only; the legacy fixed-stage roster
                    // keeps the plain explanation.
                    ToolTip.text: dialog.manifestRoster
                                  ? qsTr("Unchecked: the turn limit stays automatic. Checked: this exact number of provider turns is the limit, and the run fails if the plan needs more. Every task reserves its floor twice (first attempt and a protected retry), so a fixed limit has to hold both: roughly the Planner's turns plus twice the sum of the tasks' required reads and terminal turns.")
                                  : qsTr("Unchecked: the turn limit stays automatic. Checked: this exact number of provider turns is the limit, and the run fails if the plan needs more.")
                }
                SpinBox { id: limitTurns; objectName: "limitTurns"; Layout.fillWidth: true; enabled: fixedTurnLimitCheck.checked; from: 1; to: 64; value: 24; editable: true }
                Item { Layout.fillWidth: true }
                Item { Layout.fillWidth: true }
            }
        }

        // PC-2: the Run author's interface contracts; Manifest roster only (see interfaceContractsPayload()).
        InterfaceContractsEditor {
            id: contractsEditor
            Layout.fillWidth: true
            visible: dialog.manifestRoster
            hostWindow: dialog.hostWindow
            colors: dialog.colors
        }

        // AG-11 New Run indication: shown only for real-provider runs
        // (Mock never assembles project memory into its context at all) and
        // only once ProjectMemoryController has actually answered - never a
        // locally invented estimate, and never shown as a false zero while
        // the answer is still unknown.
        RowLayout {
            Layout.fillWidth: true
            visible: productionCheck.checked
            spacing: dialog.hostWindow.dp(6)

            Text {
                objectName: "memoryBudgetIndicator"
                Layout.fillWidth: true
                text: !dialog.memoryBudgetSnapshot.available
                      ? qsTr("Project memory: unknown (select a project)")
                      : dialog.memoryBudgetSnapshot.totalSegmentCount === 0
                        ? qsTr("Project memory: no segments yet for this project")
                        : qsTr("Project memory: %1 of %2 segment(s) selected · ~%3 / %4 tok")
                          .arg(dialog.memoryBudgetSnapshot.includedSegmentCount)
                          .arg(dialog.memoryBudgetSnapshot.totalSegmentCount)
                          .arg(dialog.memoryBudgetSnapshot.estimatedTokens)
                          .arg(dialog.memoryBudgetSnapshot.budgetTokens)
                color: dialog.colors.textMuted
                wrapMode: Text.WordWrap
                font.pixelSize: dialog.hostWindow.dp(9.5)
            }
        }

        }

        ColumnLayout {
            id: promptSection
            objectName: "newRunPromptSection"
            Layout.fillWidth: true
            Layout.fillHeight: dialog.wideLayout
            Layout.alignment: Qt.AlignTop
            Layout.preferredWidth: dialog.wideLayout ? formGrid.width * 0.42 : formGrid.width
            // Wide: the left column below the context row, so the prompt gets all the free height.
            Layout.row: 1
            Layout.column: 0
            spacing: dialog.hostWindow.dp(13)

        Text {
            text: qsTr("Title")
            color: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(13)
            font.weight: Font.Medium
        }

        TextField {
            id: titleField
            objectName: "runTitleField"
            Layout.fillWidth: true
            Layout.preferredHeight: dialog.hostWindow.dp(38)
            placeholderText: qsTr("Implement orchestration dashboard")
            color: dialog.colors.textPrimary
            placeholderTextColor: dialog.colors.textMuted
            font.pixelSize: dialog.hostWindow.dp(13)
            selectByMouse: true
            verticalAlignment: TextInput.AlignVCenter

            background: Rectangle {
                radius: dialog.colors.cornerRadiusSmall
                color: dialog.colors.inputBackground
                border.width: titleField.activeFocus ? 1 : 0
                border.color: dialog.colors.inputBorderFocused
            }
        }

        // ── Task label + counter + expand toggle ──────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: dialog.hostWindow.dp(8)

            Text {
                text: qsTr("Task")
                color: dialog.colors.textPrimary
                font.pixelSize: dialog.hostWindow.dp(13)
                font.weight: Font.Medium
            }

            Item { Layout.fillWidth: true }

            Text {
                objectName: "taskCounter"
                visible: taskField.length > 0
                text: taskField.length >= 1000
                      ? qsTr("%1 chars  ·  %2 KB")
                        .arg(taskField.length)
                        .arg((dialog.utf8ByteLength(taskField.text) / 1024).toFixed(1))
                      : qsTr("%1 chars").arg(taskField.length)
                color: dialog.colors.textMuted
                font.pixelSize: dialog.hostWindow.dp(9)
            }

            Text {
                // The wide layout already gives the prompt all the free height.
                visible: !dialog.wideLayout
                text: dialog.taskExpanded ? qsTr("Collapse ▲") : qsTr("Expand ▼")
                color: expandTaskMouse.containsMouse
                       ? dialog.colors.textPrimary : dialog.colors.accent
                font.pixelSize: dialog.hostWindow.dp(9)

                MouseArea {
                    id: expandTaskMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: dialog.taskExpanded = !dialog.taskExpanded
                }
            }
        }

        // ── Scrollable Task editor ────────────────────────────────────────────
        Rectangle {
            id: taskContainer
            Layout.fillWidth: true
            Layout.fillHeight: dialog.wideLayout
            Layout.minimumHeight: dialog.hostWindow.dp(180)
            Layout.preferredHeight: dialog.taskExpanded
                                    ? dialog.hostWindow.dp(360)
                                    : dialog.hostWindow.dp(200)
            radius: dialog.colors.cornerRadiusSmall
            color: dialog.colors.inputBackground
            border.width: taskField.activeFocus ? 1 : 0
            border.color: dialog.colors.inputBorderFocused
            clip: true

            Behavior on Layout.preferredHeight {
                NumberAnimation { duration: dialog.colors.reduceAnimations ? 0 : 160; easing.type: Easing.OutCubic }
            }

            Flickable {
                id: taskFlickable
                objectName: "taskFlickable"
                anchors.fill: parent
                anchors.margins: dialog.hostWindow.dp(2)
                clip: true
                contentWidth: width
                contentHeight: Math.max(taskField.implicitHeight, height)
                boundsBehavior: Flickable.StopAtBounds

                // Connects this Flickable to the TextArea for cursor tracking.
                TextArea.flickable: taskField

                TextArea {
                    id: taskField
                    objectName: "taskField"
                    // The room for the scrollbar is always reserved: tying the width to the
                    // scrollbar's visibility made the text re-wrap when the bar appeared, which
                    // changed the height that decides whether the bar appears (a binding loop).
                    width: Math.max(0, taskFlickable.width - dialog.hostWindow.dp(12))
                    placeholderText: productionCheck.checked
                                     ? qsTr("Describe the objective, constraints and expected result for the agents.\nThe selected project context is added by the backend.")
                                     : qsTr("Describe what the local test workflow should accomplish.\nNo provider request will be made.")
                    color: dialog.colors.textPrimary
                    placeholderTextColor: dialog.colors.textMuted
                    font.pixelSize: dialog.hostWindow.dp(13)
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    // No background — the parent Rectangle provides it.
                    background: null
                }

                ScrollBar.vertical: ScrollBar {
                    id: taskScrollBar
                    visible: taskFlickable.contentHeight > taskFlickable.height
                    policy: ScrollBar.AsNeeded
                    contentItem: Rectangle {
                        implicitWidth: dialog.hostWindow.dp(5)
                        radius: width / 2
                        color: taskScrollBar.active  ? dialog.colors.textSecondary
                             : taskScrollBar.hovered ? dialog.colors.textMuted
                             : dialog.colors.border
                        opacity: taskScrollBar.active || taskScrollBar.hovered ? 1.0 : 0.5
                    }
                    background: null
                }
            }
        }

        }
        }
    }

    footer: Rectangle {
        implicitHeight: footerColumn.implicitHeight + dialog.hostWindow.dp(24)
        color: dialog.colors.popupBackground

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: dialog.colors.separator
        }

        ColumnLayout {
            id: footerColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: dialog.hostWindow.dp(18)
            anchors.rightMargin: dialog.hostWindow.dp(18)
            spacing: dialog.hostWindow.dp(8)

        Text {
            Layout.fillWidth: true
            visible: dialog.errorText.length > 0
            text: dialog.errorText
            color: dialog.colors.error
            wrapMode: Text.WordWrap
            font.pixelSize: dialog.hostWindow.dp(12)
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: dialog.hostWindow.dp(4)
            spacing: dialog.hostWindow.dp(8)

            RunActionButton {
                objectName: "convertToManifestButton"
                visible: dialog.conversionOffered
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                text: qsTr("Convert to Manifest…")
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Re-launch this roster on the Manifest workflow instead. You will see exactly which settings are not carried over before anything is created.")
                onClicked: {
                    const sourceRunId = String(dialog.prefillData.sourceRunId || "")
                    dialog.close()
                    dialog.conversionRequested(sourceRunId)
                }
            }

            Item { Layout.fillWidth: true }

            RunActionButton {
                objectName: "updateDraftButton"
                visible: dialog.prefillData !== null && dialog.prefillData !== undefined
                         && Boolean(dialog.prefillData.isDraft)
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                text: qsTr("Update this draft")
                enabled: projectCombo.currentIndex >= 0
                         && titleField.text.trim().length > 0
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Writes the changes back into the draft you opened. Its group, order and suggested dependencies stay as they are.")
                onClicked: dialog.updateDraft()
            }

            RunActionButton {
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                text: qsTr("Save as draft")
                enabled: projectCombo.currentIndex >= 0
                         && titleField.text.trim().length > 0
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Saves this configuration without starting a run. Reopen it later from Drafts - saving never deletes the draft, so a frequently reused one also works as a template.")
                onClicked: dialog.saveDraft()
            }

            RunActionButton {
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                text: qsTr("Cancel")
                onClicked: dialog.reject()
            }

            RunActionButton {
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                text: productionCheck.checked
                      ? qsTr("Queue real run")
                      : qsTr("Start test run")
                primary: true
                enabled: projectCombo.currentIndex >= 0
                         && titleField.text.trim().length > 0
                         && taskField.text.trim().length > 0
                         && (!productionCheck.checked
                             || (customRosterCheck.checked
                                 ? dialog.agentChoices.length > 0 && agentsModel.count > 0
                                 : profileCombo.currentIndex >= 0))
                onClicked: {
                    // The page owns the backend call and only closes this dialog
                    // on success, so a validation failure keeps the input visible.
                    dialog.submit()
                }
            }
            }
        }
    }
}
