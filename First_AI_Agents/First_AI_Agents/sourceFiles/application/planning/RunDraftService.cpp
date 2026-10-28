#include "ai/application/planning/RunDraftService.hpp"

#include "ai/application/execution/IWorkflowRunner.hpp"
#include "ai/application/IdGenerator.hpp"
#include "ai/application/execution/RunInterfaceContract.hpp"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace ai::application {

namespace {

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

// Explicit limits are meaningful only for a per-Run roster, and must be valid values: a draft
// that could never start would otherwise be saved and fail later, away from the editor.
common::Result<void> validateDraftProviderBudget(
    RunLaunchMode launchMode, const ProviderExecutionBudgetOverrides& providerBudget)
{
    if (hasExplicitProviderBudgetLimits(providerBudget)
            && launchMode != RunLaunchMode::CustomRoster) {
        return common::Result<void>::failure(error(
            "run_draft.provider_budget_requires_roster",
            "Provider limits can be saved only with a per-Run roster"));
    }
    const auto resolved = resolveProviderExecutionBudget(providerBudget);
    if (!resolved.success()) return common::Result<void>::failure(*resolved.error());
    return common::Result<void>::ok();
}

// PC-2: only the Manifest workflow has worker prompts to carry interface contracts. RunManager enforces that on
// the effective workflow it has already resolved when a Run starts. A draft cannot resolve a saved profile's own
// workflow, so this is a stricter authoring-time rule: contracts are saved only with an explicit Manifest workflow
// AND a per-Run roster (CustomRoster), the one combination that is certain to resolve to the Manifest workflow
// and the only one the New Run dialog shows contracts for. A draft the dialog would hide or drop the contracts of,
// or that could fail to start with them, is refused here instead of being saved.
common::Result<std::vector<RunInterfaceContract>> normalizeDraftContracts(
    std::vector<RunInterfaceContract> contracts, RunLaunchMode launchMode, const std::string& workflowId)
{
    auto normalized = normalizeRunInterfaceContracts(std::move(contracts));
    if (!normalized.success()) return normalized;
    if (!normalized.value().empty()
            && (launchMode != RunLaunchMode::CustomRoster || workflowId != kExecutionManifestWorkflowId)) {
        return common::Result<std::vector<RunInterfaceContract>>::failure(error(
            "run_contracts.requires_manifest_workflow",
            "Interface contracts are delivered only by the Manifest workflow; choose a roster Run on the "
            "Manifest workflow or remove the contracts"));
    }
    return normalized;
}

} // namespace

RunDraftService::RunDraftService(IRunDraftRepository& drafts)
    : drafts_(drafts)
{
}

common::Result<std::vector<RunDraft>> RunDraftService::listDrafts() const
{
    return drafts_.listAll();
}

common::Result<std::optional<RunDraft>> RunDraftService::findDraft(const RunDraftId& id) const
{
    return drafts_.findById(id);
}

common::Result<std::optional<RunDraftGroup>> RunDraftService::findGroup(const RunDraftGroupId& id) const
{
    return drafts_.findGroupById(id);
}

common::Result<std::vector<RunDraft>> RunDraftService::listGroupDrafts(const RunDraftGroupId& id) const
{
    return drafts_.listGroupDrafts(id);
}

common::Result<RunDraftId> RunDraftService::createDraft(CreateRunDraftRequest request)
{
    if (request.name.empty()) {
        return common::Result<RunDraftId>::failure(
            error("run_draft.empty_name", "Run draft name cannot be empty"));
    }
    if (request.projectId.empty()) {
        return common::Result<RunDraftId>::failure(
            error("run_draft.empty_project_id", "Run draft projectId cannot be empty"));
    }
    const auto budgetValid = validateDraftProviderBudget(request.launchMode, request.providerBudget);
    if (!budgetValid.success()) {
        return common::Result<RunDraftId>::failure(*budgetValid.error());
    }
    auto contracts = normalizeDraftContracts(
        std::move(request.interfaceContracts), request.launchMode, request.workflowId);
    if (!contracts.success()) {
        return common::Result<RunDraftId>::failure(*contracts.error());
    }

    RunDraft draft;
    draft.id = generateUuid();
    draft.name = std::move(request.name);
    draft.projectId = std::move(request.projectId);
    draft.title = std::move(request.title);
    draft.task = std::move(request.task);
    draft.workflowId = std::move(request.workflowId);
    draft.launchMode = request.launchMode;
    draft.executionProfileId = std::move(request.executionProfileId);
    draft.roster = std::move(request.roster);
    draft.maxParallelism = request.maxParallelism;
    draft.workspacePath = std::move(request.workspacePath);
    draft.verificationRepairPolicy = request.verificationRepairPolicy;
    draft.providerBudget = request.providerBudget;
    draft.interfaceContracts = std::move(contracts.value());
    draft.createdAt = std::chrono::system_clock::now();
    draft.updatedAt = draft.createdAt;

    const auto inserted = drafts_.insert(std::move(draft));
    if (!inserted.success()) {
        return common::Result<RunDraftId>::failure(*inserted.error());
    }
    return common::Result<RunDraftId>::ok(inserted.value().id);
}

common::Result<void> RunDraftService::updateDraft(UpdateRunDraftRequest request)
{
    if (request.name.empty()) {
        return common::Result<void>::failure(
            error("run_draft.empty_name", "Run draft name cannot be empty"));
    }
    if (request.projectId.empty()) {
        return common::Result<void>::failure(
            error("run_draft.empty_project_id", "Run draft projectId cannot be empty"));
    }
    const auto budgetValid = validateDraftProviderBudget(request.launchMode, request.providerBudget);
    if (!budgetValid.success()) {
        return common::Result<void>::failure(*budgetValid.error());
    }
    auto contracts = normalizeDraftContracts(
        std::move(request.interfaceContracts), request.launchMode, request.workflowId);
    if (!contracts.success()) {
        return common::Result<void>::failure(*contracts.error());
    }

    const auto found = drafts_.findById(request.id);
    if (!found.success()) {
        return common::Result<void>::failure(*found.error());
    }
    if (!found.value().has_value()) {
        return common::Result<void>::failure(
            error("run_draft.not_found", "Run draft not found"));
    }

    RunDraft draft = *found.value();
    draft.name = std::move(request.name);
    draft.projectId = std::move(request.projectId);
    draft.title = std::move(request.title);
    draft.task = std::move(request.task);
    draft.workflowId = std::move(request.workflowId);
    draft.launchMode = request.launchMode;
    draft.executionProfileId = std::move(request.executionProfileId);
    draft.roster = std::move(request.roster);
    draft.maxParallelism = request.maxParallelism;
    draft.workspacePath = std::move(request.workspacePath);
    draft.verificationRepairPolicy = request.verificationRepairPolicy;
    draft.providerBudget = request.providerBudget;
    draft.interfaceContracts = std::move(contracts.value());
    draft.updatedAt = std::chrono::system_clock::now();

    const auto updated = drafts_.update(std::move(draft));
    if (!updated.success()) {
        return common::Result<void>::failure(*updated.error());
    }
    return common::Result<void>::ok();
}

common::Result<void> RunDraftService::removeDraft(const RunDraftId& id)
{
    return drafts_.remove(id);
}

} // namespace ai::application
