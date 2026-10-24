#pragma once

#include "ai/application/IAgentToolAuditRepository.hpp"
#include "ai/application/IAgentToolSessionRepository.hpp"
#include "ai/application/IModelExecutionRepository.hpp"
#include "ai/common/Result.hpp"
#include "ai/infrastructure/sqlite/SqliteDatabase.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ai::infrastructure {

class SqliteModelExecutionRepository final : public application::IModelExecutionRepository {
public:
    explicit SqliteModelExecutionRepository(std::shared_ptr<SqliteDatabase> database);
    common::Result<application::ModelExecution> insertExecution(application::ModelExecution value) override;
    common::Result<application::ModelExecution> updateExecution(application::ModelExecution value) override;
    [[nodiscard]] common::Result<std::optional<application::ModelExecution>> findExecution(const application::ModelExecutionId& id) const override;
    [[nodiscard]] common::Result<std::vector<application::ModelExecution>> listExecutions(application::ModelExecutionSource source, const std::string& sourceId) const override;
    [[nodiscard]] common::Result<std::vector<application::ModelExecution>> listProjectExecutions(const application::ProjectId& projectId) const override;
    [[nodiscard]] common::Result<std::vector<application::ModelExecution>> listUnfinishedExecutions() const override;
    common::Result<application::QualityEvaluation> insertEvaluation(application::QualityEvaluation value) override;
    [[nodiscard]] common::Result<std::optional<application::QualityEvaluation>> findEvaluation(const application::QualityEvaluationId& id) const override;
    [[nodiscard]] common::Result<std::vector<application::QualityEvaluation>> listEvaluations(const application::ModelExecutionId& executionId) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of agent tool session and provider turn persistence. */
class SqliteAgentToolSessionRepository final : public application::IAgentToolSessionRepository {
public:
    explicit SqliteAgentToolSessionRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<application::AgentToolSession> insertSession(
        application::AgentToolSession session) override;
    common::Result<application::AgentToolSession> updateSession(
        application::AgentToolSession session) override;
    [[nodiscard]] common::Result<std::optional<application::AgentToolSession>> findSession(
        const std::string& sessionId) const override;
    [[nodiscard]] common::Result<std::vector<application::AgentToolSession>> listForRun(
        const std::string& runId) const override;
    common::Result<application::AgentProviderTurn> insertTurn(
        application::AgentProviderTurn turn) override;
    [[nodiscard]] common::Result<std::vector<application::AgentProviderTurn>> listTurns(
        const std::string& sessionId) const override;
    [[nodiscard]] common::Result<std::size_t> interruptActiveSessions() override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of tool-call audit records (agent_tool_calls table). */
class SqliteAgentToolAuditRepository final : public application::IAgentToolAuditRepository {
public:
    explicit SqliteAgentToolAuditRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<void> insert(
        const application::AgentToolAuditRecord& record) override;
    common::Result<void> update(
        const application::AgentToolAuditRecord& record) override;
    [[nodiscard]] common::Result<std::optional<application::AgentToolAuditRecord>> find(
        const std::string& auditId) const override;
    [[nodiscard]] common::Result<std::vector<application::AgentToolAuditRecord>> listForSession(
        const std::string& sessionId) const override;
    [[nodiscard]] common::Result<std::size_t> interruptRunning(
        const std::string& sessionId) override;
    [[nodiscard]] common::Result<std::size_t> interruptAllRunning() override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

} // namespace ai::infrastructure
