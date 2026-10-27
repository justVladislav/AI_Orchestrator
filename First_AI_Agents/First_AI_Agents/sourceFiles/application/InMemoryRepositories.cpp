#include "ai/application/InMemoryRepositories.hpp"

#include "ai/application/artifacts/ArtifactPathPolicy.hpp"
#include "ai/application/CacheDurationAccounting.hpp"
#include "ai/application/planning/RunDraftGroup.hpp"
#include "ai/application/Sha256.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace ai::application {

namespace {

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

common::Result<void> validateProgress(double progress, const char* aggregate)
{
    // NaN would otherwise pass both range comparisons and poison ordering/UI
    // calculations later, so every aggregate creation path rejects it here.
    if (!std::isfinite(progress) || progress < 0.0 || progress > 1.0) {
        return common::Result<void>::failure(error(
            "persistence.invalid_progress",
            std::string(aggregate) + " progress must be finite and in the [0, 1] range"));
    }
    return common::Result<void>::ok();
}

common::Result<void> validateRunCacheDuration(const Run& run)
{
    if (!run.usage.cacheBreakdownIsConsistent()) {
        return common::Result<void>::failure(error(
            "persistence.invalid_data",
            "Run cache-creation and cache-read tokens must fit within input tokens"));
    }
    const auto runProblem = checkCacheCreationDurationUsage(
        run.usage.cacheCreationInputTokens, run.usage.cacheCreationDuration);
    if (runProblem != CacheCreationDurationUsageProblem::None) {
        return common::Result<void>::failure(error(
            cacheCreationDurationUsageProblemCode(runProblem),
            cacheCreationDurationUsageProblemMessage(runProblem)));
    }
    const auto budgetProblem = checkCacheCreationDurationUsage(
        run.providerBudgetUsage.cacheCreationInputTokens,
        run.providerBudgetUsage.cacheCreationDuration);
    if (budgetProblem != CacheCreationDurationUsageProblem::None) {
        return common::Result<void>::failure(error(
            cacheCreationDurationUsageProblemCode(budgetProblem),
            cacheCreationDurationUsageProblemMessage(budgetProblem)));
    }
    if (run.providerBudgetUsage.cacheCreationInputTokens
        > run.providerBudgetUsage.inputTokens) {
        return common::Result<void>::failure(error(
            "persistence.invalid_data",
            "Provider budget cache creation exceeds budget-countable input"));
    }
    return common::Result<void>::ok();
}

common::Result<void> validateStages(
    const RunId& runId,
    const std::vector<RunStage>& stages)
{
    // Validate the complete replacement before taking ownership of any part;
    // this gives the in-memory adapter the same all-or-nothing semantics as a
    // database transaction.
    std::unordered_set<StageId> stageIds;
    for (const auto& stage : stages) {
        if (stage.runId != runId) {
            return common::Result<void>::failure(
                error("stage.run_mismatch", "Every stage must belong to the aggregate run"));
        }
        const auto validProgress = validateProgress(stage.progress, "Stage");
        if (!validProgress.success()) {
            return validProgress;
        }
        if (!stageIds.insert(stage.id).second) {
            return common::Result<void>::failure(
                error("stage.duplicate_id", "Stage ids must be unique within a run"));
        }
    }
    return common::Result<void>::ok();
}

} // namespace

common::Result<Project> InMemoryProjectRepository::insert(Project project)
{
    // Every public operation locks independently, matching the thread-safety
    // expected later from repository transactions.
    std::lock_guard lock(mutex_);
    if (projects_.contains(project.id)) {
        return common::Result<Project>::failure(error("project.duplicate_id", "Project id already exists"));
    }
    projects_.emplace(project.id, project);
    return common::Result<Project>::ok(std::move(project));
}

common::Result<Project> InMemoryProjectRepository::update(Project project)
{
    std::lock_guard lock(mutex_);
    if (!projects_.contains(project.id)) {
        return common::Result<Project>::failure(error("project.not_found", "Project not found"));
    }
    projects_[project.id] = project;
    return common::Result<Project>::ok(std::move(project));
}

common::Result<std::optional<Project>> InMemoryProjectRepository::findById(const ProjectId& id) const
{
    std::lock_guard lock(mutex_);
    const auto it = projects_.find(id);
    if (it == projects_.end()) {
        return common::Result<std::optional<Project>>::ok(std::nullopt);
    }
    return common::Result<std::optional<Project>>::ok(it->second);
}

common::Result<std::optional<Project>> InMemoryProjectRepository::findByRootPath(
    const std::filesystem::path& rootPath) const
{
    std::lock_guard lock(mutex_);
    const auto it = std::find_if(projects_.begin(), projects_.end(), [&rootPath](const auto& entry) {
        return entry.second.rootPath == rootPath;
    });
    if (it == projects_.end()) {
        return common::Result<std::optional<Project>>::ok(std::nullopt);
    }
    return common::Result<std::optional<Project>>::ok(it->second);
}

