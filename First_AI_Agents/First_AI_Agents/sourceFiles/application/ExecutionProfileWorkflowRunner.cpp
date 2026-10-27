#include "ai/application/ExecutionProfileWorkflowRunner.hpp"

#include "ai/application/artifacts/ArtifactService.hpp"
#include "ai/application/ContextSnapshot.hpp"
#include "ai/application/CostCalculation.hpp"
#include "ai/application/ExecutionManifestParser.hpp"
#include "ai/application/ExecutionManifestService.hpp"
#include "ai/application/ExecutionManifestValidator.hpp"
#include "ai/application/conversations/IProjectMemoryRepository.hpp"
#include "ai/application/ModelExecutionService.hpp"
#include "ai/application/ProjectWritePolicy.hpp"
#include "ai/application/RunContextAssembler.hpp"
#include "ai/application/Sha256.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace ai::application {

namespace {

struct TaskOutcome {
    bool success {false};
    bool cancelled {false};
    std::string output;
    std::string errorCode;
    std::optional<ExecutionManifest> manifest;
    std::string errorMessage;
    models::TokenUsage usage;
    std::optional<ProposedChangeSet> proposal;

    // Audited tool-session evidence for a workspace-access task, copied from
    // AgentToolTurnLoopOutcome; zero for a plain single-shot (non-workspace)
    // task, which never runs a tool-use turn loop at all.
    std::size_t totalTurns {0};
    std::size_t totalToolCalls {0};
    std::size_t successfulInspectionCalls {0};
    std::size_t totalReturnedBytes {0};
};

std::string titleForAssignment(const StageModelAssignment& assignment)
{
    if (!assignment.agentName.empty()) return assignment.agentName;
    if (assignment.stageType == "architect") return "Architecture";
    if (assignment.stageType == "implementation") return "Implementation";
    if (assignment.stageType == "documentation") return "Documentation";
    if (assignment.stageType == "synthesis") return "Final synthesis";
    return assignment.stageType;
}

bool requiresArchitectManifest(
    const std::vector<StageModelAssignment>& assignments,
    std::string_view architectId)
{
    return std::any_of(assignments.begin(), assignments.end(),
        [&](const auto& assignment) {
            return assignment.stageType != architectId
                && assignment.stageType != "synthesis"
                && assignment.workspaceAccess;
        });
}

common::Result<ExecutionManifest> validateArchitectManifest(
    std::string_view rawOutput,
    const RunId& runId,
    const std::vector<StageModelAssignment>& assignments,
    std::string_view architectId,
    std::uint32_t maxParallelism)
{
    auto parsed = ExecutionManifestParser::parse(rawOutput, runId);
    if (!parsed.success()) return parsed;

    std::unordered_map<std::string, const StageModelAssignment*> workers;
    for (const auto& assignment : assignments) {
        if (assignment.stageType == architectId || assignment.stageType == "synthesis") continue;
        workers.emplace(assignment.stageType, &assignment);
    }

    ExecutionManifestPolicy policy;
    policy.maxTasks = static_cast<std::uint32_t>(std::max<std::size_t>(1, workers.size()));
    policy.maxDepth = static_cast<std::uint32_t>(std::max<std::size_t>(1, workers.size()));
    policy.maxParallelism = std::clamp<std::uint32_t>(maxParallelism, 1, 32);
    parsed.value().policy = policy;

    const auto findings = ExecutionManifestValidator::validate(parsed.value(), nullptr);
    if (!findings.empty()) {
        return common::Result<ExecutionManifest>::failure(
            {findings.front().code, findings.front().message});
    }
    if (parsed.value().tasks.size() != workers.size()) {
        return common::Result<ExecutionManifest>::failure({
            "run.architect_manifest_roster_mismatch",
            "Architect manifest must contain exactly one task for every non-Architect roster agent"});
    }

    std::unordered_set<std::string> seen;
    for (const auto& task : parsed.value().tasks) {
        const auto worker = workers.find(task.id);
        if (worker == workers.end() || !seen.insert(task.id).second) {
            return common::Result<ExecutionManifest>::failure({
                "run.architect_manifest_roster_mismatch",
                "Architect manifest task ids must exactly match non-Architect roster stage ids"});
        }
        if (worker->second->workspaceAccess) {
            if (task.requestedCapability != WorkspaceCapability::Propose) {
                return common::Result<ExecutionManifest>::failure({
                    "run.architect_manifest_capability_mismatch",
                    "Every workspace-access roster agent must request propose capability"});
            }
            if (task.ownedPaths.empty()) {
                return common::Result<ExecutionManifest>::failure({
                    "run.architect_manifest_missing_ownership",
                    "Every workspace-access roster agent must own at least one path"});
            }
        } else if (task.requestedCapability != WorkspaceCapability::None
                || !task.ownedPaths.empty()) {
            return common::Result<ExecutionManifest>::failure({
                "run.architect_manifest_capability_mismatch",
                "Agents without workspace access cannot receive owned paths or workspace capability"});
        }
    }
    return parsed;
}

std::string architectManifestInstructions(
    const std::vector<StageModelAssignment>& assignments,
    std::string_view architectId,
    std::uint32_t maxParallelism)
{
    std::ostringstream prompt;
    prompt
        << "You are the lead Architect for an automated multi-agent project run. "
           "Decompose the Run Task into one strict, non-overlapping allocation per roster "
           "worker. Your entire response MUST be one raw JSON object following "
           "execution-manifest.v1: no prose, no Markdown, and no code fence. Use exactly "
           "these top-level fields: schemaVersion, tasks, requestedMaxParallelism. Each "
           "task must contain exactly id, objective, role, dependencies, ownedPaths, "
           "acceptanceCriteria, contextReferences, requestedCapability. Do not create a "
           "task for the Architect or synthesis. dependencies may reference only worker "
           "ids listed below. ownedPaths must be clean workspace-relative file or directory "
           "paths and must not overlap across tasks. A workspace worker must request "
           "'propose' and own at least one path; a non-workspace worker must request 'none' "
           "and own no paths. Each task objective and its acceptanceCriteria MUST be "
           "self-contained: workers receive the authoritative Run Task, their own task "
           "record, and dependency outputs, but not the full Project Description, Project "
           "Memory, or other task records. Copy every project constraint essential to a "
           "worker into that worker's objective or acceptanceCriteria. contextReferences "
           "are labels for traceability, not a substitute for required instructions. Keep "
           "requestedMaxParallelism between 1 and "
        << std::clamp<std::uint32_t>(maxParallelism, 1, 32)
        << ".\n\nExact worker roster:";
    for (const auto& member : assignments) {
        if (member.stageType == architectId || member.stageType == "synthesis") continue;
        prompt << "\n- id=" << member.stageType
               << "; name=" << titleForAssignment(member)
               << "; role=" << (member.role.empty() ? member.stageType : member.role)
               << "; workspace=" << (member.workspaceAccess ? "propose" : "none");
    }
    return prompt.str();
}

const StageModelAssignment* architectOf(
    const std::vector<StageModelAssignment>& assignments)
{
    const auto lead = std::find_if(assignments.begin(), assignments.end(),
        [](const auto& assignment) { return assignment.leadArchitect; });
    if (lead != assignments.end()) return &*lead;
    const auto canonical = std::find_if(assignments.begin(), assignments.end(),
        [](const auto& assignment) { return assignment.stageType == "architect"; });
    return canonical == assignments.end() ? nullptr : &*canonical;
}

std::vector<StageModelAssignment> graphAssignments(
    const std::vector<StageModelAssignment>& snapshot)
{
    auto graph = snapshot;
    std::sort(graph.begin(), graph.end(), [](const auto& left, const auto& right) {
        return left.position < right.position;
    });
    const auto* architect = architectOf(graph);
    if (architect == nullptr) return graph;
    // A synthesis pass only has something to reconcile when at least one
    // other agent ran alongside the architect. For a lone architect it would
    // just re-send the architect's own single output back through the same
    // model, doubling context-processing cost for a near-identical restatement.
    if (graph.size() <= 1) return graph;
    const std::string architectId = architect->stageType;

    for (auto& assignment : graph) {
        if (assignment.stageType != architectId && assignment.dependencies.empty()) {
            assignment.dependencies.push_back(architectId);
        }
    }

    StageModelAssignment synthesis = *architect;
    synthesis.stageType = "synthesis";
    synthesis.agentName = "Final synthesis";
    synthesis.role = "synthesis";
    synthesis.instructions =
        "Combine the completed agent outputs into one coherent, actionable final result. "
        "Resolve contradictions explicitly and preserve important implementation details.";
    synthesis.leadArchitect = false;
    synthesis.dependencies.clear();
    std::uint32_t finalPosition = 0;
    for (const auto& assignment : graph) {
        synthesis.dependencies.push_back(assignment.stageType);
        finalPosition = std::max(finalPosition, assignment.position + 1);
    }
    synthesis.position = finalPosition;
    graph.push_back(std::move(synthesis));
    return graph;
}

std::string dependencyContext(
    const StageModelAssignment& assignment,
    const std::unordered_map<std::string, std::string>& outputs,
    std::string_view hiddenDependency = {})
{
    std::ostringstream context;
    for (const auto& dependency : assignment.dependencies) {
        if (!hiddenDependency.empty() && dependency == hiddenDependency) continue;
        const auto found = outputs.find(dependency);
        if (found == outputs.end()) continue;
        context << "\n## Output from " << dependency << "\n\n"
                << found->second << "\n";
    }
    return context.str();
}

// Audited tool-session evidence (turn/call/inspection/byte counts) plus, when
// present, the full proposal record - the complete structured evidence a
// downstream dependent task or the deterministic run summary needs to judge
// what a workspace-access task actually did, not just its terminal
// state/prose. Previously only the proposal half of this existed; the tool
// evidence never left AgentToolTurnLoopOutcome at all.
std::string proposalEvidence(const TaskOutcome& outcome)
{
    std::ostringstream out;
    out << "Tool evidence\n"
        << "- total_turns: " << outcome.totalTurns << "\n"
        << "- total_tool_calls: " << outcome.totalToolCalls << "\n"
        << "- successful_inspection_calls: " << outcome.successfulInspectionCalls << "\n"
        << "- total_returned_bytes: " << outcome.totalReturnedBytes << "\n";

    if (!outcome.proposal.has_value()) return out.str();
    const auto& proposal = *outcome.proposal;
    out << "\nProposal evidence\n"
        << "- change_set_id: " << proposal.changeSetId << "\n"
        << "- state: " << toString(proposal.state) << "\n"
        << "- summary: " << proposal.summary << "\n"
        << "- changes: " << proposal.changes.size() << "\n";
    for (const auto& change : proposal.changes) {
        out << "  - " << change.changeId << ": " << toString(change.operation)
            << " " << change.relativePath.generic_string() << "\n";
    }
    out << "- validation_findings: " << proposal.validationFindings.size() << "\n";
    for (const auto& finding : proposal.validationFindings) {
        out << "  - " << finding.code;
        if (!finding.changeId.empty()) out << " [" << finding.changeId << "]";
        out << ": " << finding.message << "\n";
    }
    return out.str();
}

std::string rejectedProposalMessage(const ProposedChangeSet& proposal)
{
    std::ostringstream out;
    out << "Change set " << proposal.changeSetId << " was rejected";
    if (!proposal.validationFindings.empty()) {
        out << ": ";
        for (std::size_t i = 0; i < proposal.validationFindings.size(); ++i) {
            if (i > 0) out << "; ";
            const auto& finding = proposal.validationFindings[i];
            out << finding.code;
            if (!finding.changeId.empty()) out << " [" << finding.changeId << "]";
            out << " - " << finding.message;
        }
    }
    return out.str();
}

bool isProposalOnlyWorkerGraph(
    const std::vector<StageModelAssignment>& assignments,
    std::string_view architectId)
{
    bool sawWorker = false;
    for (const auto& assignment : assignments) {
        if (assignment.stageType == architectId || assignment.stageType == "synthesis") continue;
        sawWorker = true;
        if (!assignment.workspaceAccess) return false;
    }
    return sawWorker;
}

std::string deterministicRunSummary(
    const std::vector<StageModelAssignment>& assignments,
    const std::unordered_map<std::string, TaskOutcome>& outcomes,
    RunStatus finalStatus,
    std::string_view terminalMessage,
    bool providerSynthesisSkipped,
    const models::TokenUsage& usage)
{
    std::ostringstream out;
    out << "# Deterministic Run Summary\n\n"
        << "- status: " << toString(finalStatus) << "\n"
        << "- message: " << terminalMessage << "\n"
        << "- input_tokens: " << usage.inputTokens << "\n"
        << "- output_tokens: " << usage.outputTokens << "\n"
        << "- provider_synthesis: "
        << (providerSynthesisSkipped ? "skipped" : "executed_or_not_applicable") << "\n";

    for (const auto& assignment : assignments) {
        if (assignment.stageType == "synthesis") continue;
        out << "\n## Task " << assignment.stageType << "\n\n";
        const auto found = outcomes.find(assignment.stageType);
        if (found == outcomes.end()) {
            out << "- state: not_run\n";
            continue;
        }
        const auto& outcome = found->second;
        out << "- state: "
            << (outcome.cancelled ? "cancelled" : outcome.success ? "completed" : "failed")
            << "\n"
            << "- input_tokens: " << outcome.usage.inputTokens << "\n"
            << "- output_tokens: " << outcome.usage.outputTokens << "\n";
        if (!outcome.errorCode.empty()) out << "- error_code: " << outcome.errorCode << "\n";
        if (!outcome.errorMessage.empty()) out << "- error: " << outcome.errorMessage << "\n";
        if (outcome.proposal.has_value() || outcome.totalTurns > 0) {
            out << "\n" << proposalEvidence(outcome);
        } else if (!outcome.output.empty()) {
            out << "- report_output_bytes: " << outcome.output.size() << "\n";
        }
    }
    return out.str();
}

std::string buildTaskContext(
    const Project& project,
    const Run& run,
    std::string_view projectMemorySection,
    const StageModelAssignment& assignment,
    const std::string& dependencies,
    const ExecutionManifestTask* allocation = nullptr)
{
    // A validated manifest allocation is the Architect's self-contained,
    // least-context handoff to one worker. Repeating the full Project
    // Description and Project Memory for every worker (and every provider
    // tool turn) multiplies input-token cost without expanding that worker's
    // authority. The Architect still receives both below, and legacy graph
    // tasks without an allocation retain the established full-context shape.
    if (allocation != nullptr) {
        std::ostringstream out;
        out << "# Run Task (authoritative - this is what you must do now)\n\n"
            << run.task << "\n\n"
            << "# Current Task (self-contained Architect allocation)\n\n"
            << "The Architect received the full Project Description and Project Memory. "
               "They are intentionally not repeated here. Treat this allocation as complete; "
               "inspect the workspace for repository truth and do not infer authority beyond "
               "the owned paths below.\n\n"
            << "Agent id: " << allocation->id << "\n"
            << "Objective: " << allocation->objective << "\n"
            << "Role: " << allocation->role << "\n"
            << "Requested workspace capability: "
            << toString(allocation->requestedCapability) << "\n";
        if (!allocation->ownedPaths.empty()) {
            out << "Owned paths (hard proposal boundary):\n";
            for (const auto& path : allocation->ownedPaths)
                out << "- " << path.generic_string() << "\n";
            out << "You may inspect other files for context, but propose changes ONLY inside "
                   "the owned paths above. Never include another agent's path in your change set.\n";
        }
        if (!allocation->acceptanceCriteria.empty()) {
            out << "Acceptance criteria:\n";
            for (const auto& criterion : allocation->acceptanceCriteria)
                out << "- " << criterion << "\n";
        }
        if (!allocation->contextReferences.empty()) {
            out << "Context references (traceability labels only):\n";
            for (const auto& reference : allocation->contextReferences)
                out << "- " << reference << "\n";
        }
        if (!dependencies.empty()) out << "\n# Dependency Outputs\n" << dependencies;
        return out.str();
    }

    // Explicit primacy framing around Project Memory: a 2026-07-24 fix (in
    // the now-retired single-agent ReadOnlyAnalysisWorkflowRunner) found
    // models echoing a prior run's own output/proposal instead of doing the
    // actual task, because memory segments were concatenated directly ahead
    // of the task with no signal that they were background only. That
    // framing was lost when the runner was merged into this one on
    // 2026-07-26 and never carried over. Restored here, adapted to this
    // runner's existing "# Run Task" (the whole run's objective) heading
    // rather than duplicating it.
    const bool hasMemory = !projectMemorySection.empty();
    std::ostringstream out;
    out << "# Project Description\n\n" << project.description << "\n\n";
    if (hasMemory) {
        out << "The section below is background context only - do not "
               "restate its findings, do not repeat its proposed changes, "
               "and do not copy its output. Use it only if directly "
               "relevant to the Run Task below.\n\n";
        out << projectMemorySection << "\n\n";
    }
    out << "# Run Task (authoritative - this is what you must do now)\n\n"
        << run.task << "\n\n";
    if (hasMemory) {
        out << "Address the Run Task above specifically. Ignore any "
               "unrelated content from Project Memory.\n\n";
    }
    out << "# Current Task\n\n";
    out << assignment.stageType << "\n";
    if (!dependencies.empty()) out << "\n# Dependency Outputs\n" << dependencies;
    return out.str();
}

bool retryable(std::string_view code)
{
    return code.find("timeout") != std::string_view::npos
        || code.find("rate") != std::string_view::npos
        || code.find("transport") != std::string_view::npos
        || code.find("unavailable") != std::string_view::npos
        || code.find("overloaded") != std::string_view::npos
        || code.find("server") != std::string_view::npos;
}

RunEvent stageEvent(
    RunEventType type,
    const RunId& runId,
    const StageId& stageId,
    double progress,
    std::string message,
    models::TokenUsage usage = {})
{
    RunEvent event;
    event.type = type;
    event.runId = runId;
    event.stageId = stageId;
    event.stageProgress = type == RunEventType::StageCompleted ? 1.0 : 0.0;
    event.runProgress = progress;
    event.message = std::move(message);
    event.usage = usage;
    return event;
}

} // namespace

