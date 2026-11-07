#include "ai/application/analytics/ProjectCostComposition.hpp"

#include "ai/application/conversations/CompactionCreatedAtWindow.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace ai::application {
namespace {

std::uint64_t recordedItems(const ProjectCostAggregate& totals) noexcept
{
    return totals.ledgerExecutionCount() + totals.compactionRecordCount();
}

// The ledger part of a level is copied as the analytics service returned it; the compaction part starts empty.
ProjectCostAggregate fromLedger(const ExecutionAggregate& ledger)
{
    ProjectCostAggregate aggregate;
    aggregate.ledger = ledger;
    return aggregate;
}

bool inWindow(const ProjectMemoryCompactionRecord& record,
    const std::optional<std::chrono::system_clock::time_point>& fromInclusive,
    const std::optional<std::chrono::system_clock::time_point>& toExclusive)
{
    return inCreatedAtWindow(record.createdAt, fromInclusive, toExclusive);
}

} // namespace

void addCompactionRecord(CompactionCostAggregate& aggregate, const ProjectMemoryCompactionRecord& record)
{
    ++aggregate.recordCount;
    switch (record.status) {
    case ProjectMemoryCompactionStatus::Completed: ++aggregate.completedCount; break;
    case ProjectMemoryCompactionStatus::Failed: ++aggregate.failedCount; break;
    case ProjectMemoryCompactionStatus::Cancelled: ++aggregate.cancelledCount; break;
    }
    switch (compactionPriceState(record)) {
    case ExecutionPriceState::Known: ++aggregate.knownCount; break;
    case ExecutionPriceState::Partial: ++aggregate.partialCount; break;
    case ExecutionPriceState::Unknown: ++aggregate.unknownCount; break;
    case ExecutionPriceState::None: ++aggregate.noneCount; break;
    }
    // A record whose state is none holds no amount and no tokens, so adding the stored values unconditionally is the same as adding
    // only the others; there is deliberately no usage or status filter here.
    aggregate.inputTokens += record.inputTokens;
    aggregate.outputTokens += record.outputTokens;
    aggregate.costMicros += record.costMicros;
}

common::Result<ProjectCostSnapshot> composeProjectCost(
    const ExecutionAnalyticsSnapshot& ledger,
    const std::vector<ProjectMemoryCompactionRecord>& records,
    std::optional<std::chrono::system_clock::time_point> fromInclusive,
    std::optional<std::chrono::system_clock::time_point> toExclusive)
{
    // The whole read is checked, not only the window: a repeated primary key is a corrupt history whatever the window is.
    std::unordered_set<ProjectMemoryCompactionId> seen;
    seen.reserve(records.size());
    for (const auto& record : records) {
        if (!seen.insert(record.id).second) {
            return common::Result<ProjectCostSnapshot>::failure(
                {kProjectCostDuplicateCompactionRecordCode, kProjectCostDuplicateCompactionRecordMessage});
        }
    }

    ProjectCostSnapshot snapshot;
    snapshot.totals = fromLedger(ledger.totals);
    std::unordered_map<ProviderId, std::size_t> providerIndexes;
    std::vector<std::unordered_map<ModelId, std::size_t>> modelIndexes;

    for (const auto& provider : ledger.providers) {
        providerIndexes.emplace(provider.providerId, snapshot.providers.size());
        ProviderProjectCost row;
        row.providerId = provider.providerId;
        row.totals = fromLedger(provider.totals);
        std::unordered_map<ModelId, std::size_t> models;
        for (const auto& model : provider.models) {
            models.emplace(model.modelId, row.models.size());
            row.models.push_back(ModelProjectCost {model.modelId, fromLedger(model.totals)});
        }
        modelIndexes.push_back(std::move(models));
        snapshot.providers.push_back(std::move(row));
    }

    for (const auto& record : records) {
        if (!inWindow(record, fromInclusive, toExclusive)) continue;
        addCompactionRecord(snapshot.totals.compaction, record);
        const auto [providerIt, providerInserted] = providerIndexes.emplace(record.providerId, snapshot.providers.size());
        if (providerInserted) {
            ProviderProjectCost row;
            row.providerId = record.providerId;
            snapshot.providers.push_back(std::move(row));
            modelIndexes.emplace_back();
        }
        auto& provider = snapshot.providers[providerIt->second];
        addCompactionRecord(provider.totals.compaction, record);
        const auto [modelIt, modelInserted] = modelIndexes[providerIt->second].emplace(record.modelId, provider.models.size());
        if (modelInserted) provider.models.push_back(ModelProjectCost {record.modelId, {}});
        addCompactionRecord(provider.models[modelIt->second].totals.compaction, record);
    }

    const auto before = [](const auto& left, const auto& right, const auto& leftId, const auto& rightId) {
        if (left.costMicros() != right.costMicros()) return left.costMicros() > right.costMicros();
        if (recordedItems(left) != recordedItems(right)) return recordedItems(left) > recordedItems(right);
        return leftId < rightId;
    };
    for (auto& provider : snapshot.providers) {
        std::sort(provider.models.begin(), provider.models.end(), [&](const auto& left, const auto& right) {
            return before(left.totals, right.totals, left.modelId, right.modelId);
        });
    }
    std::sort(snapshot.providers.begin(), snapshot.providers.end(), [&](const auto& left, const auto& right) {
        return before(left.totals, right.totals, left.providerId, right.providerId);
    });
    return common::Result<ProjectCostSnapshot>::ok(std::move(snapshot));
}

} // namespace ai::application
