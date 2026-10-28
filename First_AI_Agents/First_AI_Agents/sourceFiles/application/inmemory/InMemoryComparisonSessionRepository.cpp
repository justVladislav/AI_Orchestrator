#include "ai/application/inmemory/InMemoryComparisonSessionRepository.hpp"

#include <algorithm>
#include <utility>

namespace ai::application {
namespace {

common::Error repositoryError(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

void sortSessions(std::vector<ComparisonSession>& values)
{
    std::sort(values.begin(), values.end(), [](const auto& left, const auto& right) {
        if (left.createdAt != right.createdAt) return left.createdAt < right.createdAt;
        return left.id < right.id;
    });
}

} // namespace

common::Result<ComparisonSession> InMemoryComparisonSessionRepository::insertSession(
    ComparisonSession value)
{
    std::scoped_lock lock(mutex_);
    if (value.id.empty() || values_.contains(value.id)) {
        return common::Result<ComparisonSession>::failure(repositoryError(
            "comparison.persistence_conflict", "Comparison id is empty or already exists"));
    }
    const auto [it, inserted] = values_.emplace(value.id, std::move(value));
    static_cast<void>(inserted);
    return common::Result<ComparisonSession>::ok(it->second);
}

common::Result<ComparisonSession> InMemoryComparisonSessionRepository::updateSession(
    ComparisonSession value)
{
    std::scoped_lock lock(mutex_);
    const auto found = values_.find(value.id);
    if (found == values_.end()) {
        return common::Result<ComparisonSession>::failure(repositoryError(
            "comparison.not_found", "Comparison session not found"));
    }
    if (value.revision != found->second.revision + 1) {
        return common::Result<ComparisonSession>::failure(repositoryError(
            "comparison.revision_conflict", "Comparison session was changed concurrently"));
    }
    found->second = std::move(value);
    return common::Result<ComparisonSession>::ok(found->second);
}

common::Result<std::optional<ComparisonSession>>
InMemoryComparisonSessionRepository::findSession(const ComparisonSessionId& id) const
{
    std::scoped_lock lock(mutex_);
    const auto found = values_.find(id);
    return common::Result<std::optional<ComparisonSession>>::ok(
        found == values_.end() ? std::nullopt
                               : std::optional<ComparisonSession>{found->second});
}

common::Result<std::vector<ComparisonSession>>
InMemoryComparisonSessionRepository::listSessions(
    const std::optional<ProjectId>& projectId) const
{
    std::scoped_lock lock(mutex_);
    std::vector<ComparisonSession> values;
    for (const auto& [id, value] : values_) {
        static_cast<void>(id);
        if (!projectId.has_value() || value.projectId == *projectId)
            values.push_back(value);
    }
    sortSessions(values);
    return common::Result<std::vector<ComparisonSession>>::ok(std::move(values));
}

common::Result<std::vector<ComparisonSession>>
InMemoryComparisonSessionRepository::listUnfinishedSessions() const
{
    std::scoped_lock lock(mutex_);
    std::vector<ComparisonSession> values;
    for (const auto& [id, value] : values_) {
        static_cast<void>(id);
        if (!isTerminal(value.status)) values.push_back(value);
    }
    sortSessions(values);
    return common::Result<std::vector<ComparisonSession>>::ok(std::move(values));
}

} // namespace ai::application
