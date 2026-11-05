#include "ai/application/inmemory/InMemoryRepositories.hpp"
#include "ai/backend/ProviderClientTranslation.hpp"
#include "ai/backend/RealProviderClientFactory.hpp"

#include <iostream>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>

namespace {

using namespace ai::application;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

class FixedCredentialStore final : public ICredentialStore {
public:
    ai::common::Result<void> store(const std::string&, const std::string&) override
    {
        return ai::common::Result<void>::ok();
    }

    ai::common::Result<std::optional<std::string>> load(const std::string&) const override
    {
        return ai::common::Result<std::optional<std::string>>::ok(std::string {"unused-test-key"});
    }

    ai::common::Result<void> remove(const std::string&) override
    {
        return ai::common::Result<void>::ok();
    }
};

Provider provider(std::string id, std::string kind)
{
    Provider value;
    value.id = std::move(id);
    value.name = value.id;
    value.kind = std::move(kind);
    value.credentialReference = value.id + "-credential";
    return value;
}

ProviderChatRequest request(ai::models::PromptCacheMode mode)
{
    ProviderChatRequest value;
    value.apiModelId = "test-model";
    value.systemPrompt = "stable prefix";
    value.userContent = "moving request";
    value.maxOutputTokens = 16;
    value.promptCache.mode = mode;
    return value;
}

std::string codeOf(const ai::common::Result<ProviderChatResponse>& result)
{
    return result.success() ? std::string {} : result.error()->code;
}

void testProductionAdaptersValidateThePolicyBeforeAnyNetworkCall()
{
    InMemoryProviderRepository providers;
    require(providers.insert(provider("openai-provider", "openai")).success(),
        "factory: OpenAI provider seed failed");
    require(providers.insert(provider("anthropic-provider", "anthropic")).success(),
        "factory: Anthropic provider seed failed");
    FixedCredentialStore credentials;
    ai::backend::RealProviderClientFactory factory(credentials, providers);

    const auto openai = factory.clientFor("openai-provider", "openai");
    const auto anthropic = factory.clientFor("anthropic-provider", "anthropic");
    require(openai.success() && anthropic.success(),
        "factory: production clients were not created");

    std::stop_source stopped;
    stopped.request_stop();
    const auto oneHour = request(ai::models::PromptCacheMode::EphemeralOneHour);
    require(codeOf(openai.value()->send(oneHour, stopped.get_token()))
            == "provider.prompt_cache_unsupported",
        "factory: OpenAI adapter did not reject caching before the provider call");

    const auto unknown = request(static_cast<ai::models::PromptCacheMode>(99));
    require(codeOf(anthropic.value()->send(unknown, stopped.get_token()))
            == "provider.invalid_prompt_cache_policy",
        "factory: Anthropic adapter did not reject an unknown policy before the provider call");

    require(codeOf(anthropic.value()->send(oneHour, stopped.get_token()))
            == "provider.cancelled",
        "factory: Anthropic adapter did not admit the supported one-hour policy");
    require(codeOf(openai.value()->send(
                request(ai::models::PromptCacheMode::Disabled), stopped.get_token()))
            == "provider.cancelled",
        "factory: disabled compatibility path did not reach the normal provider-call boundary");
}

// C2-B/S2a: the factory builds BOTH real provider kinds as the ONE common client whose observed/legacy behaviour is proven in
// ProviderClientTranslation_tests (with a fake Generate), and its virtual observed call is reachable through the real factory.
// Only requests that never reach a provider are sent here (a typed refusal or a stop before the request): no network, no dummy
// key against a real URL.
void testTheFactoryBuildsTheCommonObservedClientForBothKinds()
{
    using ai::backend::provider_client_detail::GenerateProviderClient;
    InMemoryProviderRepository providers;
    require(providers.insert(provider("openai-provider", "openai")).success(), "wiring: OpenAI provider seed failed");
    require(providers.insert(provider("anthropic-provider", "anthropic")).success(), "wiring: Anthropic provider seed failed");
    FixedCredentialStore credentials;
    ai::backend::RealProviderClientFactory factory(credentials, providers);
    const auto openai = factory.clientFor("openai-provider", "openai");
    const auto anthropic = factory.clientFor("anthropic-provider", "anthropic");
    require(openai.success() && anthropic.success(), "wiring: production clients were not created");
    require(dynamic_cast<GenerateProviderClient*>(openai.value().get()) != nullptr,
        "wiring: the OpenAI client is the common observed client");
    require(dynamic_cast<GenerateProviderClient*>(anthropic.value().get()) != nullptr,
        "wiring: the Anthropic client is the common observed client");
    require(openai.value().get() != anthropic.value().get(), "wiring: two kinds, two clients");

    std::stop_source stopped;
    stopped.request_stop();
    for (const auto& client : {openai.value(), anthropic.value()}) {
        const auto observed = client->sendObserved(request(ai::models::PromptCacheMode::Disabled), stopped.get_token());
        require(!observed.result.success() && observed.result.error()->code == "provider.cancelled"
                && !observed.discardedResponse.has_value(),
            "wiring: a stop before the request is a payload-free cancellation through the real observed call");
    }
    const auto refused = openai.value()->sendObserved(
        request(ai::models::PromptCacheMode::EphemeralOneHour), stopped.get_token());
    require(!refused.result.success() && refused.result.error()->code == "provider.prompt_cache_unsupported"
            && !refused.discardedResponse.has_value(),
        "wiring: the policy refusal outranks the stop through the real observed call");
}

} // namespace

int main()
{
    try {
        testProductionAdaptersValidateThePolicyBeforeAnyNetworkCall();
        testTheFactoryBuildsTheCommonObservedClientForBothKinds();
        std::cout << "RealProviderClientFactoryTests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RealProviderClientFactoryTests: " << error.what() << '\n';
        return 1;
    }
}