ExecutionProfileWorkflowRunner::ExecutionProfileWorkflowRunner(
    IProviderClientFactory& clientFactory,
    IProviderRepository& providers,
    IModelRepository& models,
    ArtifactService* artifactService,
    ModelExecutionService* modelExecutionService,
    IProjectMemoryRepository* projectMemoryRepository,
    providers::IProviderAgentSessionFactory* agentSessionFactory,
    AgentToolExecutionService* agentToolExecution,
    ChangeProposalService* changeProposals,
    ExecutionManifestService* executionManifests)
    : clientFactory_(clientFactory)
    , providers_(providers)
    , models_(models)
    , artifactService_(artifactService)
    , modelExecutionService_(modelExecutionService)
    , projectMemoryRepository_(projectMemoryRepository)
    , agentSessionFactory_(agentSessionFactory)
    , agentToolExecution_(agentToolExecution)
    , changeProposals_(changeProposals)
    , executionManifests_(executionManifests)
{
    if (agentSessionFactory_ != nullptr && agentToolExecution_ != nullptr
            && changeProposals_ != nullptr) {
        workspaceTurnLoop_.emplace(*agentSessionFactory_, *agentToolExecution_, *changeProposals_);
    }
}

std::vector<RunStage> ExecutionProfileWorkflowRunner::createStages(
    const RunId& runId, const ResolvedExecutionPlan& plan) const
{
    std::vector<RunStage> stages;
    for (const auto& assignment : graphAssignments(plan.assignments)) {
        RunStage stage;
        stage.id = runId + ":" + assignment.stageType;
        stage.runId = runId;
        stage.type = assignment.stageType;
        stage.title = titleForAssignment(assignment);
        stage.description = assignment.role.empty()
            ? "Execution graph task"
            : assignment.role;
        stage.providerId = assignment.providerId;
        stage.modelId = assignment.modelId;
        stages.push_back(std::move(stage));
    }
    return stages;
}