common::Result<std::vector<Project>> InMemoryProjectRepository::listAll() const
{
    std::lock_guard lock(mutex_);
    std::vector<Project> projects;
    projects.reserve(projects_.size());
    for (const auto& [id, project] : projects_) {
        static_cast<void>(id);
        projects.push_back(project);
    }
    std::sort(projects.begin(), projects.end(), [](const Project& left, const Project& right) {
        return left.createdAt < right.createdAt;
    });
    return common::Result<std::vector<Project>>::ok(std::move(projects));
}

common::Result<Run> InMemoryRunRepository::insertRun(Run run)
{
    const auto validCacheDuration = validateRunCacheDuration(run);
    if (!validCacheDuration.success()) {
        return common::Result<Run>::failure(*validCacheDuration.error());
    }
    std::lock_guard lock(mutex_);
    if (runs_.contains(run.id)) {
        return common::Result<Run>::failure(error("run.duplicate_id", "Run id already exists"));
    }
    runs_.emplace(run.id, run);
    return common::Result<Run>::ok(std::move(run));
}

common::Result<Run> InMemoryRunRepository::insertRunWithStages(
    Run run,
    std::vector<RunStage> stages)
{
    // Do every fallible domain validation before mutating either map. The
    // single critical section is the in-memory equivalent of SQLite's
    // transaction and prevents readers from observing half an aggregate.
    const auto validRunProgress = validateProgress(run.progress, "Run");
    if (!validRunProgress.success()) {
        return common::Result<Run>::failure(*validRunProgress.error());
    }
    const auto validCacheDuration = validateRunCacheDuration(run);
    if (!validCacheDuration.success()) {
        return common::Result<Run>::failure(*validCacheDuration.error());
    }
    const auto validStages = validateStages(run.id, stages);
    if (!validStages.success()) {
        return common::Result<Run>::failure(*validStages.error());
    }

    std::lock_guard lock(mutex_);
    if (runs_.contains(run.id)) {
        return common::Result<Run>::failure(error("run.duplicate_id", "Run id already exists"));
    }
    runs_.emplace(run.id, run);
    stages_.emplace(run.id, std::move(stages));
    return common::Result<Run>::ok(std::move(run));
}

common::Result<Run> InMemoryRunRepository::insertRunWithStagesAndLegacyConversion(
    Run run,
    std::vector<RunStage> stages,
    LegacyConversionRecord record,
    const std::optional<RunId>& taskLinkSourceRunId)
{
    static_cast<void>(taskLinkSourceRunId);
    // Every fallible validation happens before either map is touched, so a refused record
    // leaves no Run behind and a refused Run leaves no record behind.
    const auto validRunProgress = validateProgress(run.progress, "Run");
    if (!validRunProgress.success()) {
        return common::Result<Run>::failure(*validRunProgress.error());
    }
    const auto validCacheDuration = validateRunCacheDuration(run);
    if (!validCacheDuration.success()) {
        return common::Result<Run>::failure(*validCacheDuration.error());
    }
    const auto validStages = validateStages(run.id, stages);
    if (!validStages.success()) {
        return common::Result<Run>::failure(*validStages.error());
    }
    if (const auto problem = legacyConversionAggregateProblem(run, record)) {
        return common::Result<Run>::failure(error(kLegacyConversionRecordInvalidCode, *problem));
    }

    std::lock_guard lock(mutex_);
    if (runs_.contains(run.id)) {
        return common::Result<Run>::failure(error("run.duplicate_id", "Run id already exists"));
    }
    runs_.emplace(run.id, run);
    stages_.emplace(run.id, std::move(stages));
    legacyConversions_.emplace(run.id, std::move(record));
    return common::Result<Run>::ok(std::move(run));
}

common::Result<std::optional<LegacyConversionRecord>> InMemoryRunRepository::findLegacyConversion(
    const RunId& runId) const
{
    std::lock_guard lock(mutex_);
    const auto found = legacyConversions_.find(runId);
    if (found == legacyConversions_.end()) {
        return common::Result<std::optional<LegacyConversionRecord>>::ok(std::nullopt);
    }
    const auto run = runs_.find(runId);
    if (run == runs_.end()) {
        return common::Result<std::optional<LegacyConversionRecord>>::failure(error(
            kLegacyConversionRecordCorruptCode, "A conversion record has no Run"));
    }
    if (const auto problem = legacyConversionAggregateProblem(run->second, found->second)) {
        return common::Result<std::optional<LegacyConversionRecord>>::failure(
            error(kLegacyConversionRecordCorruptCode, *problem));
    }
    return common::Result<std::optional<LegacyConversionRecord>>::ok(found->second);
}

common::Result<std::vector<LegacyConversionIndexEntry>>
InMemoryRunRepository::listLegacyConversionIndex() const
{
    std::lock_guard lock(mutex_);
    std::vector<LegacyConversionIndexEntry> index;
    index.reserve(legacyConversions_.size());
    for (const auto& [runId, record] : legacyConversions_) {
        index.push_back({runId, record.confirmationMode});
    }
    // An unordered map has no order of its own; the index is deterministic.
    std::sort(index.begin(), index.end(),
        [](const auto& left, const auto& right) { return left.runId < right.runId; });
    return common::Result<std::vector<LegacyConversionIndexEntry>>::ok(std::move(index));
}

