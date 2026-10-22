#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/IWorkflowRunner.hpp"

#include <functional>
#include <string>

namespace ai::application {

// kMockWorkflowId, kExecutionProfileWorkflowId, and ResolvedExecutionPlan
// are declared in IWorkflowRunner.hpp (included above) because
// IWorkflowRunner::createStages needs ResolvedExecutionPlan too.

/**
 * Provider-neutral routing boundary RunManager uses instead of owning one
 * fixed IWorkflowRunner. Resolution is a pure decision (no side effects on
 * the runner registry); runnerFor() is the only place a caller obtains a
 * runner reference.
 *
 * Lifetime contract: every IWorkflowRunner& this returns is owned by
 * BackendRuntime (or whatever composition root constructed the resolver's
 * implementation) and is guaranteed to outlive both the resolver and the
 * RunManager that holds it - registered runners are never swapped or
 * destroyed while a RunManager referencing this resolver is alive. This is
 * what makes it safe for RunManager's worker thread to call run() on a
 * runner obtained this way without any additional synchronization for the
 * runner's own lifetime.
 */
class IWorkflowResolver {
public:
    virtual ~IWorkflowResolver() = default;

    // Decides which workflow a StartRunRequest should execute against and
    // captures the immutable assignment snapshot to persist. Returns a
    // typed failure (never throws) for: explicit-but-unknown/disabled
    // executionProfileId, an invalid assignment (see
    // ExecutionAssignmentValidation.hpp), or "no enabled default profile for
    // this workflowId" when no explicit profile was given and workflowId
    // is not the Mock workflow.
    [[nodiscard]] virtual common::Result<ResolvedExecutionPlan> resolvePlan(
        const StartRunRequest& request) const = 0;

    // Looks up the runner registered for a resolved (or previously
    // persisted, for retry/resume) workflowId. Fails with
    // "workflow.unsupported" for an unknown id, or with
    // "provider_runtime.unavailable" specifically when workflowId names the
    // production execution-profile workflow but no real provider adapter is
    // linked into this build - see BackendRuntime.
    virtual common::Result<std::reference_wrapper<IWorkflowRunner>> runnerFor(
        const std::string& workflowId) = 0;
};

/**
 * Minimal IWorkflowResolver that always resolves to one fixed runner under
 * one fixed workflow id, with no execution-profile catalog involved at all.
 * Intended for tests and composition roots that only exercise the Mock
 * workflow and would otherwise have to wire an unused
 * IExecutionProfileRepository/IProviderRepository/IModelRepository just to
 * build a full WorkflowRouter.
 */
class SingleRunnerResolver final : public IWorkflowResolver {
public:
    explicit SingleRunnerResolver(IWorkflowRunner& runner, std::string workflowId = kMockWorkflowId)
        : runner_(runner)
        , workflowId_(std::move(workflowId))
    {
    }

    [[nodiscard]] common::Result<ResolvedExecutionPlan> resolvePlan(
        const StartRunRequest& request) const override
    {
        if (request.executionProfileId.has_value() || request.workflowId != workflowId_) {
            return common::Result<ResolvedExecutionPlan>::failure(
                {"workflow.unsupported", "This resolver only supports its single registered workflow"});
        }
        ResolvedExecutionPlan plan;
        plan.workflowId = workflowId_;
        return common::Result<ResolvedExecutionPlan>::ok(std::move(plan));
    }

    common::Result<std::reference_wrapper<IWorkflowRunner>> runnerFor(const std::string& workflowId) override
    {
        if (workflowId != workflowId_) {
            return common::Result<std::reference_wrapper<IWorkflowRunner>>::failure(
                {"workflow.unsupported", "Unsupported workflow id"});
        }
        return common::Result<std::reference_wrapper<IWorkflowRunner>>::ok(std::ref(runner_));
    }

private:
    IWorkflowRunner& runner_;
    std::string workflowId_;
};

} // namespace ai::application
