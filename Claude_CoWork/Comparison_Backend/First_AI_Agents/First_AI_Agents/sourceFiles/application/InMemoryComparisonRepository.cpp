#include "ai/application/IComparisonRepository.hpp"

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace ai::application {

namespace {

class InMemoryComparisonRepository final : public IComparisonRepository {
public:
    common::Result<ComparisonSession> insertSession(ComparisonSession session) override
    {
        std::lock_guard lock(mutex_);
        sessions_.emplace(session.id, session);
        return common::Result<ComparisonSession>::ok(std::move(session));
    }

    common::Result<ComparisonSession> updateSession(ComparisonSession session) override
    {
        std::lock_guard lock(mutex_);
        auto it = sessions_.find(session.id);
        if (it == sessions_.end()) {
            return common::Result<ComparisonSession>::failure(
                {"comparison.not_found", "Session not found: " + session.id});
        }
        it->second = session;
        return common::Result<ComparisonSession>::ok(std::move(session));
    }

    [[nodiscard]] common::Result<std::optional<ComparisonSession>> findSessionById(
        const ComparisonId& id) const override
    {
        std::lock_guard lock(mutex_);
        const auto it = sessions_.find(id);
        if (it == sessions_.end()) {
            return common::Result<std::optional<ComparisonSession>>::ok(std::nullopt);
        }
        return common::Result<std::optional<ComparisonSession>>::ok(it->second);
    }

    [[nodiscard]] common::Result<std::vector<ComparisonSession>> listSessionsByProject(
        const ProjectId& projectId) const override
    {
        std::lock_guard lock(mutex_);
        std::vector<ComparisonSession> out;
        for (const auto& [id, session] : sessions_) {
            if (session.projectId == projectId) {
                out.push_back(session);
            }
        }
        std::sort(out.begin(), out.end(), [](const ComparisonSession& a, const ComparisonSession& b) {
            return a.createdAt > b.createdAt;
        });
        return common::Result<std::vector<ComparisonSession>>::ok(std::move(out));
    }

    common::Result<ComparisonResult> insertResult(ComparisonResult result) override
    {
        std::lock_guard lock(mutex_);
        results_.emplace(result.id, result);
        return common::Result<ComparisonResult>::ok(std::move(result));
    }

    common::Result<ComparisonResult> updateResult(ComparisonResult result) override
    {
        std::lock_guard lock(mutex_);
        auto it = results_.find(result.id);
        if (it == results_.end()) {
            return common::Result<ComparisonResult>::failure(
                {"comparison.not_found", "Result not found: " + result.id});
        }
        it->second = result;
        return common::Result<ComparisonResult>::ok(std::move(result));
    }

    [[nodiscard]] common::Result<std::optional<ComparisonResult>> findResultById(
        const ComparisonResultId& id) const override
    {
        std::lock_guard lock(mutex_);
        const auto it = results_.find(id);
        if (it == results_.end()) {
            return common::Result<std::optional<ComparisonResult>>::ok(std::nullopt);
        }
        return common::Result<std::optional<ComparisonResult>>::ok(it->second);
    }

    [[nodiscard]] common::Result<std::vector<ComparisonResult>> listResultsBySession(
        const ComparisonId& comparisonId) const override
    {
        std::lock_guard lock(mutex_);
        std::vector<ComparisonResult> out;
        for (const auto& [id, result] : results_) {
            if (result.comparisonId == comparisonId) {
                out.push_back(result);
            }
        }
        return common::Result<std::vector<ComparisonResult>>::ok(std::move(out));
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<ComparisonId, ComparisonSession> sessions_;
    std::unordered_map<ComparisonResultId, ComparisonResult> results_;
};

} // namespace

std::unique_ptr<IComparisonRepository> makeInMemoryComparisonRepository()
{
    return std::make_unique<InMemoryComparisonRepository>();
}

} // namespace ai::application
