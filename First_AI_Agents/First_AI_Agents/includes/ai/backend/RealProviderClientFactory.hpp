#pragma once

#include "ai/application/ICredentialStore.hpp"
#include "ai/application/IProviderClient.hpp"
#include "ai/application/IProviderRepository.hpp"

#include <memory>

namespace ai::backend {

/**
 * Real OpenAI/Anthropic-backed IProviderClientFactory. A thin adapter over
 * the existing, already-tested ai::providers::OpenAIProvider/
 * AnthropicProvider (CURL transport, in AIOrchestrator_Runtime) - this class
 * only translates between Application's provider-neutral
 * ProviderChatRequest/Response and ai::models::ModelRequest/ModelResponse,
 * and resolves the credential for a provider through ICredentialStore
 * (never returning the secret to any caller of clientFor() itself).
 *
 * This header/its .cpp are the one place in this Backend allowed to depend
 * on AIOrchestrator::Runtime (and therefore CURL): only compiled into
 * AIOrchestrator_BackendRuntime when AI_ORCHESTRATOR_BUILD_REAL_PROVIDERS is
 * enabled (see CMakeLists.txt), and that dependency is PRIVATE - it never
 * appears on BackendRuntime's public link interface or public headers.
 */
class RealProviderClientFactory final : public application::IProviderClientFactory {
public:
    RealProviderClientFactory(
        application::ICredentialStore& credentials,
        application::IProviderRepository& providers);

    common::Result<std::shared_ptr<application::IProviderClient>> clientFor(
        const application::ProviderId& providerId,
        const std::string& providerKind) override;

private:
    application::ICredentialStore& credentials_;
    application::IProviderRepository& providers_;
    // A fresh shared client is returned per resolution. Calls already in
    // flight retain their credential snapshot; the next resolution observes
    // a rotated credential without invalidating another thread's client.
};

} // namespace ai::backend
