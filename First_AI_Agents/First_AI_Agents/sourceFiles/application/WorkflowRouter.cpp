#include "ai/application/WorkflowRouter.hpp"

#include "ai/application/ExecutionAssignmentValidation.hpp"

#include <algorithm>
#include <utility>

namespace ai::application {

namespace {

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

} // namespace

WorkflowRouter::WorkflowRouter(
    IExecutionProfileRepository& profiles,
    IProviderRepository& providers,
    IModelRepository& models)
    : profiles_(profiles)
    , providers_(providers)
    , models_(models)
{
}

void WorkflowRouter::registerRunner(std::string workflowId, IWorkflowRunner& runner)
{
    // Registration happens once, during composition-root construction,
    // before any RunManager built on this router starts its worker thread -
    // no concurrent access is possible yet, so no lock is needed here.
    runners_[std::move(workflowId)] = &runner;
}

common::Result<ResolvedExecutionPlan> WorkflowRouter::resolvePlan(const StartRunRequest& request) const
{
    if (request.maxParallelism == 0 || request.maxParallelism > 32) {
        return common::Result<ResolvedExecutionPlan>::failure(error(
            "run.invalid_parallelism", "Run parallelism must be between 1 and 32"));
    }
    // Explicit limits belong to a per-Run roster. A saved profile carries its own, and a Mock Run
    // spends nothing: accepting limits there would silently ignore what the caller asked for.
    if (hasExplicitProviderBudgetLimits(request.providerBudget)
            && request.assignmentOverrides.empty()) {
        return common::Result<ResolvedExecutionPlan>::failure(error(
            "run.provider_budget_requires_roster",
            "Provider limits can be set only for a per-Run roster; a saved execution profile "
            "carries its own limits"));
    }
    if (!request.assignmentOverrides.empty()) {
        if (request.executionProfileId.has_value()) {
            return common::Result<ResolvedExecutionPlan>::failure(error(
                "run.ambiguous_execution_plan",
                "Choose either an execution profile or a per-run agent roster"));
        }
        const auto validated = validateRunRoster(request.assignmentOverrides, providers_, models_);
        if (!validated.success()) {
            return common::Result<ResolvedExecutionPlan>::failure(*validated.error());
        }
        // AG-17.6: a per-Run roster may execute through the converged Manifest
        // workflow (the lead plans, available workers execute the frozen tasks) or,
        // as before, through the legacy fixed-stage roster runner. Only the Manifest
        // workflow is ever selected explicitly; every other workflowId keeps the
        // historical roster behaviour.
        const bool manifestRoster = request.workflowId == kExecutionManifestWorkflowId;
        if (manifestRoster) {
            const auto rosterValidated = validateManifestRoster(request.assignmentOverrides);
            if (!rosterValidated.success()) {
                return common::Result<ResolvedExecutionPlan>::failure(*rosterValidated.error());
            }
        }
        ResolvedExecutionPlan plan;
        plan.workflowId = manifestRoster ? kExecutionManifestWorkflowId : kExecutionProfileWorkflowId;
        plan.assignments = request.assignmentOverrides;
        plan.maxParallelism = request.maxParallelism;
        // A per-Run roster gets the Backend defaults with the turn limit in automatic mode
        // (see ProviderExecutionBudgetPolicy::autoExtendTurns) unless the caller set limits
        // explicitly; an explicit maxTurns pins the turn limit. Out-of-range values are refused
        // here, before any Run exists.
        const auto rosterBudget = resolveProviderExecutionBudget(request.providerBudget);
        if (!rosterBudget.success()) {
            return common::Result<ResolvedExecutionPlan>::failure(*rosterBudget.error());
        }
        plan.providerBudget = rosterBudget.value();
        std::sort(plan.assignments.begin(), plan.assignments.end(),
            [](const auto& left, const auto& right) { return left.position < right.position; });
        return common::Result<ResolvedExecutionPlan>::ok(std::move(plan));
    }

    if (request.executionProfileId.has_value()) {
        // Explicit selection always wins and always means production
        // execution - workflowId is never itself treated as a profile id.
        const auto profile = profiles_.findById(*request.executionProfileId);
        if (!profile.success()) {
            return common::Result<ResolvedExecutionPlan>::failure(*profile.error());
        }
        if (!profile.value().has_value()) {
            return common::Result<ResolvedExecutionPlan>::failure(
                error("execution_profile.not_found", "Execution profile not found"));
        }
        if (!profile.value()->enabled) {
            return common::Result<ResolvedExecutionPlan>::failure(
                error("execution_profile.disabled", "Execution profile is disabled"));
        }
        const auto validated = validateExecutionAssignments(profile.value()->assignments, providers_, models_);
        if (!validated.success()) {
            return common::Result<ResolvedExecutionPlan>::failure(*validated.error());
        }
        const auto workflowValidated = validateExecutionProfileWorkflow(
            profile.value()->workflowId, profile.value()->assignments);
        if (!workflowValidated.success()) {
            return common::Result<ResolvedExecutionPlan>::failure(*workflowValidated.error());
        }
        const auto budget = resolveProviderExecutionBudget(profile.value()->providerBudget);
        if (!budget.success()) {
            return common::Result<ResolvedExecutionPlan>::failure(*budget.error());
        }

        ResolvedExecutionPlan plan;
        // Every profile ever selects a production runner, historically
        // always kExecutionProfileWorkflowId regardless of the profile's own
        // workflowId string (many existing fixtures/tests set it to an
        // arbitrary placeholder and rely on exactly this). The manifest
        // workflow is the one deliberate exception: a profile that actually
        // declares kExecutionManifestWorkflowId routes to that runner
        // instead, so a manifest-workflow profile is selectable without
        // widening this to trust every profile's own field, which would
        // silently misroute the many fixtures above.
        plan.workflowId = profile.value()->workflowId == kExecutionManifestWorkflowId
            ? kExecutionManifestWorkflowId : kExecutionProfileWorkflowId;
        plan.executionProfileId = profile.value()->id;
        plan.assignments = profile.value()->assignments;
        plan.maxParallelism = request.maxParallelism;
        plan.providerBudget = budget.value();
        return common::Result<ResolvedExecutionPlan>::ok(std::move(plan));
    }

    if (request.workflowId == kMockWorkflowId) {
        ResolvedExecutionPlan plan;
        plan.workflowId = kMockWorkflowId;
        plan.maxParallelism = request.maxParallelism;
        plan.providerBudget = ProviderExecutionBudgetPolicy {};
        return common::Result<ResolvedExecutionPlan>::ok(std::move(plan));
    }

    // No explicit profile and a non-Mock workflowId: the *global* default
    // profile (ExecutionProfileService guarantees at most one) must exist,
    // be enabled, and itself target this exact workflowId - otherwise this
    // request has no valid production plan and must fail rather than guess.
    const auto allProfiles = profiles_.listAll();
    if (!allProfiles.success()) {
        return common::Result<ResolvedExecutionPlan>::failure(*allProfiles.error());
    }
    const auto defaultIt = std::find_if(
        allProfiles.value().begin(), allProfiles.value().end(),
        [](const ExecutionProfile& candidate) { return candidate.isDefault; });
    if (defaultIt == allProfiles.value().end() || !defaultIt->enabled
            || defaultIt->workflowId != request.workflowId) {
        return common::Result<ResolvedExecutionPlan>::failure(error(
            "execution_profile.no_default_for_workflow",
            "No enabled default execution profile is configured for this workflow"));
    }
    const auto validated = validateExecutionAssignments(defaultIt->assignments, providers_, models_);
    if (!validated.success()) {
        return common::Result<ResolvedExecutionPlan>::failure(*validated.error());
    }
    const auto workflowValidated = validateExecutionProfileWorkflow(
        defaultIt->workflowId, defaultIt->assignments);
    if (!workflowValidated.success()) {
        return common::Result<ResolvedExecutionPlan>::failure(*workflowValidated.error());
    }
    const auto budget = resolveProviderExecutionBudget(defaultIt->providerBudget);
    if (!budget.success()) {
        return common::Result<ResolvedExecutionPlan>::failure(*budget.error());
    }

    ResolvedExecutionPlan plan;
    // request.workflowId already equals defaultIt->workflowId here (checked
    // above), so this mirrors the explicit-selection branch's precedence.
    plan.workflowId = request.workflowId == kExecutionManifestWorkflowId
        ? kExecutionManifestWorkflowId : kExecutionProfileWorkflowId;
    plan.executionProfileId = defaultIt->id;
    plan.assignments = defaultIt->assignments;
    plan.maxParallelism = request.maxParallelism;
    plan.providerBudget = budget.value();
    return common::Result<ResolvedExecutionPlan>::ok(std::move(plan));
}

common::Result<std::reference_wrapper<IWorkflowRunner>> WorkflowRouter::runnerFor(
    const std::string& workflowId)
{
    const auto it = runners_.find(workflowId);
    if (it == runners_.end()) {
        return common::Result<std::reference_wrapper<IWorkflowRunner>>::failure(
            error("workflow.unsupported", "Unsupported workflow id"));
    }
    return common::Result<std::reference_wrapper<IWorkflowRunner>>::ok(std::ref(*it->second));
}

} // namespace ai::application
