#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/RunEvent.hpp"

#include <stop_token>
#include <vector>

namespace ai::application {

// Stable workflow ids. Not an enum: workflowId is a persisted, externally
// visible string (Run::workflowId, ExecutionProfile::workflowId), so new
// production workflows can be added without a recompile-only change here.
inline constexpr const char* kMockWorkflowId = "mock-sequential-v1";
inline constexpr const char* kExecutionProfileWorkflowId = "execution-profile-v1";
// Phase 5 (REMAINING_GAPS_EXECUTION_PLAN.md): the inspect-only-planner +
// manifest-executor workflow, wired beside the two above rather than
// replacing either. Selected the same way as kExecutionProfileWorkflowId -
// an ExecutionProfile whose own workflowId is this constant, carrying
// exactly one StageModelAssignment (see ManifestWorkflowRunner).
inline constexpr const char* kExecutionManifestWorkflowId = "execution-manifest-v1";

// What RunManager::enqueue()/enqueueWithPlan() persists onto the new Run and
// later uses to find the runner that must execute it. assignments is empty
// for the Mock workflow; for a production run it is the immutable snapshot
// captured at resolve time (see ApplicationTypes.hpp's
// Run::assignmentSnapshot) - defined here (not in IWorkflowResolver.hpp)
// because IWorkflowRunner::createStages needs it too, and IWorkflowResolver
// already depends on IWorkflowRunner, not the other way around.
struct ResolvedExecutionPlan {
    std::string workflowId;
    std::optional<ExecutionProfileId> executionProfileId;
    std::vector<StageModelAssignment> assignments;
    std::uint32_t maxParallelism {4};
    ProviderExecutionBudgetPolicy providerBudget;
};

// Immutable snapshot supplied to a runner after RunManager has persisted the
// run and its initial Pending stages.
struct WorkflowRunContext {
    Run run;
    Project project;
    std::vector<RunStage> stages;
};

struct WorkflowResult {
    bool success {false};
    RunStatus finalStatus {RunStatus::Failed};
    std::string message;
    models::TokenUsage usage;

    // The synthesis/last-stage output text a successful run produced, or a
    // deterministic Backend completion summary. A failed production workflow
    // may also return its deterministic diagnostic summary here; RunManager
    // records Project Memory only for Completed runs, so failure evidence does
    // not pollute future model context.
    // Declared last, with an empty default, so every existing four-element
    // aggregate initializer ({success, status, message, usage}) keeps
    // compiling unchanged. ProjectMemoryService::recordRunOutcome (Task B of
    // PROJECT_MEMORY_PLAN.md) is the only consumer of this field today.
    std::string finalOutput;
};

/** Provider-neutral execution boundary used by RunManager. */
class IWorkflowRunner {
public:
    virtual ~IWorkflowRunner() = default;
    // Declares stages before execution so RunsPage can display the full
    // timeline while the run is still queued. plan is the same
    // ResolvedExecutionPlan RunManager is about to persist onto the Run;
    // MockWorkflowRunner ignores it (its four stages are fixed), while
    // ExecutionProfileWorkflowRunner derives one Pending stage per
    // plan.assignments entry, in canonical stage-type order.
    [[nodiscard]] virtual std::vector<RunStage> createStages(
        const RunId& runId, const ResolvedExecutionPlan& plan) const = 0;
    virtual WorkflowResult run(
        const WorkflowRunContext& context,
        std::stop_token stopToken,
        IRunEventSink& eventSink) = 0;
};

} // namespace ai::application
