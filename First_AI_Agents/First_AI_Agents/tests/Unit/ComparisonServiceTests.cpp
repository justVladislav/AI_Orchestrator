#include "ai/application/ComparisonService.hpp"
#include "ai/application/ComparisonReadModelService.hpp"
#include "ai/application/ComparisonExecutionManager.hpp"
#include "ai/application/InMemoryComparisonSessionRepository.hpp"
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/ProjectService.hpp"
#include "ai/application/ProviderCatalogService.hpp"

#include <cassert>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <filesystem>

namespace {

using namespace ai::application;
using namespace std::chrono_literals;
namespace common = ai::common;

struct ConcurrencyProbe {
    std::atomic<int> active {0};
    std::atomic<int> maximum {0};
};

class ComparisonFakeClient final : public IProviderClient {
public:
    ComparisonFakeClient(
        std::shared_ptr<ConcurrencyProbe> probe,
        std::string label,
        bool blockUntilCancelled = false)
        : probe_(std::move(probe))
        , label_(std::move(label))
        , blockUntilCancelled_(blockUntilCancelled)
    {
    }

    common::Result<ProviderChatResponse> send(
        const ProviderChatRequest& request,
        std::stop_token stopToken) override
    {
        const int active = probe_->active.fetch_add(1) + 1;
        int observed = probe_->maximum.load();
        while (active > observed
            && !probe_->maximum.compare_exchange_weak(observed, active)) {
        }

        if (blockUntilCancelled_) {
            while (!stopToken.stop_requested()) std::this_thread::sleep_for(2ms);
            probe_->active.fetch_sub(1);
            return common::Result<ProviderChatResponse>::failure(
                {"provider.cancelled", "Cancelled by test"});
        }
        std::this_thread::sleep_for(60ms);
        probe_->active.fetch_sub(1);

        ProviderChatResponse response;
        response.content =
            "# Solution\n\n- Implement the requested contract.\n"
            "- Explain trade-offs and validation.\n\n"
            + label_ + " " + request.apiModelId + ". "
            + std::string(1'500, 'x');
        response.usage.inputTokens = 100;
        response.usage.outputTokens = 50;
        return common::Result<ProviderChatResponse>::ok(std::move(response));
    }

private:
    std::shared_ptr<ConcurrencyProbe> probe_;
    std::string label_;
    bool blockUntilCancelled_;
};

class ComparisonFakeClientFactory final : public IProviderClientFactory {
public:
    void add(ProviderId providerId, std::shared_ptr<IProviderClient> client)
    {
        clients_[std::move(providerId)] = std::move(client);
    }

    common::Result<std::shared_ptr<IProviderClient>> clientFor(
        const ProviderId& providerId,
        const std::string&) override
    {
        const auto found = clients_.find(providerId);
        if (found == clients_.end()) {
            return common::Result<std::shared_ptr<IProviderClient>>::failure(
                {"provider.missing", "No fake comparison client"});
        }
        return common::Result<std::shared_ptr<IProviderClient>>::ok(found->second);
    }

private:
    std::unordered_map<ProviderId, std::shared_ptr<IProviderClient>> clients_;
};

struct ComparisonFixture {
    InMemoryProjectRepository projects;
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    InMemoryComparisonSessionRepository sessions;
    InMemoryModelExecutionRepository executions;
    ProjectService projectService {projects};
    ProviderCatalogService catalog {providers, models};
    ModelExecutionService executionService {executions};
    ComparisonService service {sessions, projects, catalog, executionService};
    ComparisonReadModelService readModel {service, executionService, catalog};
    std::filesystem::path root;
    ProjectId projectId;
    ProviderId providerA;
    ProviderId providerB;
    ModelId modelA;
    ModelId modelB;

