#pragma once

#include "ai/providers/IProviderAgentSession.hpp"
#include "ai/application/ICredentialStore.hpp"
#include "ai/application/IProviderRepository.hpp"

namespace ai::providers {

// Real IProviderAgentSessionFactory backed by OpenAIAgentSession and
// AnthropicAgentSession. Mirrors RealProviderClientFactory's credential
// resolution pattern: the API key is loaded here and passed into the
// session constructor; callers never see it.
// Only compiled into AIOrchestrator_BackendRuntime when
// AI_ORCHESTRATOR_BUILD_REAL_AGENT_SESSIONS is ON (see CMakeLists.txt).
class RealProviderAgentSessionFactory final : public IProviderAgentSessionFactory {
public:
    RealProviderAgentSessionFactory(
        application::ICredentialStore& credentials,
        application::IProviderRepository& providers);

    common::Result<std::unique_ptr<IProviderAgentSession>> createSession(
        const application::ProviderId& providerId,
        const std::string& providerKind) override;

private:
    application::ICredentialStore& credentials_;
    application::IProviderRepository& providers_;
};

} // namespace ai::providers