common::Result<Run> InMemoryRunRepository::updateRun(Run run)
{
    const auto validCacheDuration = validateRunCacheDuration(run);
    if (!validCacheDuration.success()) {
        return common::Result<Run>::failure(*validCacheDuration.error());
    }
    std::lock_guard lock(mutex_);
    const auto it = runs_.find(run.id);
    if (it == runs_.end()) {
        return common::Result<Run>::failure(error("run.not_found", "Run not found"));
    }
    // Reject stale snapshots instead of silently overwriting a concurrent cancel
    // or progress update. SQLite will implement the same check in its UPDATE.
    if (it->second.revision != run.revision) {
        return common::Result<Run>::failure(error("run.revision_conflict", "Run was updated concurrently"));
    }
    // The resolved provider budget is immutable Run input. Mutable lifecycle
    // updates may advance usage/status, but must never replace the policy that
    // was frozen before the first provider request. The SQLite adapter enforces
    // the same invariant by deliberately excluding provider_budget_json from
    // its UPDATE statement.
    run.providerBudget = it->second.providerBudget;
    run.verificationRepairPolicy = it->second.verificationRepairPolicy;
    run.interfaceContracts = it->second.interfaceContracts;
    ++run.revision;
    it->second = run;
    return common::Result<Run>::ok(std::move(run));
}

common::Result<std::optional<Run>> InMemoryRunRepository::findRun(const RunId& id) const
{
    std::lock_guard lock(mutex_);
    const auto it = runs_.find(id);
    if (it == runs_.end()) {
        return common::Result<std::optional<Run>>::ok(std::nullopt);
    }
    return common::Result<std::optional<Run>>::ok(it->second);
}

common::Result<std::vector<Run>> InMemoryRunRepository::listRuns(
    const std::optional<ProjectId>& projectId) const
{
    std::lock_guard lock(mutex_);
    std::vector<Run> runs;
    for (const auto& [id, run] : runs_) {
        static_cast<void>(id);
        if (!projectId.has_value() || run.projectId == *projectId) {
            runs.push_back(run);
        }
    }
    std::sort(runs.begin(), runs.end(), [](const Run& left, const Run& right) {
        return left.createdAt < right.createdAt;
    });
    return common::Result<std::vector<Run>>::ok(std::move(runs));
}

common::Result<std::vector<Run>> InMemoryRunRepository::listUnfinishedRuns() const
{
    std::lock_guard lock(mutex_);
    std::vector<Run> runs;
    for (const auto& [id, run] : runs_) {
        static_cast<void>(id);
        if (run.status != RunStatus::Prepared && !isTerminal(run.status)) {
            runs.push_back(run);
        }
    }
    return common::Result<std::vector<Run>>::ok(std::move(runs));
}

common::Result<void> InMemoryRunRepository::replaceStages(
    const RunId& runId,
    std::vector<RunStage> stages)
{
    const auto validStages = validateStages(runId, stages);
    if (!validStages.success()) {
        return validStages;
    }
    std::lock_guard lock(mutex_);
    if (!runs_.contains(runId)) {
        return common::Result<void>::failure(error("run.not_found", "Run not found"));
    }
    // Stages are declared as one initial snapshot before the run is queued.
    stages_[runId] = std::move(stages);
    return common::Result<void>::ok();
}

common::Result<RunStage> InMemoryRunRepository::updateStage(RunStage stage)
{
    std::lock_guard lock(mutex_);
    const auto runIt = stages_.find(stage.runId);
    if (runIt == stages_.end()) {
        return common::Result<RunStage>::failure(error("stage.not_found", "Run stages not found"));
    }
    const auto stageIt = std::find_if(runIt->second.begin(), runIt->second.end(), [&stage](const RunStage& candidate) {
        return candidate.id == stage.id;
    });
    if (stageIt == runIt->second.end()) {
        return common::Result<RunStage>::failure(error("stage.not_found", "Stage not found"));
    }
    *stageIt = stage;
    return common::Result<RunStage>::ok(std::move(stage));
}

common::Result<std::vector<RunStage>> InMemoryRunRepository::listStages(const RunId& runId) const
{
    std::lock_guard lock(mutex_);
    const auto it = stages_.find(runId);
    if (it == stages_.end()) {
        return common::Result<std::vector<RunStage>>::ok({});
    }
    return common::Result<std::vector<RunStage>>::ok(it->second);
}

common::Result<Artifact> InMemoryArtifactRepository::insertArtifact(Artifact artifact)
{
    std::lock_guard lock(mutex_);
    if (artifacts_.contains(artifact.id)) {
        return common::Result<Artifact>::failure(
            error("artifact.duplicate_id", "Artifact id already exists"));
    }
    artifacts_.emplace(artifact.id, artifact);
    return common::Result<Artifact>::ok(std::move(artifact));
}

common::Result<std::optional<Artifact>> InMemoryArtifactRepository::findArtifact(
    const ArtifactId& id) const
{
    std::lock_guard lock(mutex_);
    const auto it = artifacts_.find(id);
    if (it == artifacts_.end()) {
        return common::Result<std::optional<Artifact>>::ok(std::nullopt);
    }
    return common::Result<std::optional<Artifact>>::ok(it->second);
}

