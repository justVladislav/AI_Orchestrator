#pragma once

#include "ai/application/IVerificationBootstrapRepository.hpp"

#include <map>
#include <mutex>
#include <vector>

namespace ai::application {

class InMemoryVerificationBootstrapRepository final
    : public IVerificationBootstrapRepository {
public:
    common::Result<void> recordDecision(VerificationBootstrapDecision decision) override;
    [[nodiscard]] common::Result<std::optional<VerificationBootstrapDecision>>
    findDecisionForRun(const RunId& runId) const override;
    common::Result<void> appendTransition(VerificationBootstrapTransition transition) override;
    [[nodiscard]] common::Result<std::vector<VerificationBootstrapTransition>>
    listTransitionsForRun(const RunId& runId) const override;
    common::Result<void> recordRetryLink(VerificationBootstrapRetryLink link) override;
    [[nodiscard]] common::Result<std::optional<VerificationBootstrapRetryLink>>
    findRetryLinkForPredecessor(const RunId& predecessorRunId) const override;

private:
    mutable std::mutex mutex_;
    std::map<RunId, VerificationBootstrapDecision> decisions_;
    std::map<RunId, std::vector<VerificationBootstrapTransition>> transitions_;
    std::map<RunId, VerificationBootstrapRetryLink> retryLinksByPredecessor_;
};

} // namespace ai::application