    ComparisonFixture()
    {
        root = std::filesystem::temp_directory_path()
            / ("comparison-service-test-"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        const auto project = projectService.createProject(
            {"Comparison", "Shared project context", root});
        assert(project.success());
        projectId = project.value().id;

        const auto firstProvider = catalog.createProvider(
            {"Provider A", "openai", "https://provider-a.invalid", true, std::nullopt});
        const auto secondProvider = catalog.createProvider(
            {"Provider B", "anthropic", "https://provider-b.invalid", true, std::nullopt});
        assert(firstProvider.success() && secondProvider.success());
        providerA = firstProvider.value();
        providerB = secondProvider.value();

        const auto firstModel = catalog.createModel(
            {providerA, "model-a", "Model A", 100000, 4096, 1, 2, 0, 0, true});
        const auto secondModel = catalog.createModel(
            {providerB, "model-b", "Model B", 100000, 4096, 1, 2, 0, 0, true});
        assert(firstModel.success() && secondModel.success());
        modelA = firstModel.value();
        modelB = secondModel.value();
    }

    ~ComparisonFixture()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

CreateComparisonSessionRequest validRequest(const ComparisonFixture& fixture)
{
    return {
        fixture.projectId,
        "Compare implementations",
        "Implement the same contract and explain trade-offs",
        "context-snapshot-hash",
        {{fixture.providerA, fixture.modelA, "candidate"},
         {fixture.providerB, fixture.modelB, "candidate"}}};
}

void testCreateSessionSeedsSharedExecutionLedger()
{
    ComparisonFixture fixture;
    const auto created = fixture.service.createSession(validRequest(fixture));
    assert(created.success());
    assert(created.value().status == ComparisonSessionStatus::Pending);
    assert(!created.value().requestHash.empty());
    assert(created.value().contextHash == "context-snapshot-hash");

    const auto results = fixture.service.listResults(created.value().id);
    assert(results.success() && results.value().size() == 2);
    assert(results.value()[0].source == ModelExecutionSource::Comparison);
    assert(results.value()[0].sourceId == created.value().id);
    assert(results.value()[0].position == 0 && results.value()[1].position == 1);
    assert(results.value()[0].requestHash == created.value().requestHash);
}

void testSelectionRequiresReviewableSessionResults()
{
    ComparisonFixture fixture;
    const auto created = fixture.service.createSession(validRequest(fixture));
    assert(created.success());
    const auto results = fixture.service.listResults(created.value().id);
    assert(results.success() && results.value().size() == 2);

    const auto started = fixture.service.startSession(created.value().id);
    assert(started.success());
    for (const auto& result : results.value()) {
        const auto executionStarted = fixture.executionService.startExecution(result.id);
        assert(executionStarted.success());
        FinishModelExecutionRequest outcome;
        outcome.status = ModelExecutionStatus::Completed;
        outcome.outputPreview = "Comparable output";
        const auto executionFinished = fixture.executionService.finishExecution(result.id, outcome);
        assert(executionFinished.success());
    }

    const auto selected = fixture.service.setSynthesisSelection(
        created.value().id, {results.value()[0].id, results.value()[1].id});
    assert(selected.success() && selected.value().synthesisSelection.size() == 2);
    const auto preferred = fixture.service.setPreferredExecution(
        created.value().id, results.value()[1].id);
    assert(preferred.success()
        && preferred.value().preferredExecutionId == results.value()[1].id);
    const auto finished = fixture.service.finishSession(
        created.value().id, ComparisonSessionStatus::Completed);
    assert(finished.success());
}

void testInvalidComparisonRosterIsRejected()
{
    ComparisonFixture fixture;
    auto request = validRequest(fixture);
    request.modelSlots.resize(1);
    const auto tooSmall = fixture.service.createSession(request);
    assert(!tooSmall.success()
        && tooSmall.error()->code == "comparison.insufficient_candidates");

    request = validRequest(fixture);
    request.modelSlots[1] = request.modelSlots[0];
    const auto duplicate = fixture.service.createSession(request);
    assert(!duplicate.success()
        && duplicate.error()->code == "comparison.duplicate_model_slot");
}

void testReadModelOwnsTotalsAndDeterministicRecommendation()
{
    ComparisonFixture fixture;
    const auto created = fixture.service.createSession(validRequest(fixture));
    assert(created.success());
    const auto candidates = fixture.service.listResults(created.value().id);
    assert(candidates.success() && candidates.value().size() == 2);
    assert(fixture.service.startSession(created.value().id).success());

    for (std::size_t index = 0; index < candidates.value().size(); ++index) {
        const auto& candidate = candidates.value()[index];
        assert(fixture.executionService.startExecution(candidate.id).success());
        FinishModelExecutionRequest outcome;
        outcome.status = ModelExecutionStatus::Completed;
        outcome.outputPreview = "Comparable result";
        outcome.inputTokens = index == 0 ? 100 : 80;
        outcome.outputTokens = index == 0 ? 50 : 60;
        outcome.costMicros = index == 0 ? 1 : 2;
        outcome.latencyMs = index == 0 ? 1000 : 3000;
        assert(fixture.executionService.finishExecution(candidate.id, outcome).success());

        CreateDeterministicQualityEvaluationRequest evaluation;
        evaluation.executionId = candidate.id;
        evaluation.evaluatorKind = QualityEvaluatorKind::Automated;
        evaluation.rubricVersion = "comparison-v1";
        evaluation.rubric = {{"correctness", 10'000}};
        evaluation.criteria = {{"correctness", index == 0 ? 9'000U : 8'000U,
            "Verified against the expected result"}};
        assert(fixture.executionService.addDeterministicEvaluation(evaluation).success());
    }

    auto projection = fixture.readModel.session(created.value().id);
    assert(projection.success());
    assert(projection.value().summary.inputTokens == 180);
    assert(projection.value().summary.outputTokens == 110);
    assert(projection.value().summary.costMicros == 3);
    assert(projection.value().summary.averageLatencyMs == 2000);
    assert(projection.value().summary.qualityComplete);
    assert(projection.value().summary.recommendedExecutionId == candidates.value()[0].id);
    assert(projection.value().summary.recommendationSource == "quality");

    assert(fixture.service.setPreferredExecution(
        created.value().id, candidates.value()[1].id).success());
    projection = fixture.readModel.session(created.value().id);
    assert(projection.success());
    assert(projection.value().summary.recommendedExecutionId == candidates.value()[1].id);
    assert(projection.value().summary.recommendationSource == "preferred");
}

ComparisonSession waitForTerminal(
    ComparisonService& service,
    const ComparisonSessionId& id)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto found = service.findSession(id);
        assert(found.success() && found.value().has_value());
        if (isTerminal(found.value()->status)) return *found.value();
        std::this_thread::sleep_for(5ms);
    }
    assert(false && "Comparison did not reach a terminal state");
    return {};
}

void testExecutionManagerRunsCandidatesInParallelAndPersistsQuality()
{
    ComparisonFixture fixture;
    const auto created = fixture.service.createSession(validRequest(fixture));
    assert(created.success());

    auto probe = std::make_shared<ConcurrencyProbe>();
    ComparisonFakeClientFactory clients;
    clients.add(fixture.providerA,
        std::make_shared<ComparisonFakeClient>(probe, "Candidate A"));
    clients.add(fixture.providerB,
        std::make_shared<ComparisonFakeClient>(probe, "Candidate B"));

    ComparisonExecutionManager manager(
        fixture.service, fixture.executionService, fixture.catalog,
        fixture.projects, clients, 2);
    assert(manager.enqueue(created.value().id).success());

    const auto session = waitForTerminal(fixture.service, created.value().id);
    assert(session.status == ComparisonSessionStatus::Completed);
    assert(probe->maximum.load() >= 2);

    const auto candidates = fixture.service.listResults(created.value().id);
    assert(candidates.success() && candidates.value().size() == 2);
    for (const auto& candidate : candidates.value()) {
        assert(candidate.status == ModelExecutionStatus::Completed);
        assert(candidate.outputContent.size() > 1'000);
        assert(candidate.outputPreview.size() == 1'000);
        assert(candidate.inputTokens == 100 && candidate.outputTokens == 50);
        const auto evaluations = fixture.executionService.listEvaluations(candidate.id);
        assert(evaluations.success() && evaluations.value().size() == 1);
        assert(evaluations.value().front().rubricVersion
            == ComparisonQualityEvaluator::rubricVersion());
    }

    const auto projection = fixture.readModel.session(created.value().id);
    assert(projection.success() && projection.value().summary.qualityComplete);
    manager.shutdown();
}

void testExecutionManagerCancelsActiveCandidates()
{
    ComparisonFixture fixture;
    const auto created = fixture.service.createSession(validRequest(fixture));
    assert(created.success());

    auto probe = std::make_shared<ConcurrencyProbe>();
    ComparisonFakeClientFactory clients;
    clients.add(fixture.providerA,
        std::make_shared<ComparisonFakeClient>(probe, "Candidate A", true));
    clients.add(fixture.providerB,
        std::make_shared<ComparisonFakeClient>(probe, "Candidate B", true));

    ComparisonExecutionManager manager(
        fixture.service, fixture.executionService, fixture.catalog,
        fixture.projects, clients, 2);
    assert(manager.enqueue(created.value().id).success());
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (probe->active.load() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(2ms);
    assert(probe->active.load() > 0);
    assert(manager.requestCancellation(created.value().id).success());

    const auto session = waitForTerminal(fixture.service, created.value().id);
    assert(session.status == ComparisonSessionStatus::Cancelled);
    const auto candidates = fixture.service.listResults(created.value().id);
    assert(candidates.success());
    for (const auto& candidate : candidates.value())
        assert(candidate.status == ModelExecutionStatus::Cancelled);
    manager.shutdown();
}

// Answers every candidate with a fixed usage chosen per API model id, so a test decides exactly
// which Response is short and which is long.
class UsageByModelClient final : public IProviderClient {
public:
    void set(const std::string& apiModelId, const ai::models::TokenUsage& usage)
    {
        std::lock_guard lock(mutex_);
        usage_[apiModelId] = usage;
    }

    common::Result<ProviderChatResponse> send(
        const ProviderChatRequest& request,
        std::stop_token) override
    {
        ProviderChatResponse response;
        response.content = "Priced candidate " + request.apiModelId;
        {
            std::lock_guard lock(mutex_);
            response.usage = usage_.at(request.apiModelId);
        }
        return common::Result<ProviderChatResponse>::ok(std::move(response));
    }

private:
    std::mutex mutex_;
    std::map<std::string, ai::models::TokenUsage> usage_;
};

ai::models::TokenUsage usageOf(
    std::uint64_t input, std::uint64_t output, std::uint64_t cacheWrite, std::uint64_t cacheRead)
{
    ai::models::TokenUsage usage;
    usage.inputTokens = input;
    usage.outputTokens = output;
    usage.cacheCreationInputTokens = cacheWrite;
    usage.cacheReadInputTokens = cacheRead;
    return usage;
}

// M2b: a comparison candidate is ONE provider response, so its cost is classified against the
// model's long-context threshold by that response's own input_tokens.
void testCandidateCostIsClassifiedPerResponseByTheModelsLongContextTier()
{
    ComparisonFixture fixture;
    // Model A: a complete tier ($2/$0.2/$2.5/$10 flat, $4/$0.4/$5/$15 above 272,000 input tokens).
    // Model B: the same tier, but its long cache-write price was never entered.
    const auto tierA = fixture.catalog.createModel(
        {fixture.providerA, "tier-a", "Tier A", 1'050'000, 4096, 2'000'000, 10'000'000, 2'500'000,
         200'000, true, LongContextPricing {272'000, 4'000'000, 15'000'000, 5'000'000, 400'000}});
    const auto tierB = fixture.catalog.createModel(
        {fixture.providerB, "tier-b", "Tier B", 1'050'000, 4096, 2'000'000, 10'000'000, 2'500'000,
         200'000, true, LongContextPricing {272'000, 4'000'000, 15'000'000, 0, 400'000}});
    assert(tierA.success() && tierB.success());

    auto client = std::make_shared<UsageByModelClient>();
    ComparisonFakeClientFactory clients;
    clients.add(fixture.providerA, client);
    clients.add(fixture.providerB, client);
    ComparisonExecutionManager manager(
        fixture.service, fixture.executionService, fixture.catalog,
        fixture.projects, clients, 2);

    // Runs one comparison and returns its two candidate rows (position 0 = model A, 1 = model B).
    int sessionCounter = 0;
    const auto runSession = [&](const ai::models::TokenUsage& usageA,
                                const ai::models::TokenUsage& usageB) {
        client->set("tier-a", usageA);
        client->set("tier-b", usageB);
        const auto created = fixture.service.createSession(
            {fixture.projectId, "Priced comparison",
             "Price each candidate, run " + std::to_string(++sessionCounter),
             "context-snapshot-hash",
             {{fixture.providerA, tierA.value(), "candidate"},
              {fixture.providerB, tierB.value(), "candidate"}}});
        assert(created.success());
        assert(manager.enqueue(created.value().id).success());
        const auto session = waitForTerminal(fixture.service, created.value().id);
        assert(session.status == ComparisonSessionStatus::Completed);
        const auto results = fixture.service.listResults(created.value().id);
        assert(results.success() && results.value().size() == 2);
        assert(results.value()[0].modelId == tierA.value());
        assert(results.value()[1].modelId == tierB.value());
        return results.value();
    };

    // A long Response with cache usage (A, 300,000 input of which 50,000 written and 200,000 read) is
    // billed entirely at the long tariff: 50,000*4 + 200,000*0.4 + 50,000*5 + 1,000*15 = 545,000
    // (flat prices would give 275,000). B sits exactly ON the threshold: 272,000 is still short.
    const auto first = runSession(usageOf(300'000, 1'000, 50'000, 200'000), usageOf(272'000, 0, 0, 0));
    assert(first[0].inputTokens == 300'000 && first[0].outputTokens == 1'000);
    assert(first[0].costMicros == 545'000 && first[0].costPriceKnown);
    assert(first[1].costMicros == 544'000 && first[1].costPriceKnown);

    // One token above the threshold flips the WHOLE Response to the long tariff: 272,001 * 4.
    // B is long with cache-write usage but no long cache-write price: a lower bound, never a
    // flat price for those tokens - 250,000*4 + 1,000*15 = 1,015,000 and the price is unknown.
    const auto second = runSession(usageOf(272'001, 0, 0, 0), usageOf(300'000, 1'000, 50'000, 0));
    assert(second[0].costMicros == 1'088'004 && second[0].costPriceKnown);
    assert(second[1].costMicros == 1'015'000 && !second[1].costPriceKnown);

    manager.shutdown();
}

} // namespace

int runComparisonServiceTests()
{
    testCreateSessionSeedsSharedExecutionLedger();
    testSelectionRequiresReviewableSessionResults();
    testInvalidComparisonRosterIsRejected();
    testReadModelOwnsTotalsAndDeterministicRecommendation();
    testExecutionManagerRunsCandidatesInParallelAndPersistsQuality();
    testExecutionManagerCancelsActiveCandidates();
    testCandidateCostIsClassifiedPerResponseByTheModelsLongContextTier();
    return 0;
}
