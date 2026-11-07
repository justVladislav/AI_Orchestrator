#pragma once

// A1-min test support: doubles that let a test run the REAL ExecutionAnalyticsService, ProjectMemoryCompactionService and
// ProjectCostService over scripted sources, count every read and every write, and fail any one required read on demand.
// Header-only on purpose: it is shared by the composition tests and (as a pattern) by nothing else.

#include "ai/application/analytics/ExecutionAnalyticsService.hpp"
#include "ai/application/analytics/ProjectCostService.hpp"
#include "CompactionIntentTestSupport.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionService.hpp"
#include "ai/application/inmemory/InMemoryRepositories.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace project_cost_test {

using namespace ai::application;
namespace common = ai::common;

inline common::Error scripted(const char* code) { return {code, std::string("scripted: ") + code}; }

// The compaction history of any number of projects, with a read counter, a write counter (a read model must never write) and an
// optional scripted read failure.
class RecordSource final : public IProjectMemoryCompactionRepository {
public:
    std::vector<ProjectMemoryCompactionRecord> records;
    std::optional<common::Error> listFailure;
    mutable int listCalls {0};
    int writes {0};

    common::Result<void> insertCompactionRecord(const ProjectMemoryCompactionRecord&) override
    {
        ++writes;
        return common::Result<void>::failure(scripted("test.unexpected_write"));
    }
    common::Result<void> commitSuccessfulCompaction(const ProjectMemoryCompactionRecord&, ProjectMemorySegment) override
    {
        ++writes;
        return common::Result<void>::failure(scripted("test.unexpected_write"));
    }
    [[nodiscard]] common::Result<std::vector<ProjectMemoryCompactionRecord>> listCompactionRecords(
        const ProjectId& projectId) const override
    {
        ++listCalls;
        if (listFailure.has_value()) {
            return common::Result<std::vector<ProjectMemoryCompactionRecord>>::failure(*listFailure);
        }
        std::vector<ProjectMemoryCompactionRecord> selected;
        for (const auto& record : records) {
            if (record.projectId == projectId) selected.push_back(record);
        }
        return common::Result<std::vector<ProjectMemoryCompactionRecord>>::ok(std::move(selected));
    }
};

class NoMemory final : public IProjectMemoryRepository {
public:
    common::Result<ProjectMemorySegment> insertMemorySegment(ProjectMemorySegment value) override
    {
        return common::Result<ProjectMemorySegment>::ok(std::move(value));
    }
    [[nodiscard]] common::Result<std::vector<ProjectMemorySegment>> listMemorySegments(const ProjectId&) const override
    {
        return common::Result<std::vector<ProjectMemorySegment>>::ok({});
    }
    [[nodiscard]] common::Result<std::optional<ProjectMemorySegment>> findMemorySegment(
        const ProjectMemorySegmentId&) const override
    {
        return common::Result<std::optional<ProjectMemorySegment>>::ok(std::nullopt);
    }
    common::Result<void> removeMemorySegments(const ProjectId&) override { return common::Result<void>::ok(); }
};

// The compaction service is only ever asked to LIST in these tests; a provider client must never be requested.
class NoClients final : public IProviderClientFactory {
public:
    mutable int requests {0};
    common::Result<std::shared_ptr<IProviderClient>> clientFor(const ProviderId&, const std::string&) override
    {
        ++requests;
        return common::Result<std::shared_ptr<IProviderClient>>::failure(scripted("test.no_client"));
    }
};

// The in-memory ledger with counters and a scripted failure of the project read.
class LedgerSource final : public IModelExecutionRepository {
public:
    InMemoryModelExecutionRepository inner;
    std::optional<common::Error> listFailure;
    mutable int projectReads {0};
    int writes {0};

