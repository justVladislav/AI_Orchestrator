// Pure ai::application-layer tests for Stage 7's global active project,
// provider/model catalog, and execution profile foundation. Every scenario
// here uses only in-memory adapters, so it never touches SQLite or the
// filesystem - SqliteRepositoryTests.cpp/BackendRuntimeTests.cpp cover the
// same business rules again against the real persistence adapters and
// through BackendRuntime's storage.unavailable controlled-failure path.
#include "ai/application/ExecutionAssignmentValidation.hpp"
#include "ai/application/catalog/ExecutionProfileService.hpp"
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/ProjectService.hpp"
#include "ai/application/catalog/ProviderCatalogService.hpp"
#include "ai/application/WorkspaceContextService.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace {

namespace fs = std::filesystem;
using namespace ai::application;

fs::path uniqueProjectPath(const char* label)
{
    return fs::temp_directory_path()
        / (std::string("ai-orchestrator-workspace-catalog-test-") + label + "-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

Project makeActiveProject(ProjectService& projectService, const char* label)
{
    const fs::path path = uniqueProjectPath(label);
    std::error_code ignored;
    fs::create_directories(path, ignored);
    const auto created = projectService.createProject({label, "Workspace/catalog test project", path});
    assert(created.success());
    return created.value();
}

void testSelectAndClearActiveProject()
{
    InMemoryProjectRepository projects;
    InMemoryWorkspaceStateRepository state;
    ProjectService projectService(projects);
    WorkspaceContextService workspace(state, projects);

    // Nothing selected yet.
    const auto initial = workspace.activeProjectId();
    assert(initial.success() && !initial.value().has_value());
    const auto initialProject = workspace.activeProject();
    assert(initialProject.success() && !initialProject.value().has_value());

    const Project project = makeActiveProject(projectService, "select");
    const auto selected = workspace.selectProject(project.id);
    assert(selected.success());

    const auto activeId = workspace.activeProjectId();
    assert(activeId.success() && activeId.value().has_value() && *activeId.value() == project.id);
    const auto activeProject = workspace.activeProject();
    assert(activeProject.success() && activeProject.value().has_value());
    assert(activeProject.value()->id == project.id);
    // Description must come from the live Project aggregate, not a copy.
    assert(activeProject.value()->description == project.description);

    // Re-selecting the same already-active project is safe (idempotent).
    const auto reselected = workspace.selectProject(project.id);
    assert(reselected.success());
    const auto stillActive = workspace.activeProjectId();
    assert(stillActive.success() && stillActive.value().has_value() && *stillActive.value() == project.id);

    const auto cleared = workspace.clearActiveProject();
    assert(cleared.success());
    const auto afterClear = workspace.activeProjectId();
    assert(afterClear.success() && !afterClear.value().has_value());
}

void testSelectUnknownProjectIsRejected()
{
    InMemoryProjectRepository projects;
    InMemoryWorkspaceStateRepository state;
    WorkspaceContextService workspace(state, projects);

    const auto selected = workspace.selectProject("does-not-exist");
    assert(!selected.success());
    assert(selected.error()->code == "workspace.project_not_found");
    const auto activeId = workspace.activeProjectId();
    assert(activeId.success() && !activeId.value().has_value());
}

void testSelectArchivedOrUnavailableProjectIsRejected()
{
    InMemoryProjectRepository projects;
    InMemoryWorkspaceStateRepository state;
    ProjectService projectService(projects);
    WorkspaceContextService workspace(state, projects);

    const Project archivable = makeActiveProject(projectService, "archive");
    const auto archived = projectService.archiveProject(archivable.id);
    assert(archived.success());
    assert(archived.value().status == ProjectStatus::Archived);

    const auto selectArchived = workspace.selectProject(archivable.id);
    assert(!selectArchived.success());
    assert(selectArchived.error()->code == "workspace.project_not_active");

    // Directly persist an Unavailable project (no application-layer command
    // currently produces this status; it exists for completeness in
    // ProjectStatus and must still be rejected the same way as Archived).
    Project unavailable = makeActiveProject(projectService, "unavailable-source");
    unavailable.status = ProjectStatus::Unavailable;
    const auto updated = projects.update(unavailable);
    assert(updated.success());
    const auto selectUnavailable = workspace.selectProject(unavailable.id);
    assert(!selectUnavailable.success());
    assert(selectUnavailable.error()->code == "workspace.project_not_active");
}

void testStaleActiveProjectReferenceSelfHeals()
{
    InMemoryProjectRepository projects;
    InMemoryWorkspaceStateRepository state;
    ProjectService projectService(projects);
    WorkspaceContextService workspace(state, projects);

    const Project project = makeActiveProject(projectService, "stale");
    assert(workspace.selectProject(project.id).success());

    // Simulate "the project became unavailable after it was selected"
    // without going through selectProject again.
    Project archived = project;
    archived.status = ProjectStatus::Archived;
    assert(projects.update(archived).success());

    // Startup/read-time self-heal: no crash, no dangling id, state clears.
    const auto activeProject = workspace.activeProject();
    assert(activeProject.success() && !activeProject.value().has_value());
    const auto activeId = workspace.activeProjectId();
    assert(activeId.success() && !activeId.value().has_value());

    // A raw repository read (bypassing the service) confirms the stored
    // state itself was actually cleared, not just filtered on the way out.
    const auto rawState = state.activeProjectId();
    assert(rawState.success() && !rawState.value().has_value());
}

Provider makeProvider(ProviderCatalogService& catalog, const char* name, const char* kind = "openai")
{
    const auto created = catalog.createProvider({name, kind, "https://example.invalid", true, std::nullopt});
    assert(created.success());
    const auto found = catalog.findProvider(created.value());
    assert(found.success() && found.value().has_value());
    return *found.value();
}

void testProviderCreateUpdateEnableList()
{
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    ProviderCatalogService catalog(providers, models);

    const auto badKind = catalog.createProvider({"Bad", "not-a-kind", "https://x.invalid", true, std::nullopt});
    assert(!badKind.success() && badKind.error()->code == "provider.unknown_kind");

    const Provider provider = makeProvider(catalog, "OpenAI Primary");
    assert(provider.enabled);
    assert(!provider.credentialReference.has_value());

    const auto updated = catalog.updateProvider(
        {provider.id, "OpenAI Primary (renamed)", "openai", "https://api.example.invalid", "cred-ref-1"});
    assert(updated.success());
    const auto reloaded = catalog.findProvider(provider.id);
    assert(reloaded.success() && reloaded.value().has_value());
    assert(reloaded.value()->name == "OpenAI Primary (renamed)");
    assert(reloaded.value()->credentialReference.has_value() && *reloaded.value()->credentialReference == "cred-ref-1");

    const auto disabled = catalog.setProviderEnabled(provider.id, false);
    assert(disabled.success());
    const auto afterDisable = catalog.findProvider(provider.id);
    assert(afterDisable.success() && afterDisable.value().has_value() && !afterDisable.value()->enabled);

    const Provider second = makeProvider(catalog, "Anthropic Primary", "anthropic");
    const auto listed = catalog.listProviders();
    assert(listed.success() && listed.value().size() == 2);
    assert(listed.value()[0].id == provider.id && listed.value()[1].id == second.id);
}

Model makeModel(
    ProviderCatalogService& catalog,
    const ProviderId& providerId,
    const char* apiModelId,
    bool enabled = true)
{
    const auto created = catalog.createModel(
        {providerId, apiModelId, apiModelId, 128000, 4096, 1000000, 3000000, 0, 0, enabled});
    assert(created.success());
    const auto found = catalog.findModel(created.value());
    assert(found.success() && found.value().has_value());
    return *found.value();
}

void testModelCreateUpdateEnableDefaultList()
{
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    ProviderCatalogService catalog(providers, models);
    const Provider provider = makeProvider(catalog, "Model Test Provider");

    const auto missingProvider = catalog.createModel(
        {"no-such-provider", "gpt-x", "GPT X", 1000, 100, 0, 0, true});
    assert(!missingProvider.success() && missingProvider.error()->code == "provider.not_found");

    const Model model = makeModel(catalog, provider.id, "gpt-x");
    assert(!model.isDefault);
    assert(model.enabled);

    // Duplicate apiModelId within the same provider is rejected.
    const auto duplicate = catalog.createModel(
        {provider.id, "gpt-x", "Duplicate", 1000, 100, 0, 0, true});
    assert(!duplicate.success() && duplicate.error()->code == "model.duplicate_api_model_id");

    const Model second = makeModel(catalog, provider.id, "gpt-y");

    const auto updated = catalog.updateModel(
        {model.id, "gpt-x-updated", "GPT X Updated", 200000, 8192, 500000, 1500000});
    assert(updated.success());
    const auto reloaded = catalog.findModel(model.id);
    assert(reloaded.success() && reloaded.value()->apiModelId == "gpt-x-updated");
    assert(reloaded.value()->displayName == "GPT X Updated");
    assert(reloaded.value()->contextWindowTokens == 200000);

    // Renaming to a value that collides with a sibling model is rejected.
    const auto collide = catalog.updateModel(
        {model.id, "gpt-y", "Collides", 1, 1, 0, 0});
    assert(!collide.success() && collide.error()->code == "model.duplicate_api_model_id");

    const auto listed = catalog.listModels(provider.id);
    assert(listed.success() && listed.value().size() == 2);
    const auto listedAll = catalog.listModels();
    assert(listedAll.success() && listedAll.value().size() == 2);

    // A disabled model cannot become the default.
    const Model disabledModel = makeModel(catalog, provider.id, "gpt-z", /*enabled=*/false);
    const auto defaultOnDisabled = catalog.setDefaultModel(provider.id, disabledModel.id);
    assert(!defaultOnDisabled.success() && defaultOnDisabled.error()->code == "model.disabled_cannot_be_default");

    // A model belonging to a different provider is rejected.
    const Provider otherProvider = makeProvider(catalog, "Other Provider", "anthropic");
    const Model otherModel = makeModel(catalog, otherProvider.id, "claude-x");
    const auto crossProvider = catalog.setDefaultModel(provider.id, otherModel.id);
    assert(!crossProvider.success() && crossProvider.error()->code == "model.provider_mismatch");

    // Atomic default switch: setting a new default clears the previous one.
    const auto firstDefault = catalog.setDefaultModel(provider.id, model.id);
    assert(firstDefault.success());
    assert(catalog.findModel(model.id).value()->isDefault);
    const auto secondDefault = catalog.setDefaultModel(provider.id, second.id);
    assert(secondDefault.success());
    assert(catalog.findModel(second.id).value()->isDefault);
    assert(!catalog.findModel(model.id).value()->isDefault);

    // A default model cannot be disabled without first choosing a new default.
    const auto disableDefault = catalog.setModelEnabled(second.id, false);
    assert(!disableDefault.success() && disableDefault.error()->code == "model.default_must_stay_enabled");

    // Disabling the provider must not touch its models.
    assert(catalog.setProviderEnabled(provider.id, false).success());
    assert(catalog.findModel(model.id).value()->enabled);
    assert(catalog.findModel(second.id).value()->enabled);

    // The current default (second) cannot be deleted without first choosing
    // a new default - same rule as disabling it.
    const auto deleteDefault = catalog.deleteModel(second.id);
    assert(!deleteDefault.success() && deleteDefault.error()->code == "model.default_cannot_be_deleted");

    // A non-default model with no referencing rows (nothing to enforce in
    // this InMemory store - SqliteRepositoryTests covers the FK-enforced
    // "model.in_use" path) deletes cleanly and is gone afterward.
    const auto deleted = catalog.deleteModel(model.id);
    assert(deleted.success());
    assert(catalog.findModel(model.id).success() && !catalog.findModel(model.id).value().has_value());

    const auto deleteMissing = catalog.deleteModel(model.id);
    assert(!deleteMissing.success() && deleteMissing.error()->code == "model.not_found");
}

// AG-17 gpt-6-sol checkpoint M2a: the optional long-context price tier of a model.
// threshold 0 = flat (all five long prices must be 0); threshold > 0 = tier, strictly
// below the context window; a long price of 0 = "not entered" (allowed, partially too).
void testModelLongContextPricingRules()
{
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    ProviderCatalogService catalog(providers, models);
    const Provider provider = makeProvider(catalog, "Tier Provider");

    // Every existing caller creates a FLAT model: no tier at all.
    const Model flat = makeModel(catalog, provider.id, "flat-model");
    assert(!flat.longContextPricing.enabled());
    assert(flat.longContextPricing == LongContextPricing {});

    // Distinct values per field, so a swapped field cannot pass.
    const LongContextPricing tier {
        272'000, 4'000'000, 15'000'000, 5'000'000, 400'000, 8'000'000};
    const auto create = [&](const char* apiModelId, std::uint64_t contextWindow,
                            const LongContextPricing& pricing) {
        CreateModelRequest request;
        request.providerId = provider.id;
        request.apiModelId = apiModelId;
        request.displayName = apiModelId;
        request.contextWindowTokens = contextWindow;
        request.maxOutputTokens = 128'000;
        request.inputPriceMicrosPerMillionTokens = 2'000'000;
        request.outputPriceMicrosPerMillionTokens = 10'000'000;
        request.cacheWritePriceMicrosPerMillionTokens = 2'500'000;
        request.cacheReadPriceMicrosPerMillionTokens = 200'000;
        request.longContextPricing = pricing;
        request.cacheWriteOneHourPriceMicrosPerMillionTokens = 4'000'000;
        return catalog.createModel(request);
    };
    const auto countModels = [&] { return catalog.listModels(provider.id).value().size(); };

    // A valid tier is stored exactly, next to untouched flat prices.
    const auto created = create("tiered-model", 1'050'000, tier);
    assert(created.success());
    const Model tiered = *catalog.findModel(created.value()).value();
    assert(tiered.longContextPricing == tier);
    assert(tiered.longContextPricing.thresholdInputTokens == 272'000);
    assert(tiered.longContextPricing.inputPriceMicrosPerMillionTokens == 4'000'000);
    assert(tiered.longContextPricing.outputPriceMicrosPerMillionTokens == 15'000'000);
    assert(tiered.longContextPricing.cacheWritePriceMicrosPerMillionTokens == 5'000'000);
    assert(tiered.longContextPricing.cacheReadPriceMicrosPerMillionTokens == 400'000);
    assert(tiered.longContextPricing.cacheWriteOneHourPriceMicrosPerMillionTokens == 8'000'000);
    assert(tiered.inputPriceMicrosPerMillionTokens == 2'000'000);
    assert(tiered.cacheReadPriceMicrosPerMillionTokens == 200'000);
    assert(tiered.cacheWriteOneHourPriceMicrosPerMillionTokens == 4'000'000);

    // The threshold is STRICTLY below the context window; a window that was not entered
    // cannot hold a tier at all.
    assert(create("edge-ok", 1'000, {999, 0, 0, 0, 0}).success());
    const auto atWindow = create("edge-eq", 1'000, {1'000, 0, 0, 0, 0});
    assert(!atWindow.success() && atWindow.error()->code == "model.long_context_threshold_out_of_range");
    const auto pastWindow = create("edge-gt", 1'000, {1'001, 0, 0, 0, 0});
    assert(!pastWindow.success() && pastWindow.error()->code == "model.long_context_threshold_out_of_range");
    const auto noWindow = create("no-window", 0, {1, 0, 0, 0, 0});
    assert(!noWindow.success() && noWindow.error()->code == "model.long_context_threshold_out_of_range");

    // Threshold 0 means flat: ANY non-zero long price would be silently ignored, so each is refused.
    for (const LongContextPricing& orphan : {
             LongContextPricing {0, 1, 0, 0, 0}, LongContextPricing {0, 0, 1, 0, 0},
             LongContextPricing {0, 0, 0, 1, 0}, LongContextPricing {0, 0, 0, 0, 1},
             LongContextPricing {0, 0, 0, 0, 0, 1}}) {
        const auto refused = create("orphan-prices", 1'050'000, orphan);
        assert(!refused.success()
            && refused.error()->code == "model.long_context_prices_without_threshold");
    }

    // A zero long price is "not entered": an enabled tier with no price at all, or with only
    // some of them, is allowed (the cost step reports such a Response as unknown).
    assert(create("unpriced-tier", 1'050'000, {272'000, 0, 0, 0, 0}).success());
    assert(create("partial-tier", 1'050'000, {272'000, 4'000'000, 0, 0, 400'000}).success());

    // Every refusal above created nothing.
    assert(countModels() == 1 + 1 + 1 + 1 + 1);   // flat, tiered, edge-ok, unpriced, partial

    // UPDATE. A caller that knows nothing about the tier - the shape every existing UI
    // price/metadata edit uses, nine positional values - must never wipe it.
    assert(catalog.updateModel({tiered.id, "tiered-model", "Renamed", 1'050'000, 128'000,
                                2'500'000, 11'000'000, 2'600'000, 210'000}).success());
    const Model afterBlindEdit = *catalog.findModel(tiered.id).value();
    assert(afterBlindEdit.longContextPricing == tier);
    assert(afterBlindEdit.cacheWriteOneHourPriceMicrosPerMillionTokens == 4'000'000);
    assert(afterBlindEdit.displayName == "Renamed");
    assert(afterBlindEdit.inputPriceMicrosPerMillionTokens == 2'500'000);
    assert(afterBlindEdit.cacheReadPriceMicrosPerMillionTokens == 210'000);

    // A value REPLACES the tier; an all-zero value removes it.
    const LongContextPricing replacement {
        500'000, 8'000'000, 20'000'000, 9'000'000, 700'000, 12'000'000};
    UpdateModelRequest replace {tiered.id, "tiered-model", "Renamed", 1'050'000, 128'000,
                                2'500'000, 11'000'000, 2'600'000, 210'000, replacement};
    assert(catalog.updateModel(replace).success());
    assert(catalog.findModel(tiered.id).value()->longContextPricing == replacement);
    replace.cacheWriteOneHourPriceMicrosPerMillionTokens = 6'000'000;
    assert(catalog.updateModel(replace).success());
    assert(catalog.findModel(tiered.id).value()->cacheWriteOneHourPriceMicrosPerMillionTokens
        == 6'000'000);
    UpdateModelRequest clear = replace;
    clear.longContextPricing = LongContextPricing {};
    assert(catalog.updateModel(clear).success());
    assert(catalog.findModel(tiered.id).value()->longContextPricing == LongContextPricing {});

    // Refused updates leave the stored model exactly as it was.
    assert(catalog.updateModel(replace).success());
    const Model beforeRefusals = *catalog.findModel(tiered.id).value();
    UpdateModelRequest orphanUpdate = replace;
    orphanUpdate.longContextPricing = LongContextPricing {0, 5, 0, 0, 0};
    orphanUpdate.displayName = "Must not be saved";
    const auto orphanRefused = catalog.updateModel(orphanUpdate);
    assert(!orphanRefused.success()
        && orphanRefused.error()->code == "model.long_context_prices_without_threshold");
    UpdateModelRequest windowUpdate = replace;
    windowUpdate.longContextPricing = LongContextPricing {1'050'000, 0, 0, 0, 0};
    windowUpdate.displayName = "Must not be saved";
    const auto windowRefused = catalog.updateModel(windowUpdate);
    assert(!windowRefused.success()
        && windowRefused.error()->code == "model.long_context_threshold_out_of_range");
    // Lowering the context window to or below the STORED threshold (500,000) is refused too.
    UpdateModelRequest shrink {tiered.id, "tiered-model", "Must not be saved", 500'000, 128'000,
                               2'500'000, 11'000'000, 2'600'000, 210'000};
    const auto shrinkRefused = catalog.updateModel(shrink);
    assert(!shrinkRefused.success()
        && shrinkRefused.error()->code == "model.long_context_threshold_out_of_range");
    const Model afterRefusals = *catalog.findModel(tiered.id).value();
    assert(afterRefusals.longContextPricing == beforeRefusals.longContextPricing);
    assert(afterRefusals.displayName == beforeRefusals.displayName);
    assert(afterRefusals.contextWindowTokens == beforeRefusals.contextWindowTokens);
    // ...while shrinking the window AND removing the tier together is a valid edit.
    shrink.displayName = "Shrunk";
    shrink.longContextPricing = LongContextPricing {};
    assert(catalog.updateModel(shrink).success());
    assert(catalog.findModel(tiered.id).value()->contextWindowTokens == 500'000);
    assert(!catalog.findModel(tiered.id).value()->longContextPricing.enabled());

    // Enabling/disabling and choosing the default never touch the tier.
    assert(catalog.updateModel(replace).success());
    assert(catalog.setModelEnabled(tiered.id, false).success());
    assert(catalog.setModelEnabled(tiered.id, true).success());
    assert(catalog.setDefaultModel(provider.id, tiered.id).success());
    assert(catalog.findModel(tiered.id).value()->longContextPricing == replacement);
}

// The ONE rule set of a long-context tier, shared by the catalog service (on save) and the SQLite
// layer (the v59 migration and every read): exercised directly, at its exact edges.
void testLongContextTierRuleSetIsSharedAndExact()
{
    using Problem = LongContextPricingProblem;
    const LongContextPricing flat {};
    assert(checkLongContextPricing(0, flat) == Problem::None);
    assert(checkLongContextPricing(400'000, flat) == Problem::None);

    // Threshold 0 = flat: each of the five prices alone makes the tier invalid, whatever the window.
    for (const LongContextPricing& orphan : {
             LongContextPricing {0, 1, 0, 0, 0}, LongContextPricing {0, 0, 1, 0, 0},
             LongContextPricing {0, 0, 0, 1, 0}, LongContextPricing {0, 0, 0, 0, 1},
             LongContextPricing {0, 0, 0, 0, 0, 1}}) {
        assert(checkLongContextPricing(1'050'000, orphan) == Problem::PricesWithoutThreshold);
        assert(checkLongContextPricing(0, orphan) == Problem::PricesWithoutThreshold);
    }

    // Threshold > 0: STRICTLY below the window; a window that was not entered holds no tier.
    assert(checkLongContextPricing(1'000, {999, 0, 0, 0, 0}) == Problem::None);
    assert(checkLongContextPricing(1'000, {1'000, 0, 0, 0, 0}) == Problem::ThresholdOutOfRange);
    assert(checkLongContextPricing(1'000, {1'001, 0, 0, 0, 0}) == Problem::ThresholdOutOfRange);
    assert(checkLongContextPricing(0, {1, 0, 0, 0, 0}) == Problem::ThresholdOutOfRange);
    // Prices never matter for the range: unpriced, partial and fully priced tiers are all fine.
    assert(checkLongContextPricing(1'050'000, {272'000, 0, 0, 0, 0}) == Problem::None);
    assert(checkLongContextPricing(1'050'000, {272'000, 4'000'000, 0, 0, 400'000}) == Problem::None);
    assert(checkLongContextPricing(1'050'000, {272'000, 4, 15, 5, 400}) == Problem::None);
    const std::uint64_t huge = ~std::uint64_t {0};
    assert(checkLongContextPricing(huge, {huge - 1, 1, 1, 1, 1}) == Problem::None);
    assert(checkLongContextPricing(huge, {huge, 1, 1, 1, 1}) == Problem::ThresholdOutOfRange);

    // Typed codes and messages: stable, distinct, and empty for None.
    assert(std::string(longContextPricingProblemCode(Problem::PricesWithoutThreshold))
        == "model.long_context_prices_without_threshold");
    assert(std::string(longContextPricingProblemCode(Problem::ThresholdOutOfRange))
        == "model.long_context_threshold_out_of_range");
    assert(std::string(longContextPricingProblemCode(Problem::None)).empty());
    assert(std::string(longContextPricingProblemMessage(Problem::None)).empty());
    assert(std::string(longContextPricingProblemMessage(Problem::PricesWithoutThreshold))
        != std::string(longContextPricingProblemMessage(Problem::ThresholdOutOfRange)));
}

void testExecutionProfileValidation()
{
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    InMemoryExecutionProfileRepository profiles;
    ProviderCatalogService catalog(providers, models);
    ExecutionProfileService profileService(profiles, providers, models);

    const Provider provider = makeProvider(catalog, "Profile Provider");
    const Model model = makeModel(catalog, provider.id, "profile-model");

    const auto emptyName = profileService.createProfile({"", "mock-sequential-v1", true, {}});
    assert(!emptyName.success() && emptyName.error()->code == "execution_profile.empty_name");

    const auto emptyWorkflow = profileService.createProfile({"Profile", "", true, {}});
    assert(!emptyWorkflow.success() && emptyWorkflow.error()->code == "execution_profile.empty_workflow_id");

    StageModelAssignment unknownStage {"unknown", provider.id, model.id, std::chrono::milliseconds {1000}};
    const auto badStageType = profileService.createProfile(
        {"Profile", "mock-sequential-v1", true, {unknownStage}});
    assert(!badStageType.success() && badStageType.error()->code == "execution_profile.unknown_stage_type");

    StageModelAssignment architect {"architect", provider.id, model.id, std::chrono::milliseconds {1000}};
    StageModelAssignment duplicateArchitect = architect;
    const auto duplicateStage = profileService.createProfile(
        {"Profile", "mock-sequential-v1", true, {architect, duplicateArchitect}});
    assert(!duplicateStage.success() && duplicateStage.error()->code == "execution_profile.duplicate_stage_type");

    StageModelAssignment zeroTimeout {"architect", provider.id, model.id, std::chrono::milliseconds {0}};
    const auto badTimeout = profileService.createProfile(
        {"Profile", "mock-sequential-v1", true, {zeroTimeout}});
    assert(!badTimeout.success() && badTimeout.error()->code == "execution_profile.invalid_timeout");

    StageModelAssignment unknownProvider {"architect", "no-such-provider", model.id, std::chrono::milliseconds {1000}};
    const auto missingProvider = profileService.createProfile(
        {"Profile", "mock-sequential-v1", true, {unknownProvider}});
    assert(!missingProvider.success() && missingProvider.error()->code == "provider.not_found");

    assert(catalog.setProviderEnabled(provider.id, false).success());
    const auto disabledProvider = profileService.createProfile(
        {"Profile", "mock-sequential-v1", true, {architect}});
    assert(!disabledProvider.success() && disabledProvider.error()->code == "provider.disabled");
    assert(catalog.setProviderEnabled(provider.id, true).success());

    const Provider otherProvider = makeProvider(catalog, "Mismatch Provider");
    StageModelAssignment mismatched {"architect", otherProvider.id, model.id, std::chrono::milliseconds {1000}};
    const auto mismatch = profileService.createProfile(
        {"Profile", "mock-sequential-v1", true, {mismatched}});
    assert(!mismatch.success() && mismatch.error()->code == "model.provider_mismatch");

    const Model disabledModel = makeModel(catalog, provider.id, "disabled-model", /*enabled=*/false);
    StageModelAssignment disabledAssignment {"architect", provider.id, disabledModel.id, std::chrono::milliseconds {1000}};
    const auto disabledModelResult = profileService.createProfile(
        {"Profile", "mock-sequential-v1", true, {disabledAssignment}});
    assert(!disabledModelResult.success() && disabledModelResult.error()->code == "model.disabled");

    // A fully valid profile with all three stage types succeeds.
    StageModelAssignment implementation {"implementation", provider.id, model.id, std::chrono::milliseconds {2000}};
    StageModelAssignment documentation {"documentation", provider.id, model.id, std::chrono::milliseconds {1500}};

    const auto manifestWithManyAssignments = profileService.createProfile(
        {"Invalid Manifest Profile", kExecutionManifestWorkflowId, true,
            {architect, implementation}});
    assert(!manifestWithManyAssignments.success()
        && manifestWithManyAssignments.error()->code
            == "execution_profile.manifest_requires_single_architect");

    const auto manifestWithoutArchitect = profileService.createProfile(
        {"Invalid Manifest Profile", kExecutionManifestWorkflowId, true,
            {implementation}});
    assert(!manifestWithoutArchitect.success()
        && manifestWithoutArchitect.error()->code
            == "execution_profile.manifest_requires_single_architect");

    // A saved profile persists only stage/provider/model/timeout: a roster's lead
    // flag, workspace access and positions would be lost on the way to SQLite, so
    // even a roster-shaped Manifest profile is refused up front (AG-17.6). A
    // multi-model roster is configured per Run.
    auto rosterLead = architect;
    rosterLead.leadArchitect = true;
    rosterLead.position = 0;
    auto rosterWorker = implementation;
    rosterWorker.workspaceAccess = true;
    rosterWorker.position = 1;
    const auto rosterAsSavedProfile = profileService.createProfile(
        {"Manifest Roster Profile", kExecutionManifestWorkflowId, true,
            {rosterLead, rosterWorker}});
    assert(!rosterAsSavedProfile.success()
        && rosterAsSavedProfile.error()->code
            == "execution_profile.manifest_requires_single_architect");

    // The per-Run Manifest roster rules (validateManifestRoster).
    assert(validateManifestRoster({rosterLead, rosterWorker}).success());
    assert(validateManifestRoster({architect}).success());   // one member = single assignment
    {
        const auto empty = validateManifestRoster({});
        assert(!empty.success() && empty.error()->code == "run.manifest_roster_empty");
        auto noLead = rosterLead;
        noLead.leadArchitect = false;
        const auto leadless = validateManifestRoster({noLead, rosterWorker});
        assert(!leadless.success()
            && leadless.error()->code == "run.manifest_roster_requires_one_lead");
        // A roster is a pool of available workers, but at least one must be able to
        // take a workspace task: neither the lead (even with workspace access) nor a
        // member without workspace access is ever an assignee.
        auto singleShotWorker = rosterWorker;
        singleShotWorker.workspaceAccess = false;
        const auto nobody = validateManifestRoster({rosterLead, singleShotWorker});
        assert(!nobody.success()
            && nobody.error()->code == "run.manifest_roster_requires_workspace_worker");
        auto toolingLead = rosterLead;
        toolingLead.workspaceAccess = true;
        const auto toolingOnly = validateManifestRoster({toolingLead, singleShotWorker});
        assert(!toolingOnly.success()
            && toolingOnly.error()->code == "run.manifest_roster_requires_workspace_worker");
        auto duplicatePosition = rosterWorker;
        duplicatePosition.position = rosterLead.position;
        const auto invalidRouting = validateManifestRoster({rosterLead, duplicatePosition});
        assert(!invalidRouting.success()
            && invalidRouting.error()->code == "run.manifest_roster_invalid");
    }

    const auto manifestProfile = profileService.createProfile(
        {"Manifest Profile", kExecutionManifestWorkflowId, true, {architect}});
    assert(manifestProfile.success());

    const auto created = profileService.createProfile(
        {"Full Profile", "mock-sequential-v1", true, {architect, implementation, documentation}});
    assert(created.success());
    const auto found = profileService.findProfile(created.value());
    assert(found.success() && found.value().has_value());
    assert(found.value()->assignments.size() == 3);
    assert(!found.value()->isDefault);

    // At most one default profile: setting a second default clears the first.
    const auto secondProfile = profileService.createProfile(
        {"Second Profile", "mock-sequential-v1", true, {architect}});
    assert(secondProfile.success());
    assert(profileService.setDefaultProfile(created.value()).success());
    assert(profileService.findProfile(created.value()).value()->isDefault);
    assert(profileService.setDefaultProfile(secondProfile.value()).success());
    assert(profileService.findProfile(secondProfile.value()).value()->isDefault);
    assert(!profileService.findProfile(created.value()).value()->isDefault);

    const auto listed = profileService.listProfiles();
    assert(listed.success() && listed.value().size() == 3);
}

} // namespace

int runWorkspaceAndCatalogTests()
{
    testSelectAndClearActiveProject();
    testSelectUnknownProjectIsRejected();
    testSelectArchivedOrUnavailableProjectIsRejected();
    testStaleActiveProjectReferenceSelfHeals();
    testProviderCreateUpdateEnableList();
    testModelCreateUpdateEnableDefaultList();
    testModelLongContextPricingRules();
    testLongContextTierRuleSetIsSharedAndExact();
    testExecutionProfileValidation();
    return 0;
}