common::Result<std::vector<Artifact>> InMemoryArtifactRepository::listArtifacts(
    const RunId& runId) const
{
    std::lock_guard lock(mutex_);
    std::vector<Artifact> artifacts;
    for (const auto& [id, artifact] : artifacts_) {
        static_cast<void>(id);
        if (artifact.runId == runId) {
            artifacts.push_back(artifact);
        }
    }
    // Deterministic order matches the contract every IArtifactRepository
    // adapter must honor: createdAt first, then id as a stable tiebreaker.
    std::sort(artifacts.begin(), artifacts.end(), [](const Artifact& left, const Artifact& right) {
        if (left.createdAt != right.createdAt) {
            return left.createdAt < right.createdAt;
        }
        return left.id < right.id;
    });
    return common::Result<std::vector<Artifact>>::ok(std::move(artifacts));
}

common::Result<bool> InMemoryRunWarningRepository::recordWarning(RunWarning warning)
{
    std::lock_guard lock(mutex_);
    for (const auto& existing : warnings_) {
        if (existing.runId == warning.runId && existing.stageId == warning.stageId
            && existing.code == warning.code && existing.artifactName == warning.artifactName) {
            return common::Result<bool>::ok(false);
        }
    }
    warnings_.push_back(std::move(warning));
    return common::Result<bool>::ok(true);
}

common::Result<std::vector<RunWarning>> InMemoryRunWarningRepository::listWarnings(const RunId& runId) const
{
    std::lock_guard lock(mutex_);
    std::vector<RunWarning> warnings;
    for (const auto& warning : warnings_) {
        if (warning.runId == runId) warnings.push_back(warning);
    }
    std::sort(warnings.begin(), warnings.end(), [](const RunWarning& left, const RunWarning& right) {
        if (left.createdAt != right.createdAt) return left.createdAt < right.createdAt;
        return left.id < right.id;
    });
    return common::Result<std::vector<RunWarning>>::ok(std::move(warnings));
}

common::Result<StoredArtifactFile> InMemoryArtifactStore::writeArtifact(
    const RunId& runId,
    std::string_view suggestedName,
    std::string_view content)
{
    std::lock_guard lock(mutex_);
    const std::string relativePath = buildArtifactRelativePath(runId, suggestedName, counter_++);
    filesByRelativePath_[relativePath] = std::string(content);

    StoredArtifactFile stored;
    stored.relativePath = relativePath;
    stored.byteSize = content.size();
    stored.sha256 = sha256Hex(content);
    return common::Result<StoredArtifactFile>::ok(std::move(stored));
}

common::Result<std::filesystem::path> InMemoryArtifactStore::resolveAbsolutePath(
    const std::filesystem::path& relativePath) const
{
    std::lock_guard lock(mutex_);
    const std::string key = relativePath.generic_string();
    if (!filesByRelativePath_.contains(key)) {
        return common::Result<std::filesystem::path>::failure(
            error("artifact.not_found", "Artifact content was not found in the in-memory store"));
    }
    // No real filesystem exists here; the "absolute path" is the same key
    // used to store the content so tests can still round-trip through it.
    return common::Result<std::filesystem::path>::ok(relativePath);
}

common::Result<std::string> InMemoryArtifactStore::readArtifact(
    const std::filesystem::path& relativePath) const
{
    std::lock_guard lock(mutex_);
    const std::string key = relativePath.generic_string();
    const auto it = filesByRelativePath_.find(key);
    if (it == filesByRelativePath_.end()) {
        return common::Result<std::string>::failure(
            error("artifact.not_found", "Artifact content was not found in the in-memory store"));
    }
    return common::Result<std::string>::ok(it->second);
}