    common::Result<ModelExecution> insertExecution(ModelExecution value) override
    {
        return inner.insertExecution(std::move(value));
    }
    common::Result<ModelExecution> updateExecution(ModelExecution value) override
    {
        ++writes;
        return inner.updateExecution(std::move(value));
    }
    [[nodiscard]] common::Result<std::optional<ModelExecution>> findExecution(const ModelExecutionId& id) const override
    {
        return inner.findExecution(id);
    }
    [[nodiscard]] common::Result<std::vector<ModelExecution>> listExecutions(
        ModelExecutionSource source, const std::string& sourceId) const override
    {
        return inner.listExecutions(source, sourceId);
    }
    [[nodiscard]] common::Result<std::vector<ModelExecution>> listProjectExecutions(const ProjectId& projectId) const override
    {
        ++projectReads;
        if (listFailure.has_value()) return common::Result<std::vector<ModelExecution>>::failure(*listFailure);
        return inner.listProjectExecutions(projectId);
    }
    [[nodiscard]] common::Result<std::vector<ModelExecution>> listUnfinishedExecutions() const override
    {
        return inner.listUnfinishedExecutions();
    }
    common::Result<QualityEvaluation> insertEvaluation(QualityEvaluation value) override
    {
        ++writes;
        return inner.insertEvaluation(std::move(value));
    }
    [[nodiscard]] common::Result<std::optional<QualityEvaluation>> findEvaluation(const QualityEvaluationId& id) const override
    {
        return inner.findEvaluation(id);
    }
    [[nodiscard]] common::Result<std::vector<QualityEvaluation>> listEvaluations(const ModelExecutionId& id) const override
    {
        return inner.listEvaluations(id);
    }
};

// The in-memory model catalog with a counter and a scripted failure of listAll().
class CatalogSource final : public IModelRepository {
public:
    InMemoryModelRepository inner;
    std::optional<common::Error> listFailure;
    mutable int listCalls {0};

    common::Result<Model> insert(Model model) override { return inner.insert(std::move(model)); }
    common::Result<Model> update(Model model) override { return inner.update(std::move(model)); }
    [[nodiscard]] common::Result<std::optional<Model>> findById(const ModelId& id) const override { return inner.findById(id); }
    [[nodiscard]] common::Result<std::vector<Model>> listAll(const std::optional<ProviderId>& providerId) const override
    {
        ++listCalls;
        if (listFailure.has_value()) return common::Result<std::vector<Model>>::failure(*listFailure);
        return inner.listAll(providerId);
    }
    common::Result<void> setDefaultModel(const ProviderId& providerId, const ModelId& modelId) override
    {
        return inner.setDefaultModel(providerId, modelId);
    }
    common::Result<void> remove(const ModelId& id) override { return inner.remove(id); }
};

// All the real services over the scripted sources.
struct Fixture {
    LedgerSource ledger;
    CatalogSource catalog;
    RecordSource compactions;
    NoMemory memory;
    NoClients clients;
    InMemoryProjectRepository projects;
    InMemoryProviderRepository providers;
    ExecutionAnalyticsService analytics {ledger, catalog};
    test_support::ScriptedCompactionIntentRepository intents;
    ProjectMemoryCompactionService compactionService {memory, compactions, intents, projects, providers, catalog, clients};
    CompactionIntentReadModelService intentReadModel {intents};
    ProjectCostService service {analytics, compactionService, intentReadModel};
};

inline std::chrono::system_clock::time_point at(int seconds)
{
    return std::chrono::system_clock::time_point {} + std::chrono::seconds {1'000'000 + seconds};
}

inline ProjectMemoryCompactionRecord compactionRecord(const char* id, const char* project, const char* provider,
    const char* model, std::optional<bool> priceKnown, std::uint64_t cost, std::uint64_t input, std::uint64_t output,
    ProjectMemoryCompactionStatus status, int seconds = 0)
{
    ProjectMemoryCompactionRecord record;
    record.id = id;
    record.projectId = project;
    record.providerId = provider;
    record.modelId = model;
    record.costPriceKnown = priceKnown;
    record.costMicros = cost;
    record.inputTokens = input;
    record.outputTokens = output;
    record.status = status;
    record.createdAt = at(seconds);
    return record;
}

inline ModelExecution ledgerRow(const char* id, const char* project, const char* provider, const char* model,
    std::uint64_t input, std::uint64_t output, std::uint64_t cost, bool priceKnown, int seconds = 0,
    std::uint32_t position = 0)
{
    ModelExecution execution;
    execution.id = id;
    execution.source = ModelExecutionSource::Chat;
    execution.sourceId = std::string("chat-") + id;
    execution.projectId = project;
    execution.providerId = provider;
    execution.modelId = model;
    execution.role = "assistant";
    execution.position = position;
    execution.status = ModelExecutionStatus::Completed;
    execution.inputTokens = input;
    execution.outputTokens = output;
    execution.costMicros = cost;
    execution.costPriceKnown = priceKnown;
    execution.latencyMs = 100;
    execution.createdAt = at(seconds);
    return execution;
}

} // namespace project_cost_test
