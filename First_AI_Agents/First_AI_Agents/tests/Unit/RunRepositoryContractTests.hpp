#pragma once

#include "ai/application/IRunRepository.hpp"
#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/execution/LegacyConversionRecord.hpp"
#include "ai/application/execution/LegacyRosterAdapter.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

namespace ai::tests {

inline void requireRepositoryContract(bool condition, const char* message)
{
    // Contract checks stay active in Release so a future adapter cannot appear
    // conformant merely because NDEBUG disabled assertions.
    if (!condition) {
        throw std::runtime_error(message);
    }
}

inline application::Run contractRun(
    const std::string& prefix,
    const application::ProjectId& projectId)
{
    application::Run run;
    run.id = prefix + "-run";
    run.projectId = projectId;
    run.title = "Repository contract";
    run.task = "Persist the complete aggregate";
    run.workflowId = "mock-sequential-v1";
    run.status = application::RunStatus::Queued;
    run.createdAt = std::chrono::system_clock::now();
    return run;
}

inline std::vector<application::RunStage> contractStages(
    const std::string& prefix,
    const application::RunId& runId)
{
    std::vector<application::RunStage> stages;
    for (int index = 0; index < 3; ++index) {
        application::RunStage stage;
        stage.id = prefix + "-stage-" + std::to_string(index);
        stage.runId = runId;
        stage.type = "contract-" + std::to_string(index);
        stage.title = "Stage " + std::to_string(index);
        stage.description = "Ordered repository contract stage";
        stages.push_back(std::move(stage));
    }
    return stages;
}

inline bool containsRun(
    const std::vector<application::Run>& runs,
    const application::RunId& runId)
{
    return std::any_of(runs.begin(), runs.end(), [&](const application::Run& run) {
        return run.id == runId;
    });
}

/**
 * Provider-neutral behavior suite shared by every IRunRepository adapter.
 *
 * A new SQL Server/PostgreSQL adapter should invoke this exact function with
 * its own isolated repository fixture before it is accepted by a composition
 * root. Adapter-specific migration tests remain separate.
 */
inline void runRunRepositoryContractTests(
    application::IRunRepository& repository,
    const std::string& prefix,
    const application::ProjectId& projectId)
{
    using namespace application;

    Run run = contractRun(prefix, projectId);
    run.providerBudget.maxTurns = 9;
    run.providerBudget.maxInputTokens = 123'456;
    const auto initialStages = contractStages(prefix, run.id);
    const auto inserted = repository.insertRunWithStages(run, initialStages);
    const std::string insertMessage = inserted.success() ? std::string {}
        : "Atomic Run+Stages insert failed: " + inserted.error()->code
            + " - " + inserted.error()->message;
    requireRepositoryContract(inserted.success(), insertMessage.c_str());

    const auto found = repository.findRun(run.id);
    requireRepositoryContract(
        found.success() && found.value().has_value(),
        "Atomically inserted run was not readable");
    requireRepositoryContract(found.value()->task == run.task, "Run task changed during insert");

    const auto storedStages = repository.listStages(run.id);
    requireRepositoryContract(
        storedStages.success() && storedStages.value().size() == initialStages.size(),
        "Atomically inserted stages were not readable");
    for (std::size_t index = 0; index < initialStages.size(); ++index) {
        requireRepositoryContract(
            storedStages.value()[index].id == initialStages[index].id,
            "Repository did not preserve declared stage order");
    }

    const auto filtered = repository.listRuns(projectId);
    requireRepositoryContract(
        filtered.success() && containsRun(filtered.value(), run.id),
        "Project run filter omitted the inserted aggregate");
    const auto unfinished = repository.listUnfinishedRuns();
    requireRepositoryContract(
        unfinished.success() && containsRun(unfinished.value(), run.id),
        "Queued aggregate was omitted from unfinished runs");

    // Duplicate aggregate creation may not overwrite the already committed
    // stage snapshot.
    auto duplicateStages = initialStages;
    duplicateStages.front().title = "Must not overwrite";
    const auto duplicateRun = repository.insertRunWithStages(run, duplicateStages);
    requireRepositoryContract(!duplicateRun.success(), "Duplicate run id was accepted");
    const auto afterDuplicateRun = repository.listStages(run.id);
    requireRepositoryContract(
        afterDuplicateRun.success()
            && afterDuplicateRun.value().front().title == initialStages.front().title,
        "Duplicate aggregate attempt changed committed stages");

    // A stage belonging to another run is rejected before any aggregate state
    // becomes visible.
    Run mismatched = contractRun(prefix + "-mismatch", projectId);
    auto mismatchedStages = contractStages(prefix + "-mismatch", mismatched.id);
    mismatchedStages.back().runId = "another-run";
    const auto mismatchResult = repository.insertRunWithStages(mismatched, mismatchedStages);
    requireRepositoryContract(!mismatchResult.success(), "Mismatched stage owner was accepted");
    const auto missingMismatch = repository.findRun(mismatched.id);
    requireRepositoryContract(
        missingMismatch.success() && !missingMismatch.value().has_value(),
        "Rejected stage owner left a partial run row");

    // Duplicate stage ids fail after SQLite has begun its transaction. Both
    // SQLite rollback and in-memory prevalidation must leave no partial run.
    Run duplicateStageRun = contractRun(prefix + "-duplicate-stage", projectId);
    auto duplicateIds = contractStages(prefix + "-duplicate-stage", duplicateStageRun.id);
    duplicateIds[1].id = duplicateIds[0].id;
    const auto duplicateStageResult = repository.insertRunWithStages(duplicateStageRun, duplicateIds);
    requireRepositoryContract(!duplicateStageResult.success(), "Duplicate stage ids were accepted");
    const auto missingDuplicateStageRun = repository.findRun(duplicateStageRun.id);
    requireRepositoryContract(
        missingDuplicateStageRun.success() && !missingDuplicateStageRun.value().has_value(),
        "Failed stage insert left a partial run row");
    const auto missingDuplicateStages = repository.listStages(duplicateStageRun.id);
    requireRepositoryContract(
        missingDuplicateStages.success() && missingDuplicateStages.value().empty(),
        "Failed aggregate insert left partial stages");

    Run invalidProgress = contractRun(prefix + "-invalid-progress", projectId);
    invalidProgress.progress = 1.5;
    const auto invalidProgressResult = repository.insertRunWithStages(
        invalidProgress,
        contractStages(prefix + "-invalid-progress", invalidProgress.id));
    requireRepositoryContract(!invalidProgressResult.success(), "Invalid run progress was accepted");
    const auto missingInvalidProgress = repository.findRun(invalidProgress.id);
    requireRepositoryContract(
        missingInvalidProgress.success() && !missingInvalidProgress.value().has_value(),
        "Invalid aggregate left a partial run row");

    Run invalidDuration = contractRun(prefix + "-invalid-cache-duration", projectId);
    invalidDuration.usage.inputTokens = 30;
    invalidDuration.usage.cacheCreationInputTokens = 30;
    invalidDuration.usage.cacheCreationDuration.known = true;
    invalidDuration.usage.cacheCreationDuration.fiveMinuteTokens = 10;
    invalidDuration.usage.cacheCreationDuration.oneHourTokens = 19;
    const auto invalidDurationResult = repository.insertRunWithStages(
        invalidDuration,
        contractStages(prefix + "-invalid-cache-duration", invalidDuration.id));
    requireRepositoryContract(
        !invalidDurationResult.success()
            && invalidDurationResult.error()->code
                == "cache_duration.partition_total_mismatch",
        "Run repository accepted a duration partition that does not match cache creation");
    const auto missingInvalidDuration = repository.findRun(invalidDuration.id);
    requireRepositoryContract(
        missingInvalidDuration.success() && !missingInvalidDuration.value().has_value(),
        "Rejected duration partition left a partial run row");

    Run invalidCacheBreakdown = contractRun(prefix + "-invalid-cache-breakdown", projectId);
    invalidCacheBreakdown.usage.inputTokens = 10;
    invalidCacheBreakdown.usage.cacheCreationInputTokens = 8;
    invalidCacheBreakdown.usage.cacheReadInputTokens = 8;
    invalidCacheBreakdown.usage.cacheCreationDuration = {true, 3, 5};
    const auto invalidCacheBreakdownResult = repository.insertRunWithStages(
        invalidCacheBreakdown,
        contractStages(prefix + "-invalid-cache-breakdown", invalidCacheBreakdown.id));
    requireRepositoryContract(
        !invalidCacheBreakdownResult.success()
            && invalidCacheBreakdownResult.error()->code == "persistence.invalid_data",
        "Run repository accepted cache subsets larger than the input total");

    Run hiddenDuration = contractRun(prefix + "-hidden-cache-duration", projectId);
    hiddenDuration.usage.inputTokens = 30;
    hiddenDuration.usage.cacheCreationInputTokens = 30;
    hiddenDuration.usage.cacheCreationDuration.fiveMinuteTokens = 30;
    const auto hiddenDurationResult = repository.insertRunWithStages(
        hiddenDuration,
        contractStages(prefix + "-hidden-cache-duration", hiddenDuration.id));
    requireRepositoryContract(
        !hiddenDurationResult.success()
            && hiddenDurationResult.error()->code
                == "cache_duration.detail_without_partition",
        "Run repository accepted duration detail marked as unknown");

    // Both adapters expose the same optimistic-concurrency semantics.
    Run firstWriter = *found.value();
    Run staleWriter = firstWriter;
    firstWriter.title = "Committed revision";
    firstWriter.providerBudget.maxTurns = 1;
    firstWriter.providerBudget.maxInputTokens = 1;
    firstWriter.providerBudgetUsage.turnsStarted = 3;
    firstWriter.providerBudgetUsage.turnsCompleted = 2;
    firstWriter.providerBudgetUsage.inputTokens = 42;
    firstWriter.providerBudgetUsage.cacheCreationInputTokens = 30;
    firstWriter.providerBudgetUsage.cacheCreationDuration = {true, 10, 20};
    // MW-P2: cache-creation/cache-read tokens are mutable cumulative usage,
    // same as input/output tokens - not part of the immutable budget policy.
    firstWriter.usage.inputTokens = 7'500;
    firstWriter.usage.cacheCreationInputTokens = 500;
    firstWriter.usage.cacheReadInputTokens = 7'000;
    firstWriter.usage.cacheCreationDuration.known = true;
    firstWriter.usage.cacheCreationDuration.fiveMinuteTokens = 125;
    firstWriter.usage.cacheCreationDuration.oneHourTokens = 375;
    const auto updated = repository.updateRun(firstWriter);
    requireRepositoryContract(updated.success() && updated.value().revision == firstWriter.revision + 1,
        "Run revision was not incremented");
    requireRepositoryContract(
        updated.value().providerBudget.maxTurns == 9
            && updated.value().providerBudget.maxInputTokens == 123'456,
        "Mutable Run update replaced the immutable provider budget policy");
    requireRepositoryContract(
        updated.value().providerBudgetUsage.turnsStarted == 3
            && updated.value().providerBudgetUsage.turnsCompleted == 2
            && updated.value().providerBudgetUsage.inputTokens == 42
            && updated.value().providerBudgetUsage.cacheCreationInputTokens == 30
            && updated.value().providerBudgetUsage.cacheCreationDuration.known
            && updated.value().providerBudgetUsage.cacheCreationDuration.fiveMinuteTokens == 10
            && updated.value().providerBudgetUsage.cacheCreationDuration.oneHourTokens == 20,
        "Mutable Run update did not persist provider budget usage");
    requireRepositoryContract(
        updated.value().usage.cacheCreationInputTokens == 500
            && updated.value().usage.cacheReadInputTokens == 7'000
            && updated.value().usage.cacheCreationDuration.known
            && updated.value().usage.cacheCreationDuration.fiveMinuteTokens == 125
            && updated.value().usage.cacheCreationDuration.oneHourTokens == 375,
        "Mutable Run update did not persist duration-aware cache token usage");
    auto invalidDurationUpdate = updated.value();
    invalidDurationUpdate.usage.cacheCreationDuration.oneHourTokens = 374;
    const auto rejectedDurationUpdate = repository.updateRun(invalidDurationUpdate);
    requireRepositoryContract(
        !rejectedDurationUpdate.success()
            && rejectedDurationUpdate.error()->code
                == "cache_duration.partition_total_mismatch",
        "Run repository accepted an invalid duration partition during update");
    const auto afterRejectedDurationUpdate = repository.findRun(run.id);
    requireRepositoryContract(
        afterRejectedDurationUpdate.success()
            && afterRejectedDurationUpdate.value()->usage.cacheCreationDuration.oneHourTokens == 375
            && afterRejectedDurationUpdate.value()->revision == updated.value().revision,
        "Rejected duration partition changed the stored Run");
    auto invalidCacheBreakdownUpdate = updated.value();
    invalidCacheBreakdownUpdate.usage.inputTokens = 10;
    invalidCacheBreakdownUpdate.usage.cacheCreationInputTokens = 8;
    invalidCacheBreakdownUpdate.usage.cacheReadInputTokens = 8;
    invalidCacheBreakdownUpdate.usage.cacheCreationDuration = {true, 3, 5};
    const auto rejectedCacheBreakdownUpdate = repository.updateRun(invalidCacheBreakdownUpdate);
    requireRepositoryContract(
        !rejectedCacheBreakdownUpdate.success()
            && rejectedCacheBreakdownUpdate.error()->code == "persistence.invalid_data",
        "Run repository accepted invalid cache subsets during update");
    auto invalidBudgetDurationUpdate = updated.value();
    invalidBudgetDurationUpdate.providerBudgetUsage.cacheCreationDuration.oneHourTokens = 19;
    const auto rejectedBudgetDurationUpdate = repository.updateRun(invalidBudgetDurationUpdate);
    requireRepositoryContract(
        !rejectedBudgetDurationUpdate.success()
            && rejectedBudgetDurationUpdate.error()->code
                == "cache_duration.partition_total_mismatch",
        "Run repository accepted an invalid provider-budget duration partition");
    staleWriter.title = "Stale revision";
    const auto conflict = repository.updateRun(staleWriter);
    requireRepositoryContract(
        !conflict.success() && conflict.error()->code == "run.revision_conflict",
        "Stale run revision was not rejected");

    RunStage changedStage = storedStages.value()[1];
    changedStage.title = "Updated stage";
    changedStage.progress = 0.5;
    const auto stageUpdated = repository.updateStage(changedStage);
    requireRepositoryContract(stageUpdated.success(), "Existing stage update failed");

    auto replacement = contractStages(prefix + "-replacement", run.id);
    replacement.resize(2);
    const auto replaced = repository.replaceStages(run.id, replacement);
    requireRepositoryContract(replaced.success(), "Valid ordered stage replacement failed");
    const auto afterReplacement = repository.listStages(run.id);
    requireRepositoryContract(
        afterReplacement.success() && afterReplacement.value().size() == replacement.size()
            && afterReplacement.value()[0].id == replacement[0].id
            && afterReplacement.value()[1].id == replacement[1].id,
        "Stage replacement did not preserve order");

    auto invalidReplacement = replacement;
    invalidReplacement[1].id = invalidReplacement[0].id;
    const auto rejectedReplacement = repository.replaceStages(run.id, invalidReplacement);
    requireRepositoryContract(!rejectedReplacement.success(), "Invalid stage replacement was accepted");
    const auto afterRejectedReplacement = repository.listStages(run.id);
    requireRepositoryContract(
        afterRejectedReplacement.success()
            && afterRejectedReplacement.value().size() == replacement.size()
            && afterRejectedReplacement.value()[1].id == replacement[1].id,
        "Failed stage replacement changed committed data");
}

// ---------------------------------------------------------------------------------------
// AG-17.7C: the legacy conversion aggregate. Shared by every adapter: Run + stages + roster
// snapshot + the immutable conversion record commit together, or not at all.
// ---------------------------------------------------------------------------------------

// A converted Run (a per-Run Manifest Run, parent = `sourceRunId`) together with the stages the
// Manifest workflow declares and the record certifying exactly its frozen roster.
struct ContractConversion {
    application::Run run;
    std::vector<application::RunStage> stages;
    application::LegacyConversionRecord record;
};

inline std::vector<application::StageModelAssignment> contractLegacyProposalRoster(
    const std::string& providerId, const std::string& modelId)
{
    using namespace application;
    std::vector<StageModelAssignment> roster;
    StageModelAssignment lead;
    lead.stageType = "architect";
    lead.providerId = providerId;
    lead.modelId = modelId;
    lead.timeout = std::chrono::milliseconds(60000);
    lead.role = "Software Architect";
    lead.instructions = "Plan the work.";
    lead.leadArchitect = true;
    lead.maxAttempts = 2;
    lead.position = 0;
    roster.push_back(lead);
    for (std::uint32_t index = 1; index <= 2; ++index) {
        StageModelAssignment worker;
        worker.stageType = "agent-" + std::to_string(index + 1);
        worker.providerId = providerId;
        worker.modelId = modelId;
        worker.timeout = std::chrono::milliseconds(60000);
        worker.role = "Implementation specialist";
        worker.workspaceAccess = true;
        worker.position = index;
        roster.push_back(worker);
    }
    return roster;
}

// `safe` builds the retired read-only lone workspace lead (nothing dropped, safe automatic Retry);
// otherwise the proposal roster with an explicit confirmation.
inline ContractConversion contractConversion(
    const std::string& prefix,
    const application::ProjectId& projectId,
    const std::string& providerId,
    const std::string& modelId,
    const application::RunId& sourceRunId,
    bool safe)
{
    using namespace application;
    LegacyRosterConversionPlan plan;
    if (safe) {
        StageModelAssignment analysis;
        analysis.stageType = "analysis";
        analysis.providerId = providerId;
        analysis.modelId = modelId;
        analysis.timeout = std::chrono::milliseconds(60000);
        analysis.role = "Read-only workspace analysis agent";
        plan = LegacyRosterAdapter::buildConversionPlan(kRetiredReadOnlyAnalysisWorkflowId, {analysis});
    } else {
        plan = LegacyRosterAdapter::buildConversionPlan(
            kExecutionProfileWorkflowId, contractLegacyProposalRoster(providerId, modelId));
    }
    requireRepositoryContract(plan.converts(), "contract fixture: the legacy roster must convert");
    const auto draft = makeLegacyConversionDraft(sourceRunId, plan,
        safe ? LegacyConversionConfirmationMode::NotRequiredSafeRetry
             : LegacyConversionConfirmationMode::ExplicitUser);
    requireRepositoryContract(draft.success(), "contract fixture: the conversion draft must build");

    ContractConversion result;
    result.run.id = prefix + "-converted-run";
    result.run.projectId = projectId;
    result.run.parentRunId = sourceRunId;
    result.run.title = "Converted from a legacy roster";
    result.run.task = "Persist the conversion aggregate";
    result.run.workflowId = kExecutionManifestWorkflowId;
    result.run.assignmentSnapshot = plan.roster;
    result.run.status = RunStatus::Queued;
    result.run.createdAt = std::chrono::system_clock::now();
    result.stages = contractStages(prefix + "-converted", result.run.id);
    const auto confirmedAt = std::chrono::system_clock::now();
    result.record = makeLegacyConversionRecord(
        draft.value(), result.run.id, result.run.createdAt, confirmedAt);
    return result;
}

inline std::int64_t contractMilliseconds(std::chrono::system_clock::time_point value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

// Field-by-field equality at the precision the adapters persist (milliseconds).
inline bool sameConversionRecord(
    const application::LegacyConversionRecord& left,
    const application::LegacyConversionRecord& right)
{
    if (left.runId != right.runId || left.sourceRunId != right.sourceRunId
        || left.sourceWorkflowId != right.sourceWorkflowId || left.policyId != right.policyId
        || left.reasonCode != right.reasonCode
        || left.sourceSnapshotFingerprint != right.sourceSnapshotFingerprint
        || left.targetRosterFingerprint != right.targetRosterFingerprint
        || left.confirmationFingerprint != right.confirmationFingerprint
        || left.confirmationMode != right.confirmationMode
        || left.confirmedAt.has_value() != right.confirmedAt.has_value()
        || contractMilliseconds(left.createdAt) != contractMilliseconds(right.createdAt)
        || left.dropped.size() != right.dropped.size()) {
        return false;
    }
    if (left.confirmedAt.has_value()
        && contractMilliseconds(*left.confirmedAt) != contractMilliseconds(*right.confirmedAt)) {
        return false;
    }
    for (std::size_t index = 0; index < left.dropped.size(); ++index) {
        if (left.dropped[index].memberId != right.dropped[index].memberId
            || left.dropped[index].setting != right.dropped[index].setting
            || left.dropped[index].legacyValue != right.dropped[index].legacyValue
            || left.dropped[index].reason != right.dropped[index].reason) {
            return false;
        }
    }
    return true;
}

// Inserts the legacy source Run that conversions name as their parent.
inline application::RunId insertContractLegacySource(
    application::IRunRepository& repository,
    const std::string& prefix,
    const application::ProjectId& projectId)
{
    application::Run source = contractRun(prefix + "-legacy-source", projectId);
    source.workflowId = application::kExecutionProfileWorkflowId;
    source.status = application::RunStatus::Completed;
    source.finishedAt = source.createdAt;
    requireRepositoryContract(
        repository.insertRunWithStages(source, contractStages(prefix + "-legacy-source", source.id)).success(),
        "Could not insert the legacy source Run");
    return source.id;
}

inline void runLegacyConversionRepositoryContractTests(
    application::IRunRepository& repository,
    const std::string& prefix,
    const application::ProjectId& projectId,
    const std::string& providerId,
    const std::string& modelId)
{
    using namespace application;
    const RunId sourceRunId = insertContractLegacySource(repository, prefix, projectId);

    // 1. A healthy explicit conversion commits Run, stages, roster and record together.
    auto healthy = contractConversion(prefix, projectId, providerId, modelId, sourceRunId, false);
    const auto inserted = repository.insertRunWithStagesAndLegacyConversion(
        healthy.run, healthy.stages, healthy.record, std::nullopt);
    const std::string healthyInsertMessage = inserted.success()
        ? std::string {}
        : "A healthy conversion aggregate was refused: " + inserted.error()->code
            + " (" + inserted.error()->message + ")";
    requireRepositoryContract(inserted.success(), healthyInsertMessage.c_str());
    const auto storedRun = repository.findRun(healthy.run.id);
    requireRepositoryContract(storedRun.success() && storedRun.value().has_value()
            && storedRun.value()->parentRunId == std::optional<RunId>(sourceRunId)
            && storedRun.value()->workflowId == kExecutionManifestWorkflowId
            && storedRun.value()->assignmentSnapshot.size() == healthy.run.assignmentSnapshot.size(),
        "The converted Run was not stored with its parent, workflow and roster");
    const auto storedStages = repository.listStages(healthy.run.id);
    requireRepositoryContract(storedStages.success() && storedStages.value().size() == healthy.stages.size(),
        "The converted Run's stages were not stored with it");
    const auto found = repository.findLegacyConversion(healthy.run.id);
    requireRepositoryContract(found.success() && found.value().has_value()
            && sameConversionRecord(*found.value(), healthy.record),
        "The conversion record did not round-trip exactly");
    requireRepositoryContract(found.value()->confirmedAt.has_value()
            && !found.value()->dropped.empty(),
        "An explicit conversion keeps its confirmation time and its dropped list");

    // 2. Any other Run, and an unknown id, has no conversion record.
    const auto noRecord = repository.findLegacyConversion(sourceRunId);
    requireRepositoryContract(noRecord.success() && !noRecord.value().has_value(),
        "A Run that was not converted must have no conversion record");
    const auto unknown = repository.findLegacyConversion(prefix + "-never-existed");
    requireRepositoryContract(unknown.success() && !unknown.value().has_value(),
        "An unknown Run must have no conversion record");

    // 3. A refused record leaves NO Run, NO stage and NO record behind.
    const auto expectRefusedWithoutTrace = [&](ContractConversion candidate, const char* message) {
        const auto refused = repository.insertRunWithStagesAndLegacyConversion(
            candidate.run, candidate.stages, candidate.record, std::nullopt);
        requireRepositoryContract(!refused.success(), message);
        const auto run = repository.findRun(candidate.run.id);
        requireRepositoryContract(run.success() && !run.value().has_value(),
            "A refused conversion left a Run behind");
        const auto stages = repository.listStages(candidate.run.id);
        requireRepositoryContract(stages.success() && stages.value().empty(),
            "A refused conversion left stages behind");
        const auto record = repository.findLegacyConversion(candidate.run.id);
        requireRepositoryContract(record.success() && !record.value().has_value(),
            "A refused conversion left a record behind");
    };
    {
        auto tampered = contractConversion(prefix + "-tampered", projectId, providerId, modelId, sourceRunId, false);
        tampered.record.confirmationFingerprint = std::string(64, 'a');
        expectRefusedWithoutTrace(std::move(tampered), "A record with a tampered fingerprint was accepted");
    }
    {
        auto otherRun = contractConversion(prefix + "-otherrun", projectId, providerId, modelId, sourceRunId, false);
        otherRun.record.runId = prefix + "-some-other-run";
        expectRefusedWithoutTrace(std::move(otherRun), "A record naming another Run was accepted");
    }
    {
        auto otherRoster = contractConversion(prefix + "-roster", projectId, providerId, modelId, sourceRunId, false);
        otherRoster.run.assignmentSnapshot[1].instructions = "changed after the record was built";
        expectRefusedWithoutTrace(std::move(otherRoster),
            "A record that does not certify the Run's roster was accepted");
    }
    {
        auto wrongParent = contractConversion(prefix + "-parent", projectId, providerId, modelId, sourceRunId, false);
        wrongParent.run.parentRunId = prefix + "-someone-else";
        expectRefusedWithoutTrace(std::move(wrongParent), "A record whose source is not the Run's parent was accepted");
    }
    {
        auto notManifest = contractConversion(prefix + "-wf", projectId, providerId, modelId, sourceRunId, false);
        notManifest.run.workflowId = kExecutionProfileWorkflowId;
        expectRefusedWithoutTrace(std::move(notManifest), "A converted Run on the legacy workflow was accepted");
    }

    // AG-17.7C-3: the conversion INDEX (the history marker) lists exactly the converted Runs and
    // their confirmation mode - nothing for a refused conversion and nothing for the source Run
    // or any other Run. (Other suites may share the repository, so only this prefix is judged.)
    const auto indexOfThisSuite = [&] {
        const auto index = repository.listLegacyConversionIndex();
        requireRepositoryContract(index.success(), "The conversion index could not be read");
        std::vector<LegacyConversionIndexEntry> mine;
        for (const auto& entry : index.value()) {
            if (entry.runId.rfind(prefix, 0) == 0) mine.push_back(entry);
        }
        return mine;
    };
    const auto afterRefusals = indexOfThisSuite();
    requireRepositoryContract(afterRefusals.size() == 1
            && afterRefusals.front().runId == healthy.run.id
            && afterRefusals.front().mode == std::optional<LegacyConversionConfirmationMode>(
                   LegacyConversionConfirmationMode::ExplicitUser),
        "The index must list the one committed explicit conversion, and nothing refused or ordinary");

    // 4. A duplicate Run id is refused and leaves the first aggregate and record untouched.
    auto duplicate = healthy;
    duplicate.record.dropped.clear();
    duplicate.record.confirmationFingerprint = LegacyRosterAdapter::confirmationFingerprint(
        duplicate.record.sourceRunId, duplicate.record.sourceWorkflowId, duplicate.record.policyId,
        duplicate.record.reasonCode, duplicate.record.sourceSnapshotFingerprint,
        kExecutionManifestWorkflowId, duplicate.record.targetRosterFingerprint, duplicate.record.dropped);
    const auto refusedDuplicate = repository.insertRunWithStagesAndLegacyConversion(
        duplicate.run, duplicate.stages, duplicate.record, std::nullopt);
    requireRepositoryContract(!refusedDuplicate.success(), "A duplicate converted Run id was accepted");
    const auto afterDuplicate = repository.findLegacyConversion(healthy.run.id);
    requireRepositoryContract(afterDuplicate.success() && afterDuplicate.value().has_value()
            && sameConversionRecord(*afterDuplicate.value(), healthy.record),
        "A refused duplicate changed the committed conversion record");

    // 5. A safe automatic Retry round-trips WITHOUT a confirmation time and with nothing dropped.
    auto safe = contractConversion(prefix + "-safe", projectId, providerId, modelId, sourceRunId, true);
    requireRepositoryContract(repository.insertRunWithStagesAndLegacyConversion(
                                  safe.run, safe.stages, safe.record, std::nullopt).success(),
        "A safe-retry conversion aggregate was refused");
    const auto safeFound = repository.findLegacyConversion(safe.run.id);
    requireRepositoryContract(safeFound.success() && safeFound.value().has_value()
            && sameConversionRecord(*safeFound.value(), safe.record)
            && !safeFound.value()->confirmedAt.has_value() && safeFound.value()->dropped.empty()
            && safeFound.value()->confirmationMode == LegacyConversionConfirmationMode::NotRequiredSafeRetry,
        "A safe automatic Retry must never gain a confirmation time");

    // The index follows: both modes, deterministic order, still nothing for any other Run.
    const auto finalIndex = indexOfThisSuite();
    requireRepositoryContract(finalIndex.size() == 2, "The index must list both converted Runs");
    const auto modeInIndex = [&](const RunId& id) -> std::optional<LegacyConversionConfirmationMode> {
        for (const auto& entry : finalIndex) {
            if (entry.runId == id) return entry.mode;
        }
        return std::nullopt;
    };
    requireRepositoryContract(modeInIndex(healthy.run.id) == std::optional<LegacyConversionConfirmationMode>(
                                  LegacyConversionConfirmationMode::ExplicitUser)
            && modeInIndex(safe.run.id) == std::optional<LegacyConversionConfirmationMode>(
                   LegacyConversionConfirmationMode::NotRequiredSafeRetry),
        "The index must carry each Run's own confirmation mode");
    requireRepositoryContract(!modeInIndex(sourceRunId).has_value(),
        "The source Run of a conversion is not itself a converted Run");
    requireRepositoryContract(finalIndex.front().runId < finalIndex.back().runId,
        "The index must come back in a deterministic order");
}

} // namespace ai::tests