common::Result<void> InMemoryArtifactStore::removeArtifact(
    const std::filesystem::path& relativePath)
{
    std::lock_guard lock(mutex_);
    const std::string key = relativePath.generic_string();
    if (filesByRelativePath_.erase(key) == 0) {
        return common::Result<void>::failure(
            error("artifact.not_found", "Artifact content was not found in the in-memory store"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<ProjectId>> InMemoryWorkspaceStateRepository::activeProjectId() const
{
    std::lock_guard lock(mutex_);
    return common::Result<std::optional<ProjectId>>::ok(activeProjectId_);
}

common::Result<void> InMemoryWorkspaceStateRepository::setActiveProjectId(
    const std::optional<ProjectId>& projectId)
{
    std::lock_guard lock(mutex_);
    activeProjectId_ = projectId;
    return common::Result<void>::ok();
}

common::Result<Provider> InMemoryProviderRepository::insert(Provider provider)
{
    std::lock_guard lock(mutex_);
    if (providers_.contains(provider.id)) {
        return common::Result<Provider>::failure(error("provider.duplicate_id", "Provider id already exists"));
    }
    providers_.emplace(provider.id, provider);
    return common::Result<Provider>::ok(std::move(provider));
}

common::Result<Provider> InMemoryProviderRepository::update(Provider provider)
{
    std::lock_guard lock(mutex_);
    if (!providers_.contains(provider.id)) {
        return common::Result<Provider>::failure(error("provider.not_found", "Provider not found"));
    }
    providers_[provider.id] = provider;
    return common::Result<Provider>::ok(std::move(provider));
}

common::Result<std::optional<Provider>> InMemoryProviderRepository::findById(const ProviderId& id) const
{
    std::lock_guard lock(mutex_);
    const auto it = providers_.find(id);
    if (it == providers_.end()) {
        return common::Result<std::optional<Provider>>::ok(std::nullopt);
    }
    return common::Result<std::optional<Provider>>::ok(it->second);
}

common::Result<std::vector<Provider>> InMemoryProviderRepository::listAll() const
{
    std::lock_guard lock(mutex_);
    std::vector<Provider> providers;
    providers.reserve(providers_.size());
    for (const auto& [id, provider] : providers_) {
        static_cast<void>(id);
        providers.push_back(provider);
    }
    std::sort(providers.begin(), providers.end(), [](const Provider& left, const Provider& right) {
        if (left.createdAt != right.createdAt) return left.createdAt < right.createdAt;
        return left.id < right.id;
    });
    return common::Result<std::vector<Provider>>::ok(std::move(providers));
}

common::Result<Model> InMemoryModelRepository::insert(Model model)
{
    std::lock_guard lock(mutex_);
    if (models_.contains(model.id)) {
        return common::Result<Model>::failure(error("model.duplicate_id", "Model id already exists"));
    }
    models_.emplace(model.id, model);
    return common::Result<Model>::ok(std::move(model));
}

common::Result<Model> InMemoryModelRepository::update(Model model)
{
    std::lock_guard lock(mutex_);
    const auto it = models_.find(model.id);
    if (it == models_.end()) {
        return common::Result<Model>::failure(error("model.not_found", "Model not found"));
    }
    // isDefault is intentionally preserved from the stored row: only
    // setDefaultModel() below may change it, so an ordinary field update can
    // never accidentally clear or set the default flag.
    model.isDefault = it->second.isDefault;
    models_[model.id] = model;
    return common::Result<Model>::ok(std::move(model));
}

common::Result<std::optional<Model>> InMemoryModelRepository::findById(const ModelId& id) const
{
    std::lock_guard lock(mutex_);
    const auto it = models_.find(id);
    if (it == models_.end()) {
        return common::Result<std::optional<Model>>::ok(std::nullopt);
    }
    return common::Result<std::optional<Model>>::ok(it->second);
}

common::Result<std::vector<Model>> InMemoryModelRepository::listAll(
    const std::optional<ProviderId>& providerId) const
{
    std::lock_guard lock(mutex_);
    std::vector<Model> models;
    for (const auto& [id, model] : models_) {
        static_cast<void>(id);
        if (!providerId.has_value() || model.providerId == *providerId) {
            models.push_back(model);
        }
    }
    std::sort(models.begin(), models.end(), [](const Model& left, const Model& right) {
        if (left.createdAt != right.createdAt) return left.createdAt < right.createdAt;
        return left.id < right.id;
    });
    return common::Result<std::vector<Model>>::ok(std::move(models));
}

common::Result<void> InMemoryModelRepository::setDefaultModel(
    const ProviderId& providerId,
    const ModelId& modelId)
{
    std::lock_guard lock(mutex_);
    const auto target = models_.find(modelId);
    if (target == models_.end()) {
        return common::Result<void>::failure(error("model.not_found", "Model not found"));
    }
    if (target->second.providerId != providerId) {
        return common::Result<void>::failure(
            error("model.provider_mismatch", "Model does not belong to the given provider"));
    }

    // Single critical section: no reader can observe two default models (or
    // zero) for this provider between the unset and set steps.
    for (auto& [id, model] : models_) {
        static_cast<void>(id);
        if (model.providerId == providerId) {
            model.isDefault = false;
        }
    }
    target->second.isDefault = true;
    return common::Result<void>::ok();
}

common::Result<void> InMemoryModelRepository::remove(const ModelId& id)
{
    std::lock_guard lock(mutex_);
    // No referential-integrity concept in this test-only store - the Sqlite
    // implementation is what actually enforces "model.in_use" via its
    // ON DELETE RESTRICT foreign keys.
    if (models_.erase(id) == 0) {
        return common::Result<void>::failure(error("model.not_found", "Model not found"));
    }
    return common::Result<void>::ok();
}

common::Result<ExecutionProfile> InMemoryExecutionProfileRepository::insert(ExecutionProfile profile)
{
    std::lock_guard lock(mutex_);
    if (profiles_.contains(profile.id)) {
        return common::Result<ExecutionProfile>::failure(
            error("execution_profile.duplicate_id", "Execution profile id already exists"));
    }
    profiles_.emplace(profile.id, profile);
    return common::Result<ExecutionProfile>::ok(std::move(profile));
}

common::Result<ExecutionProfile> InMemoryExecutionProfileRepository::update(ExecutionProfile profile)
{
    std::lock_guard lock(mutex_);
    const auto it = profiles_.find(profile.id);
    if (it == profiles_.end()) {
        return common::Result<ExecutionProfile>::failure(
            error("execution_profile.not_found", "Execution profile not found"));
    }
    // Same rule as InMemoryModelRepository::update: only setDefaultProfile()
    // may change isDefault.
    profile.isDefault = it->second.isDefault;
    profiles_[profile.id] = profile;
    return common::Result<ExecutionProfile>::ok(std::move(profile));
}

common::Result<std::optional<ExecutionProfile>> InMemoryExecutionProfileRepository::findById(
    const ExecutionProfileId& id) const
{
    std::lock_guard lock(mutex_);
    const auto it = profiles_.find(id);
    if (it == profiles_.end()) {
        return common::Result<std::optional<ExecutionProfile>>::ok(std::nullopt);
    }
    return common::Result<std::optional<ExecutionProfile>>::ok(it->second);
}

common::Result<std::vector<ExecutionProfile>> InMemoryExecutionProfileRepository::listAll() const
{
    std::lock_guard lock(mutex_);
    std::vector<ExecutionProfile> profiles;
    profiles.reserve(profiles_.size());
    for (const auto& [id, profile] : profiles_) {
        static_cast<void>(id);
        profiles.push_back(profile);
    }
    std::sort(profiles.begin(), profiles.end(), [](const ExecutionProfile& left, const ExecutionProfile& right) {
        if (left.createdAt != right.createdAt) return left.createdAt < right.createdAt;
        return left.id < right.id;
    });
    return common::Result<std::vector<ExecutionProfile>>::ok(std::move(profiles));
}

common::Result<void> InMemoryExecutionProfileRepository::setDefaultProfile(
    const ExecutionProfileId& profileId)
{
    std::lock_guard lock(mutex_);
    const auto target = profiles_.find(profileId);
    if (target == profiles_.end()) {
        return common::Result<void>::failure(
            error("execution_profile.not_found", "Execution profile not found"));
    }
    for (auto& [id, profile] : profiles_) {
        static_cast<void>(id);
        profile.isDefault = false;
    }
    target->second.isDefault = true;
    return common::Result<void>::ok();
}

// ── InMemoryRunDraftRepository ───────────────────────────────────────────────

common::Result<RunDraft> InMemoryRunDraftRepository::insert(RunDraft draft)
{
    std::lock_guard lock(mutex_);
    if (draft.provenance.has_value()) {
        return common::Result<RunDraft>::failure(error("run_draft.group_insert_required",
            "A draft that belongs to a group is created through insertGroup"));
    }
    if (drafts_.contains(draft.id)) {
        return common::Result<RunDraft>::failure(error("run_draft.duplicate_id", "Run draft id already exists"));
    }
    drafts_.emplace(draft.id, draft);
    return common::Result<RunDraft>::ok(std::move(draft));
}

common::Result<RunDraft> InMemoryRunDraftRepository::update(RunDraft draft)
{
    std::lock_guard lock(mutex_);
    const auto it = drafts_.find(draft.id);
    if (it == drafts_.end()) {
        return common::Result<RunDraft>::failure(error("run_draft.not_found", "Run draft not found"));
    }
    const auto unchanged = checkRunDraftProvenanceUnchanged(it->second.provenance, it->second.projectId, draft);
    if (!unchanged.success()) return common::Result<RunDraft>::failure(*unchanged.error());
    it->second = draft;
    return common::Result<RunDraft>::ok(std::move(draft));
}

common::Result<std::optional<RunDraft>> InMemoryRunDraftRepository::findById(const RunDraftId& id) const
{
    std::lock_guard lock(mutex_);
    const auto it = drafts_.find(id);
    if (it == drafts_.end()) return common::Result<std::optional<RunDraft>>::ok(std::nullopt);
    return common::Result<std::optional<RunDraft>>::ok(it->second);
}

common::Result<std::vector<RunDraft>> InMemoryRunDraftRepository::listAll() const
{
    std::lock_guard lock(mutex_);
    std::vector<RunDraft> drafts;
    drafts.reserve(drafts_.size());
    for (const auto& [id, draft] : drafts_) {
        static_cast<void>(id);
        drafts.push_back(draft);
    }
    std::sort(drafts.begin(), drafts.end(), [](const RunDraft& left, const RunDraft& right) {
        if (left.updatedAt != right.updatedAt) return left.updatedAt > right.updatedAt;
        return left.id < right.id;
    });
    return common::Result<std::vector<RunDraft>>::ok(std::move(drafts));
}

common::Result<void> InMemoryRunDraftRepository::remove(const RunDraftId& id)
{
    std::lock_guard lock(mutex_);
    const auto it = drafts_.find(id);
    if (it == drafts_.end()) {
        return common::Result<void>::failure(error("run_draft.not_found", "Run draft not found"));
    }
    const std::optional<RunDraftGroupId> groupId = it->second.provenance.has_value()
        ? std::optional<RunDraftGroupId>(it->second.provenance->groupId) : std::nullopt;
    drafts_.erase(it);
    // The last draft takes its group with it (one critical section = one transaction here).
    if (groupId.has_value()) {
        const bool stillUsed = std::any_of(drafts_.begin(), drafts_.end(), [&](const auto& entry) {
            return entry.second.provenance.has_value() && entry.second.provenance->groupId == *groupId;
        });
        if (!stillUsed) groups_.erase(*groupId);
    }
    return common::Result<void>::ok();
}

common::Result<std::vector<RunDraft>> InMemoryRunDraftRepository::insertGroup(
    RunDraftGroup group, std::vector<RunDraft> drafts)
{
    const auto allowed = checkRunDraftGroupPublicInsert(group);
    if (!allowed.success()) return common::Result<std::vector<RunDraft>>::failure(*allowed.error());
    return insertGroupChecked(std::move(group), std::move(drafts));
}

common::Result<std::vector<RunDraft>> InMemoryRunDraftRepository::insertConfirmedFollowUpGroup(
    RunDraftGroup group, std::vector<RunDraft> drafts, const std::function<void()>& whileLocked)
{
    if (group.origin != RunDraftGroupOrigin::FollowUpRequest) {
        return common::Result<std::vector<RunDraft>>::failure(
            error("run_draft_group.invalid_group", "The confirmation path saves only a follow-up group"));
    }
    return insertGroupChecked(std::move(group), std::move(drafts), whileLocked);
}

common::Result<std::vector<RunDraft>> InMemoryRunDraftRepository::insertGroupChecked(
    RunDraftGroup group, std::vector<RunDraft> drafts, const std::function<void()>& whileLocked)
{
    // Everything is validated and every uniqueness rule is checked BEFORE the first write, so a refusal leaves the
    // store exactly as it was.
    const auto valid = validateRunDraftGroupInsertion(group, drafts);
    if (!valid.success()) return common::Result<std::vector<RunDraft>>::failure(*valid.error());

    std::lock_guard lock(mutex_);
    if (groups_.contains(group.id)) {
        return common::Result<std::vector<RunDraft>>::failure(
            error("run_draft_group.duplicate_id", "Run draft group id already exists"));
    }
    for (const auto& [id, existing] : groups_) {
        static_cast<void>(id);
        if (existing.idempotencyKey == group.idempotencyKey) {
            return common::Result<std::vector<RunDraft>>::failure(error(
                "run_draft_group.duplicate_idempotency_key", "A group with this idempotency key already exists"));
        }
        if (group.sourceRequestId.has_value() && existing.sourceRequestId == group.sourceRequestId) {
            return common::Result<std::vector<RunDraft>>::failure(error(
                "run_draft_group.duplicate_source_request", "A group of this follow-up request already exists"));
        }
    }
    for (const auto& draft : drafts) {
        if (drafts_.contains(draft.id)) {
            return common::Result<std::vector<RunDraft>>::failure(
                error("run_draft.duplicate_id", "Run draft id already exists"));
        }
    }

    std::sort(drafts.begin(), drafts.end(), [](const RunDraft& left, const RunDraft& right) {
        return left.provenance->ordinal < right.provenance->ordinal;
    });
    for (const auto& draft : drafts) drafts_.emplace(draft.id, draft);
    groups_.emplace(group.id, std::move(group));
    if (whileLocked) whileLocked();
    return common::Result<std::vector<RunDraft>>::ok(std::move(drafts));
}

common::Result<std::optional<RunDraftGroup>> InMemoryRunDraftRepository::findGroupById(
    const RunDraftGroupId& id) const
{
    std::lock_guard lock(mutex_);
    const auto it = groups_.find(id);
    if (it == groups_.end()) return common::Result<std::optional<RunDraftGroup>>::ok(std::nullopt);
    return common::Result<std::optional<RunDraftGroup>>::ok(it->second);
}

common::Result<std::optional<RunDraftGroup>> InMemoryRunDraftRepository::findGroupByIdempotencyKey(
    const std::string& idempotencyKey) const
{
    std::lock_guard lock(mutex_);
    for (const auto& [id, group] : groups_) {
        static_cast<void>(id);
        if (group.idempotencyKey == idempotencyKey) {
            return common::Result<std::optional<RunDraftGroup>>::ok(group);
        }
    }
    return common::Result<std::optional<RunDraftGroup>>::ok(std::nullopt);
}

common::Result<std::vector<RunDraft>> InMemoryRunDraftRepository::listGroupDrafts(const RunDraftGroupId& id) const
{
    std::lock_guard lock(mutex_);
    std::vector<RunDraft> drafts;
    for (const auto& [draftId, draft] : drafts_) {
        static_cast<void>(draftId);
        if (draft.provenance.has_value() && draft.provenance->groupId == id) drafts.push_back(draft);
    }
    std::sort(drafts.begin(), drafts.end(), [](const RunDraft& left, const RunDraft& right) {
        return left.provenance->ordinal < right.provenance->ordinal;
    });
    return common::Result<std::vector<RunDraft>>::ok(std::move(drafts));
}

// ── InMemoryAgentToolSessionRepository ───────────────────────────────────────

common::Result<AgentToolSession> InMemoryAgentToolSessionRepository::insertSession(
    AgentToolSession session)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (sessions_.count(session.sessionId))
        return common::Result<AgentToolSession>::failure(
            {"agent_session.duplicate_id", "Agent tool session already exists"});
    const std::string id = session.sessionId;
    sessions_.emplace(id, session);
    return common::Result<AgentToolSession>::ok(std::move(session));
}

common::Result<AgentToolSession> InMemoryAgentToolSessionRepository::updateSession(
    AgentToolSession session)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session.sessionId);
    if (it == sessions_.end())
        return common::Result<AgentToolSession>::failure(
            {"agent_session.not_found", "Agent tool session not found"});
    it->second = session;
    return common::Result<AgentToolSession>::ok(std::move(session));
}

common::Result<std::optional<AgentToolSession>> InMemoryAgentToolSessionRepository::findSession(
    const std::string& sessionId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end())
        return common::Result<std::optional<AgentToolSession>>::ok(std::nullopt);
    return common::Result<std::optional<AgentToolSession>>::ok(it->second);
}