WorkflowResult ExecutionProfileWorkflowRunner::run(
    const WorkflowRunContext& context,
    std::stop_token stopToken,
    IRunEventSink& eventSink)
{
    const auto assignments = graphAssignments(context.run.assignmentSnapshot);
    if (assignments.empty() || architectOf(assignments) == nullptr) {
        return {false, RunStatus::Failed,
            "Production execution graph has no lead architect", {}};
    }

    // AG-17.7D-1: `enabled=false` means no new provider session. A Run that replays a frozen
    // snapshot (a Retry never re-validates the catalog) is held to the switch HERE, before any
    // stage starts and before any provider is touched: every agent's provider and model, and its
    // fallback pair, must be enabled. A Run already running is not re-checked mid-flight.
    for (const auto& assignment : assignments) {
        const std::string label = assignment.stageType;
        const auto disabledReason = [&](const ProviderId& providerId, const ModelId& modelId,
                                        const char* kind) -> std::optional<std::string> {
            const auto provider = providers_.findById(providerId);
            if (provider.success() && provider.value().has_value() && !provider.value()->enabled) {
                return "The " + std::string(kind) + " provider of agent '" + label + "' is disabled";
            }
            const auto model = models_.findById(modelId);
            if (model.success() && model.value().has_value() && !model.value()->enabled) {
                return "The " + std::string(kind) + " model of agent '" + label + "' is disabled";
            }
            return std::nullopt;
        };
        auto reason = disabledReason(assignment.providerId, assignment.modelId, "primary");
        if (!reason.has_value() && assignment.fallbackProviderId.has_value()
            && assignment.fallbackModelId.has_value()) {
            reason = disabledReason(*assignment.fallbackProviderId, *assignment.fallbackModelId, "fallback");
        }
        if (reason.has_value()) {
            return {false, RunStatus::Failed, "run.execution_target_disabled: " + *reason, {}};
        }
    }

    const auto budgetPolicyValid = validateProviderExecutionBudgetPolicy(
        context.run.providerBudget);
    if (!budgetPolicyValid.success()) {
        return {false, RunStatus::Failed,
            budgetPolicyValid.error()->code + ": " + budgetPolicyValid.error()->message,
            {}};
    }
    // Same Run-wide authority ManifestWorkflowRunner constructs: every
    // roster agent in this graph (single-shot Architect/Implementation/
    // Documentation/synthesis calls below, and any workspaceAccess agent's
    // AgentToolTurnLoopRunner session) shares this one guard so cumulative
    // provider consumption is bounded across the whole Run, not reset per
    // stage. Unlike the Manifest workflow's single-session guard, this
    // graph deliberately dispatches independent roster tasks concurrently
    // (see maxParallelism below), so the guard is allowed the same number
    // of simultaneously in-flight requests instead of serializing every
    // provider call to one at a time - the worst-case token overshoot is
    // then still bounded, by at most maxParallelism in-flight turns.
    auto providerBudgetGuard = std::make_shared<ProviderSessionBudgetGuard>(
        context.run.providerBudget,
        context.run.providerBudgetUsage,
        [&eventSink, runId = context.run.id](const ProviderExecutionBudgetUsage& usage) {
            return eventSink.persistProviderBudgetUsage(runId, usage);
        },
        ProviderSessionBudgetGuard::Clock {},
        std::clamp<std::size_t>(context.run.maxParallelism, 1, 32));

    std::unordered_map<std::string, StageModelAssignment> byId;
    std::unordered_map<std::string, RunStage> stagesByType;
    std::unordered_map<std::string, std::string> outputs;
    std::unordered_map<std::string, TaskOutcome> taskOutcomes;
    std::unordered_map<std::string, ExecutionManifestTask> allocations;
    std::unordered_set<std::string> completed;
    std::unordered_set<std::string> skipped;
    std::unordered_set<std::string> pending;
    for (const auto& assignment : assignments) {
        byId.emplace(assignment.stageType, assignment);
        pending.insert(assignment.stageType);
    }
    for (const auto& stage : context.stages) stagesByType.emplace(stage.type, stage);

    const auto architect = *architectOf(assignments);
    const bool architectMustAllocate =
        requiresArchitectManifest(assignments, architect.stageType);
    const bool skipProviderSynthesis =
        isProposalOnlyWorkerGraph(assignments, architect.stageType);
    RunContextAssembly projectMemory;
    if (projectMemoryRepository_ != nullptr) {
        const auto segments =
            projectMemoryRepository_->listMemorySegments(context.project.id);
        if (!segments.success()) {
            return {false, RunStatus::Failed,
                "Project memory could not be loaded: " + segments.error()->code, {}};
        }
        RunContextAssemblyRequest memoryRequest;
        memoryRequest.memorySegments = segments.value();
        memoryRequest.memoryTokenBudget = projectMemoryBudget(
            assignments, [this](const ModelId& id) { return models_.findById(id); });
        const auto assembled = assembleRunContext(std::move(memoryRequest));
        if (!assembled.success()) {
            return {false, RunStatus::Failed,
                "Project memory context could not be assembled: "
                    + assembled.error()->code, {}};
        }
        projectMemory = assembled.value();
    }

    if (artifactService_ != nullptr) {
        const auto snapshot = buildRunContextSnapshot(
            context.run.id,
            context.project.id,
            context.project.description,
            context.run.task,
            {},
            std::nullopt,
            projectMemory.projectMemorySection,
            projectMemory.includedMemorySegmentIds);
        if (snapshot.success()) {
            const std::optional<StageId> stageId =
                context.stages.empty()
                ? std::nullopt
                : std::optional<StageId> {context.stages.front().id};
            static_cast<void>(artifactService_->createDiagnosticArtifact(
                context.run.id,
                stageId,
                ArtifactKind::ContextSnapshot,
                "context-snapshot.md",
                "text/markdown",
                renderRunContextSnapshot(snapshot.value())));
        }
    }

    const std::size_t totalTasks = assignments.size();
    std::size_t parallelism = std::clamp<std::size_t>(
        context.run.maxParallelism, 1, 32);
    models::TokenUsage totalUsage;

    // Runs one workspaceAccess == true agent through the shared
    // AgentToolTurnLoopRunner (read-only workspace tools + propose_change_set)
    // instead of a single-shot completion - the same mechanics
    // ReadOnlyAnalysisWorkflowRunner already gives its one agent, now
    // available to any individual roster agent alongside non-workspace
    // agents in the same parallel dependency graph.
    auto executeWorkspaceTask = [&](const StageModelAssignment& assignment,
                                    const std::string& inputs,
                                    const RunStage& stage) -> TaskOutcome {
        TaskOutcome outcome;
        if (agentSessionFactory_ == nullptr || agentToolExecution_ == nullptr
                || changeProposals_ == nullptr || !workspaceTurnLoop_.has_value()
                || !agentToolExecution_->storageAvailable()) {
            outcome.errorCode = "run.workspace_access_unavailable";
            outcome.errorMessage =
                "Workspace tool access is not wired or unavailable for this runner";
            return outcome;
        }

        const auto provider = providers_.findById(assignment.providerId);
        const auto model = models_.findById(assignment.modelId);
        if (!provider.success() || !provider.value().has_value()
                || !model.success() || !model.value().has_value()) {
            outcome.errorCode = "run.execution_target_unavailable";
            outcome.errorMessage = "Provider or model disappeared after plan resolution";
            return outcome;
        }

        const std::filesystem::path workspaceRoot =
            context.run.workspacePath.value_or(context.project.rootPath);
        const std::string role = assignment.role.empty() ? assignment.stageType : assignment.role;
        const auto allocationFound = allocations.find(assignment.stageType);
        if (allocationFound == allocations.end()
                || allocationFound->second.ownedPaths.empty()) {
            outcome.errorCode = "run.workspace_ownership_missing";
            outcome.errorMessage =
                "Workspace agent has no validated Architect-owned path allocation";
            return outcome;
        }

        providers::ProviderAgentSessionRequest sessionRequest;
        sessionRequest.providerId = assignment.providerId;
        sessionRequest.modelId = assignment.modelId;
        sessionRequest.apiModelId = model.value()->apiModelId;
        sessionRequest.systemPrompt = std::string(kWorkspaceToolsSystemPrompt)
            + "\n\nYour role: " + role
            + "\n\nThe Current Task allocation is authoritative. Propose changes only "
              "inside its owned paths; the Backend rejects every other path.";
        if (!assignment.instructions.empty()) {
            sessionRequest.systemPrompt += "\n\nAdditional instructions:\n" + assignment.instructions;
        }
        sessionRequest.userContent = buildTaskContext(
            context.project, context.run, projectMemory.projectMemorySection, assignment, inputs,
            &allocationFound->second);
        sessionRequest.tools = agentToolExecution_->toolDefinitions();
        sessionRequest.tools.push_back(proposeChangeSetToolDefinition());
        sessionRequest.turnTimeout = assignment.timeout.count() > 0
            ? assignment.timeout : kAgentToolTurnLoopDefaultTurnTimeout;
        sessionRequest.maxOutputTokens = model.value()->maxOutputTokens > 0
            ? static_cast<std::uint32_t>(std::min<std::uint64_t>(
                  model.value()->maxOutputTokens, std::numeric_limits<std::uint32_t>::max()))
            : kAgentToolTurnLoopMaxOutputTokens;
        sessionRequest.maxTurns = kAgentToolTurnLoopMaxTurns;
        sessionRequest.maxTotalToolCalls = kAgentToolTurnLoopMaxTotalToolCalls;
        sessionRequest.maxToolCallsPerTurn = kAgentToolTurnLoopMaxToolCallsPerTurn;

        // Mirrors the plain single-shot path's model-execution bookkeeping
        // (below in executeTask) so a workspace-access agent's real cost/
        // usage reaches the same ledger ExecutionAnalyticsService/Overview/
        // Comparison read from. This entire block used to be missing here:
        // the run/stage completed normally and its token usage still
        // reached Run.tokenUsage via outcome.usage, but no row was ever
        // written to model_executions, so a project run only through
        // workspace-access agents (the actual "propose changes" workflow)
        // showed zero cost on Overview even with real, priced models.
        const auto attemptStartedAt = std::chrono::steady_clock::now();
        std::optional<ModelExecutionId> executionId;
        if (modelExecutionService_ != nullptr) {
            CreateModelExecutionRequest create;
            create.source = ModelExecutionSource::Run;
            create.sourceId = context.run.id;
            create.projectId = context.project.id;
            create.providerId = assignment.providerId;
            create.modelId = assignment.modelId;
            create.role = role;
            create.position = assignment.position;
            create.attempt = 1;
            create.requestHash = sha256Hex(
                sessionRequest.systemPrompt + "\n" + sessionRequest.userContent);
            create.contextHash = sha256Hex(sessionRequest.userContent);
            const auto created = modelExecutionService_->createExecution(create);
            if (created.success()) {
                executionId = created.value().id;
                static_cast<void>(modelExecutionService_->startExecution(*executionId));
            }
        }

        ProjectWritePolicy writePolicy;
        writePolicy.projectRoot = workspaceRoot;
        writePolicy.allowedRoots = allocationFound->second.ownedPaths;
        writePolicy.allowDelete = true;
        writePolicy.allowRename = true;

        AgentToolTurnLoopRequest loopRequest;
        loopRequest.sessionRequest = std::move(sessionRequest);
        loopRequest.runId = context.run.id;
        loopRequest.providerKind = provider.value()->kind;
        loopRequest.policy = AgentToolExecutionService::buildPolicy(workspaceRoot);
        loopRequest.proposalTemplate.runId = context.run.id;
        loopRequest.proposalTemplate.projectId = context.project.id;
        loopRequest.proposalTemplate.providerId = assignment.providerId;
        loopRequest.proposalTemplate.modelId = assignment.modelId;
        // Same ModelExecution row created above (executionId), so a
        // persisted proposal can always be traced back to the exact
        // execution that produced it - this was previously left unset,
        // leaving proposed_change_sets.model_execution_id empty for every
        // proposal a workspace-access agent ever produced.
        loopRequest.proposalTemplate.modelExecutionId = executionId;
        loopRequest.proposalTemplate.agentRole = role;
        loopRequest.proposalTemplate.policy = writePolicy;
        loopRequest.providerBudgetGuard = providerBudgetGuard;
        // AG-17 M2b: classify every response against the threshold of `model` - the ONE Model
        // copy this attempt looked up above, whose prices later price result.usage.
        loopRequest.longContextThresholdInputTokens =
            model.value()->longContextPricing.thresholdInputTokens;

        const auto result = workspaceTurnLoop_->run(
            std::move(loopRequest), stopToken, &eventSink, &stage.id);

        std::optional<ArtifactId> artifactId;
        if (artifactService_ != nullptr && !result.finalText.empty()) {
            const auto artifact = artifactService_->createArtifact(
                context.run.id, stage.id, ArtifactKind::Report,
                assignment.stageType + "-output.md", "text/markdown", result.finalText);
            if (artifact.success()) artifactId = artifact.value().id;
        }

        outcome.success = result.success;
        outcome.cancelled = result.cancelled;
        outcome.errorCode = result.errorCode;
        outcome.errorMessage = result.errorMessage;
        outcome.usage = result.usage;
        outcome.totalTurns = result.totalTurns;
        outcome.totalToolCalls = result.totalToolCalls;
        outcome.successfulInspectionCalls = result.successfulInspectionCalls;
        outcome.totalReturnedBytes = result.totalReturnedBytes;
        // Preserve structured proposal/validation evidence for downstream
        // decisions. The previous lossy one-line prose summary caused a real
        // synthesis run to claim that a worker had not inspected the
        // filesystem even though its audited tool session contained twelve
        // successful calls. Proposed content stays in the immutable proposal
        // artifact; dependency context receives only safe metadata, paths and
        // validation findings.
        if (result.proposal.has_value()) {
            outcome.proposal = result.proposal;
            outcome.output = proposalEvidence(outcome);
            // Defence in depth while AgentToolTurnLoopRunner's proposal-state
            // contract is hardened independently: a parsed but Rejected
            // proposal must never satisfy this workflow task or unlock a
            // billable dependent/synthesis stage.
            if (result.proposal->state == ChangeSetState::Rejected) {
                outcome.success = false;
                if (outcome.errorCode.empty()) {
                    outcome.errorCode = "run.proposal_rejected";
                }
                if (outcome.errorMessage.empty()) {
                    outcome.errorMessage = rejectedProposalMessage(*result.proposal);
                }
            }
        } else {
            outcome.output = result.finalText;
        }

        if (executionId.has_value()) {
            FinishModelExecutionRequest finish;
            finish.status = outcome.cancelled
                ? ModelExecutionStatus::Cancelled
                : (outcome.success ? ModelExecutionStatus::Completed : ModelExecutionStatus::Failed);
            finish.outputArtifactId = artifactId;
            finish.outputContent = outcome.output;
            finish.outputPreview = outcome.output.substr(0, 1000);
            finish.inputTokens = outcome.usage.inputTokens;
            finish.outputTokens = outcome.usage.outputTokens;
            finish.cacheCreationInputTokens = outcome.usage.cacheCreationInputTokens;
            finish.cacheReadInputTokens = outcome.usage.cacheReadInputTokens;
            finish.cacheCreationDuration = outcome.usage.cacheCreationDuration;
            // MW-P2: see CostCalculation.hpp - bills any cache-write/
            // cache-read tokens (accumulated into outcome.usage by
            // AgentToolTurnLoopRunner alongside its conservative inputTokens
            // sum) at their own rate instead of the plain input rate.
            {
                const auto costBreakdown =
                    computeAggregatedExecutionCostMicros(*model.value(), outcome.usage);
                finish.costMicros = costBreakdown.costMicros;
                finish.costPriceKnown = costBreakdown.priceKnown;
            }
            finish.latencyMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - attemptStartedAt).count());
            finish.errorCode = outcome.errorCode;
            finish.errorMessage = outcome.errorMessage;
            static_cast<void>(modelExecutionService_->finishExecution(*executionId, finish));
        }
        return outcome;
    };

    auto executeTask = [&](StageModelAssignment assignment,
                           std::string inputs) -> TaskOutcome {
        TaskOutcome outcome;
        const auto stageFound = stagesByType.find(assignment.stageType);
        if (stageFound == stagesByType.end()) {
            outcome.errorCode = "run.stage_missing";
            outcome.errorMessage = "Persisted stage is missing from the execution graph";
            return outcome;
        }

        if (assignment.workspaceAccess) {
            return executeWorkspaceTask(assignment, inputs, stageFound->second);
        }

        const ExecutionManifestTask* allocation = nullptr;
        if (architectMustAllocate
                && assignment.stageType != architect.stageType
                && assignment.stageType != "synthesis") {
            const auto found = allocations.find(assignment.stageType);
            if (found == allocations.end()) {
                outcome.errorCode = "run.worker_allocation_missing";
                outcome.errorMessage = "Worker has no validated Architect allocation";
                return outcome;
            }
            allocation = &found->second;
        }
        ProviderChatRequest request;
        request.userContent = buildTaskContext(
            context.project, context.run, projectMemory.projectMemorySection,
            assignment, inputs, allocation);
        request.timeout = assignment.timeout;
        // A production Run can legitimately spend several minutes reasoning
        // before the provider sends its first response byte. Do not turn the
        // persisted UI timeout into a hard transport deadline: connection
        // establishment remains bounded by HttpClient's independent connect
        // timeout, and the user can still stop the active transfer through
        // the cancellation callback propagated from stopToken.
        request.disableTotalTimeout = true;
        const std::string role = assignment.role.empty()
            ? assignment.stageType
            : assignment.role;
        if (assignment.stageType == "synthesis") {
            request.systemPrompt =
                "You are the lead Architect producing the final synthesis for a multi-agent run. "
                "Use every dependency output, resolve conflicts, and return one reviewable final result.";
        } else if (assignment.leadArchitect || assignment.stageType == architect.stageType) {
            if (architectMustAllocate) {
                request.systemPrompt = architectManifestInstructions(
                    assignments, architect.stageType, context.run.maxParallelism);
            } else {
                request.systemPrompt =
                    "You are the lead Architect for an automated multi-agent project run. "
                    "Analyze the full task, define the solution architecture, and give every "
                    "agent an actionable work plan.\n\nAgent roster:";
                for (const auto& member : context.run.assignmentSnapshot) {
                    request.systemPrompt += "\n- " + titleForAssignment(member)
                        + ": " + (member.role.empty() ? member.stageType : member.role);
                }
            }
        } else {
            request.systemPrompt = "You are an AI engineering agent with the role '" + role
                + "'. Execute the assigned graph task using the architect and dependency outputs. "
                  "Return concrete, reviewable output only.";
        }
        if (!assignment.instructions.empty()) {
            request.systemPrompt += "\n\nAdditional instructions:\n" + assignment.instructions;
        }

        const bool isArchitectManifest = architectMustAllocate
            && assignment.stageType == architect.stageType;
        const std::uint32_t primaryAttempts = isArchitectManifest
            ? std::max<std::uint32_t>(2, assignment.maxAttempts)
            : assignment.maxAttempts;
        const std::string originalTaskContent = request.userContent;
        struct Target { ProviderId providerId; ModelId modelId; bool fallback; };
        std::vector<Target> targets;
        for (std::uint32_t attempt = 0; attempt < primaryAttempts; ++attempt) {
            targets.push_back({assignment.providerId, assignment.modelId, false});
        }
        if (assignment.fallbackProviderId.has_value()) {
            targets.push_back({*assignment.fallbackProviderId,
                *assignment.fallbackModelId, true});
        }

        bool manifestRepairUsed = false;
        for (std::size_t index = 0; index < targets.size(); ++index) {
            if (stopToken.stop_requested()) {
                outcome.cancelled = true;
                outcome.errorCode = "provider.cancelled";
                outcome.errorMessage = "Execution cancelled";
                return outcome;
            }
            const auto& target = targets[index];
            const auto provider = providers_.findById(target.providerId);
            const auto model = models_.findById(target.modelId);
            if (!provider.success() || !provider.value().has_value()
                || !model.success() || !model.value().has_value()) {
                outcome.errorCode = "run.execution_target_unavailable";
                outcome.errorMessage = "Provider or model disappeared after plan resolution";
                return outcome;
            }

            request.providerId = target.providerId;
            request.modelId = target.modelId;
            request.apiModelId = model.value()->apiModelId;
            // Runs use the selected catalog model's provider ceiling instead
            // of ProviderChatRequest's small generic fallback. This is the
            // provider-aware "Auto" policy: the UI does not guess a token
            // budget, while OpenAI/Anthropic still receive the bound required
            // by their APIs. A zero catalog value keeps the provider-neutral
            // request in automatic/unspecified mode.
            request.maxOutputTokens = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(
                    model.value()->maxOutputTokens,
                    std::numeric_limits<std::uint32_t>::max()));
            const auto attemptStartedAt = std::chrono::steady_clock::now();
            std::optional<ModelExecutionId> executionId;
            if (modelExecutionService_ != nullptr) {
                CreateModelExecutionRequest create;
                create.source = ModelExecutionSource::Run;
                create.sourceId = context.run.id;
                create.projectId = context.project.id;
                create.providerId = target.providerId;
                create.modelId = target.modelId;
                create.role = role;
                create.position = assignment.position;
                create.attempt = static_cast<std::uint32_t>(index + 1);
                create.requestHash = sha256Hex(request.systemPrompt + "\n" + request.userContent);
                create.contextHash = sha256Hex(request.userContent);
                const auto created = modelExecutionService_->createExecution(create);
                if (!created.success()) {
                    outcome.errorCode = created.error()->code;
                    outcome.errorMessage = created.error()->message;
                    return outcome;
                }
                executionId = created.value().id;
                const auto started = modelExecutionService_->startExecution(*executionId);
                if (!started.success()) {
                    outcome.errorCode = started.error()->code;
                    outcome.errorMessage = started.error()->message;
                    return outcome;
                }
            }

            auto finishAttempt = [&](ModelExecutionStatus status,
                                     const models::TokenUsage& usage,
                                     const std::string& content,
                                     std::optional<ArtifactId> artifactId,
                                     std::string errorCode,
                                     std::string errorMessage) {
                if (!executionId.has_value()) return;
                FinishModelExecutionRequest finish;
                finish.status = status;
                finish.outputArtifactId = std::move(artifactId);
                finish.outputContent = content;
                finish.outputPreview = content.substr(0, 1000);
                finish.inputTokens = usage.inputTokens;
                finish.outputTokens = usage.outputTokens;
                finish.cacheCreationInputTokens = usage.cacheCreationInputTokens;
                finish.cacheReadInputTokens = usage.cacheReadInputTokens;
                finish.cacheCreationDuration = usage.cacheCreationDuration;
                // MW-P2: see CostCalculation.hpp. A single-shot attempt is ONE provider response
                // (or none at all: `usage` is empty for a failure before the call), so it is
                // classified against the model's long-context threshold by its own input_tokens.
                {
                    const auto costBreakdown =
                        computeSingleResponseCostMicros(*model.value(), usage);
                    finish.costMicros = costBreakdown.costMicros;
                    finish.costPriceKnown = costBreakdown.priceKnown;
                }
                finish.latencyMs = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - attemptStartedAt).count());
                finish.errorCode = std::move(errorCode);
                finish.errorMessage = std::move(errorMessage);
                static_cast<void>(modelExecutionService_->finishExecution(*executionId, finish));
            };

            const auto client = clientFactory_.clientFor(target.providerId, provider.value()->kind);
            if (!client.success()) {
                finishAttempt(ModelExecutionStatus::Failed, {}, {}, std::nullopt,
                    client.error()->code, client.error()->message);
                outcome.errorCode = client.error()->code;
                outcome.errorMessage = client.error()->message;
            } else {
                const auto budgetAdmitted = providerBudgetGuard->beginProviderTurn(stopToken);
                if (!budgetAdmitted.success()) {
                    finishAttempt(ModelExecutionStatus::Failed, {}, {}, std::nullopt,
                        budgetAdmitted.error()->code, budgetAdmitted.error()->message);
                    outcome.errorCode = budgetAdmitted.error()->code;
                    outcome.errorMessage = budgetAdmitted.error()->message;
                    return outcome;
                }
                const auto response = client.value()->send(request, stopToken);
                if (response.success()) {
                    const auto budgetRecorded =
                        providerBudgetGuard->completeProviderTurn(response.value().usage);
                    accumulateResponseUsage(
                        outcome.usage,
                        response.value().usage,
                        model.value()->longContextPricing.thresholdInputTokens);
                    if (!budgetRecorded.success()) {
                        // The call already happened and its cost is accounted for
                        // above; the Run-wide budget is now exhausted, so this
                        // stage - and any further provider call in this Run -
                        // must stop here rather than accept the content or retry
                        // a fallback target.
                        outcome.errorCode = budgetRecorded.error()->code;
                        outcome.errorMessage = budgetRecorded.error()->message;
                        finishAttempt(ModelExecutionStatus::Failed, response.value().usage,
                            response.value().content, std::nullopt,
                            outcome.errorCode, outcome.errorMessage);
                        return outcome;
                    }
                    outcome.errorCode.clear();
                    outcome.errorMessage.clear();
                    outcome.output = response.value().content;

                    std::optional<ArtifactId> artifactId;
                    if (artifactService_ != nullptr) {
                        const auto artifact = artifactService_->createArtifact(
                            context.run.id, stageFound->second.id, ArtifactKind::Report,
                            assignment.stageType
                                + (isArchitectManifest ? "-output.json" : "-output.md"),
                            isArchitectManifest ? "application/json" : "text/markdown",
                            response.value().content);
                        if (artifact.success()) artifactId = artifact.value().id;
                    }

                    if (isArchitectManifest) {
                        auto candidate = validateArchitectManifest(
                            response.value().content, context.run.id, assignments,
                            architect.stageType, context.run.maxParallelism);
                        if (!candidate.success()) {
                            outcome.errorCode = candidate.error()->code;
                            outcome.errorMessage = candidate.error()->message;
                            finishAttempt(ModelExecutionStatus::Failed, response.value().usage,
                                response.value().content, artifactId,
                                outcome.errorCode, outcome.errorMessage);
                            if (!manifestRepairUsed && index + 1 < primaryAttempts) {
                                manifestRepairUsed = true;
                                request.userContent = originalTaskContent
                                    + "\n\n# One bounded Architect manifest repair\n\n"
                                      "The previous manifest was rejected with "
                                    + outcome.errorCode + ": " + outcome.errorMessage
                                    + ". Correct only the JSON shape, roster mapping, DAG, "
                                      "capability, or owned-path constraints named by this error."
                                      "\n\nPrevious invalid output:\n"
                                    + response.value().content
                                    + "\n\nReturn the corrected raw JSON object only.";
                                continue;
                            }
                            return outcome;
                        }
                        if (executionManifests_ != nullptr) {
                            const auto policy = candidate.value().policy;
                            auto frozen = executionManifests_->freeze(candidate.value(), policy);
                            if (!frozen.success()) {
                                outcome.errorCode = frozen.error()->code;
                                outcome.errorMessage = frozen.error()->message;
                                finishAttempt(ModelExecutionStatus::Failed, response.value().usage,
                                    response.value().content, artifactId,
                                    outcome.errorCode, outcome.errorMessage);
                                return outcome;
                            }
                            outcome.manifest = std::move(frozen.value());
                        } else {
                            outcome.manifest = std::move(candidate.value());
                        }
                    }

                    finishAttempt(ModelExecutionStatus::Completed, response.value().usage,
                        response.value().content, artifactId, {}, {});
                    outcome.success = true;
                    return outcome;
                }
                providerBudgetGuard->abandonProviderTurn();
                outcome.cancelled = stopToken.stop_requested();
                outcome.errorCode = response.error()->code;
                outcome.errorMessage = response.error()->message;
                finishAttempt(outcome.cancelled ? ModelExecutionStatus::Cancelled
                                                : ModelExecutionStatus::Failed,
                    {}, {}, std::nullopt, outcome.errorCode, outcome.errorMessage);
                if (outcome.cancelled) return outcome;
            }

            const bool hasAnotherPrimary = !target.fallback
                && index + 1 < primaryAttempts;
            if (hasAnotherPrimary && !retryable(outcome.errorCode)) {
                index = primaryAttempts - 1;
            }
        }
        return outcome;
    };

    auto persistCompletionSummary = [&](const std::string& summary) {
        if (artifactService_ == nullptr) return;
        static_cast<void>(artifactService_->createDiagnosticArtifact(
            context.run.id, std::nullopt, ArtifactKind::Report,
            "completion-summary.md", "text/markdown", summary));
    };

    while (!pending.empty()) {
        if (stopToken.stop_requested()) {
            for (const auto& id : pending) {
                const auto stage = stagesByType.find(id);
                if (stage != stagesByType.end()) {
                    eventSink.publish(stageEvent(RunEventType::StageCancelled,
                        context.run.id, stage->second.id,
                        static_cast<double>(completed.size()) / totalTasks,
                        "Run cancelled before task execution"));
                }
            }
            return {false, RunStatus::Cancelled, "Execution graph cancelled", totalUsage};
        }

        std::vector<std::string> ready;
        for (const auto& id : pending) {
            const auto& assignment = byId.at(id);
            if (std::all_of(assignment.dependencies.begin(), assignment.dependencies.end(),
                    [&](const auto& dependency) { return completed.contains(dependency); })) {
                ready.push_back(id);
            }
        }
        std::sort(ready.begin(), ready.end(), [&](const auto& left, const auto& right) {
            return byId.at(left).position < byId.at(right).position;
        });
        if (ready.empty()) {
            return {false, RunStatus::Failed,
                "Execution graph has unresolved dependencies", totalUsage};
        }

        // Workspace proposal tasks already return validated, structured
        // Backend state. Re-sending those results to the Architect model is
        // redundant in the normal conflict-free case and was the largest
        // single cost/latency item in the reproduced failure. Keep the
        // declared stage visible for an honest persisted timeline, but mark
        // it Skipped and finish from a deterministic Backend summary.
        if (skipProviderSynthesis) {
            const auto synthesis = std::find(ready.begin(), ready.end(), "synthesis");
            if (synthesis != ready.end()) {
                const auto& stage = stagesByType.at("synthesis");
                pending.erase("synthesis");
                skipped.insert("synthesis");
                eventSink.publish(stageEvent(RunEventType::StageSkipped,
                    context.run.id, stage.id,
                    static_cast<double>(completed.size() + skipped.size()) / totalTasks,
                    "Provider synthesis skipped: structured proposal results require no reconciliation"));
                ready.erase(synthesis);
                if (ready.empty()) continue;
            }
        }
        if (ready.size() > parallelism) ready.resize(parallelism);

        std::vector<std::pair<std::string, std::future<TaskOutcome>>> active;
        for (const auto& id : ready) {
            const auto& assignment = byId.at(id);
            const auto& stage = stagesByType.at(id);
            eventSink.publish(stageEvent(RunEventType::StageStarted,
                context.run.id, stage.id,
                static_cast<double>(completed.size()) / totalTasks,
                stage.title + " started"));
            const std::string_view hiddenDependency = allocations.contains(id)
                ? std::string_view {architect.stageType} : std::string_view {};
            const std::string inputs = dependencyContext(assignment, outputs, hiddenDependency);
            active.emplace_back(id, std::async(std::launch::async,
                [&, assignment, inputs] { return executeTask(assignment, inputs); }));
            pending.erase(id);
        }

        bool batchFailed = false;
        bool batchCancelled = false;
        std::string failureMessage;
        for (auto& [id, future] : active) {
            TaskOutcome outcome;
            try {
                outcome = future.get();
            } catch (const std::exception& exception) {
                outcome.errorCode = "run.task_exception";
                outcome.errorMessage = exception.what();
            } catch (...) {
                outcome.errorCode = "run.task_exception";
                outcome.errorMessage = "Unknown task exception";
            }
            if (outcome.success && architectMustAllocate
                    && id == architect.stageType) {
                if (!outcome.manifest.has_value()) {
                    outcome.success = false;
                    outcome.errorCode = "run.architect_manifest_missing";
                    outcome.errorMessage =
                        "Architect completed without a validated ownership manifest";
                } else {
                    parallelism = std::min<std::size_t>(
                        parallelism, outcome.manifest->requestedMaxParallelism);
                    for (const auto& task : outcome.manifest->tasks) {
                        allocations.emplace(task.id, task);
                        auto& worker = byId.at(task.id);
                        worker.dependencies.clear();
                        worker.dependencies.push_back(architect.stageType);
                        worker.dependencies.insert(worker.dependencies.end(),
                            task.dependencies.begin(), task.dependencies.end());
                    }
                }
            }
            taskOutcomes[id] = outcome;
            // Provider usage is real spend even when the task fails or is
            // cancelled. Counting only successful tasks would hide the cost
            // of rejected proposals after this fail-closed fix.
            mergeAccumulatedUsage(totalUsage, outcome.usage);
            const auto& stage = stagesByType.at(id);
            if (outcome.success) {
                outputs[id] = std::move(outcome.output);
                completed.insert(id);
                eventSink.publish(stageEvent(RunEventType::StageCompleted,
                    context.run.id, stage.id,
                    static_cast<double>(completed.size()) / totalTasks,
                    stage.title + " completed", outcome.usage));
            } else if (outcome.cancelled) {
                batchCancelled = true;
                failureMessage = outcome.errorMessage;
                eventSink.publish(stageEvent(RunEventType::StageCancelled,
                    context.run.id, stage.id,
                    static_cast<double>(completed.size()) / totalTasks,
                    outcome.errorMessage, outcome.usage));
            } else {
                batchFailed = true;
                failureMessage = outcome.errorCode + ": " + outcome.errorMessage;
                eventSink.publish(stageEvent(RunEventType::StageFailed,
                    context.run.id, stage.id,
                    static_cast<double>(completed.size()) / totalTasks,
                    failureMessage, outcome.usage));
            }
        }

        if (batchFailed || batchCancelled) {
            for (const auto& id : pending) {
                const auto& stage = stagesByType.at(id);
                eventSink.publish(stageEvent(
                    batchCancelled ? RunEventType::StageCancelled : RunEventType::StageSkipped,
                    context.run.id, stage.id,
                    static_cast<double>(completed.size()) / totalTasks,
                    batchCancelled ? "Run cancelled before task execution"
                                   : "Skipped because a dependency failed"));
            }
            const RunStatus finalStatus =
                batchCancelled ? RunStatus::Cancelled : RunStatus::Failed;
            const std::string summary = deterministicRunSummary(
                assignments, taskOutcomes, finalStatus, failureMessage,
                true, totalUsage);
            persistCompletionSummary(summary);
            return {false, finalStatus, failureMessage, totalUsage, summary};
        }
    }

    // graphAssignments() appends the "synthesis" stage whenever a lead
    // architect exists alongside at least one other agent, so a successful
    // multi-agent run always has its output here. A lone-architect run has
    // no synthesis stage at all; its own output is the final result.
    const auto synthesisOutput = outputs.find("synthesis");
    if (synthesisOutput != outputs.end()) {
        return {true, RunStatus::Completed,
            "Execution graph and final synthesis completed", totalUsage,
            synthesisOutput->second};
    }

    if (skipProviderSynthesis) {
        constexpr std::string_view completionMessage =
            "Execution graph completed; provider synthesis skipped for structured proposal results";
        const std::string summary = deterministicRunSummary(
            assignments, taskOutcomes, RunStatus::Completed, completionMessage,
            true, totalUsage);
        persistCompletionSummary(summary);
        return {true, RunStatus::Completed, std::string(completionMessage),
            totalUsage, summary};
    }

    return {true, RunStatus::Completed,
        "Execution graph completed", totalUsage, outputs.at(architect.stageType)};
}

} // namespace ai::application
