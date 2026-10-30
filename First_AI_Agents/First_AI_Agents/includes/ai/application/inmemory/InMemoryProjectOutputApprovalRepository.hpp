#pragma once

#include "ai/application/workspace/ProjectOutputApproval.hpp"

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

namespace ai::application {

/**
 * Thread-safe development/test adapter with the same semantics as the SQLite one: one lock covers read, repeat check, limit,
 * validator and insert, so concurrent commits never leave a partial or invalid set. (It has no projects table, so it cannot reject an
 * unknown project; the service does that before it calls a repository.)
 */
class InMemoryProjectOutputApprovalRepository final : public IProjectOutputApprovalRepository {
public:
    [[nodiscard]] common::Result<ProjectOutputApprovalCommitResult> commit(
        const ProjectOutputApproval& candidate, const ProjectOutputApprovalSetValidator& validator) override
    {
        using Result = common::Result<ProjectOutputApprovalCommitResult>;
        std::lock_guard lock(mutex_);
        std::vector<ProjectOutputApproval> current = pairLocked(candidate.projectId, candidate.workspaceKey);
        for (const auto& record : current) {
            if (record.pathKey == candidate.pathKey) {
                return Result::ok({ProjectOutputApprovalCommitOutcome::AlreadyApproved, record});
            }
        }
        if (current.size() >= kProjectOutputApprovalLimit) {
            return Result::failure({kOutputApprovalLimitReachedCode, "This folder already has the most approvals it can hold"});
        }
        if (validator) {
            const auto valid = validator(current);
            if (!valid.success()) return Result::failure(*valid.error());
        }
        // The id is a conflict only for a NEW insertion: an exact repeat of the path was answered above, as the SQLite order does.
        for (const auto& record : records_) {
            if (record.id == candidate.id) {
                return Result::failure({kOutputApprovalConflictCode, "An approval with this id already exists"});
            }
        }
        records_.push_back(candidate);
        return Result::ok({ProjectOutputApprovalCommitOutcome::Inserted, candidate});
    }

    [[nodiscard]] common::Result<std::vector<ProjectOutputApproval>> listForWorkspace(
        const ProjectId& projectId, const std::string& workspaceKey) const override
    {
        std::lock_guard lock(mutex_);
        return common::Result<std::vector<ProjectOutputApproval>>::ok(pairLocked(projectId, workspaceKey));
    }

    [[nodiscard]] common::Result<std::vector<ProjectOutputApproval>> listForProject(const ProjectId& projectId) const override
    {
        std::lock_guard lock(mutex_);
        std::vector<ProjectOutputApproval> result;
        for (const auto& record : records_) {
            if (record.projectId == projectId) result.push_back(record);
        }
        std::sort(result.begin(), result.end(), [](const ProjectOutputApproval& a, const ProjectOutputApproval& b) {
            return a.workspaceKey != b.workspaceKey ? a.workspaceKey < b.workspaceKey : a.pathKey < b.pathKey;
        });
        return common::Result<std::vector<ProjectOutputApproval>>::ok(std::move(result));
    }

    common::Result<bool> revoke(const ProjectId& projectId, const std::string& approvalId) override
    {
        std::lock_guard lock(mutex_);
        const auto found = std::find_if(records_.begin(), records_.end(), [&](const ProjectOutputApproval& record) {
            return record.projectId == projectId && record.id == approvalId;
        });
        if (found == records_.end()) return common::Result<bool>::ok(false);
        records_.erase(found);
        return common::Result<bool>::ok(true);
    }

private:
    [[nodiscard]] std::vector<ProjectOutputApproval> pairLocked(const ProjectId& projectId, const std::string& workspaceKey) const
    {
        std::vector<ProjectOutputApproval> result;
        for (const auto& record : records_) {
            if (record.projectId == projectId && record.workspaceKey == workspaceKey) result.push_back(record);
        }
        std::sort(result.begin(), result.end(),
            [](const ProjectOutputApproval& a, const ProjectOutputApproval& b) { return a.pathKey < b.pathKey; });
        return result;
    }

    mutable std::mutex mutex_;
    std::vector<ProjectOutputApproval> records_;
};

} // namespace ai::application