common::Result<std::vector<AgentToolSession>> InMemoryAgentToolSessionRepository::listForRun(
    const std::string& runId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AgentToolSession> result;
    for (const auto& [id, s] : sessions_) {
        if (s.runId == runId) result.push_back(s);
    }
    std::sort(result.begin(), result.end(),
        [](const AgentToolSession& a, const AgentToolSession& b) {
            return a.createdAt < b.createdAt;
        });
    return common::Result<std::vector<AgentToolSession>>::ok(std::move(result));
}

common::Result<AgentProviderTurn> InMemoryAgentToolSessionRepository::insertTurn(
    AgentProviderTurn turn)
{
    std::lock_guard<std::mutex> lock(mutex_);
    turns_[turn.sessionId].push_back(turn);
    return common::Result<AgentProviderTurn>::ok(std::move(turn));
}

common::Result<std::vector<AgentProviderTurn>> InMemoryAgentToolSessionRepository::listTurns(
    const std::string& sessionId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = turns_.find(sessionId);
    if (it == turns_.end())
        return common::Result<std::vector<AgentProviderTurn>>::ok({});
    auto result = it->second;
    std::sort(result.begin(), result.end(),
        [](const AgentProviderTurn& a, const AgentProviderTurn& b) {
            return a.turnIndex < b.turnIndex;
        });
    return common::Result<std::vector<AgentProviderTurn>>::ok(std::move(result));
}

common::Result<std::size_t> InMemoryAgentToolSessionRepository::interruptActiveSessions()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::system_clock::now();
    std::size_t count = 0;
    for (auto& [id, s] : sessions_) {
        if (s.state == AgentToolSessionState::Active) {
            s.state      = AgentToolSessionState::Interrupted;
            s.finishedAt = now;
            ++count;
        }
    }
    return common::Result<std::size_t>::ok(count);
}

// ── InMemoryAgentToolAuditRepository ─────────────────────────────────────────

common::Result<void> InMemoryAgentToolAuditRepository::insert(
    const AgentToolAuditRecord& record)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (records_.count(record.auditId))
        return common::Result<void>::failure(
            {"agent_call.duplicate_id", "Audit record already exists"});
    records_.emplace(record.auditId, record);
    return common::Result<void>::ok();
}

common::Result<void> InMemoryAgentToolAuditRepository::update(
    const AgentToolAuditRecord& record)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(record.auditId);
    if (it == records_.end())
        return common::Result<void>::failure(
            {"agent_call.not_found", "Audit record not found"});
    it->second = record;
    return common::Result<void>::ok();
}

common::Result<std::optional<AgentToolAuditRecord>> InMemoryAgentToolAuditRepository::find(
    const std::string& auditId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(auditId);
    if (it == records_.end())
        return common::Result<std::optional<AgentToolAuditRecord>>::ok(std::nullopt);
    return common::Result<std::optional<AgentToolAuditRecord>>::ok(it->second);
}

common::Result<std::vector<AgentToolAuditRecord>> InMemoryAgentToolAuditRepository::listForSession(
    const std::string& sessionId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AgentToolAuditRecord> result;
    for (const auto& [id, r] : records_) {
        if (r.sessionId == sessionId) result.push_back(r);
    }
    std::sort(result.begin(), result.end(),
        [](const AgentToolAuditRecord& a, const AgentToolAuditRecord& b) {
            return a.createdAt < b.createdAt;
        });
    return common::Result<std::vector<AgentToolAuditRecord>>::ok(std::move(result));
}

common::Result<std::size_t> InMemoryAgentToolAuditRepository::interruptRunning(
    const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::system_clock::now();
    std::size_t count = 0;
    for (auto& [id, r] : records_) {
        if (r.sessionId == sessionId &&
            (r.state == AgentToolCallState::Requested || r.state == AgentToolCallState::Running)) {
            r.state      = AgentToolCallState::Interrupted;
            r.finishedAt = now;
            ++count;
        }
    }
    return common::Result<std::size_t>::ok(count);
}

common::Result<std::size_t> InMemoryAgentToolAuditRepository::interruptAllRunning()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::system_clock::now();
    std::size_t count = 0;
    for (auto& [id, r] : records_) {
        if (r.state == AgentToolCallState::Requested || r.state == AgentToolCallState::Running) {
            r.state      = AgentToolCallState::Interrupted;
            r.finishedAt = now;
            ++count;
        }
    }
    return common::Result<std::size_t>::ok(count);
}

} // namespace ai::application
