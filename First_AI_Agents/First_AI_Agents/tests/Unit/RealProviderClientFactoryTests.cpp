#include "ai/application/inmemory/InMemoryRepositories.hpp"
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

} // namespace

int main()
{
    try {
        testProductionAdaptersValidateThePolicyBeforeAnyNetworkCall();
        std::cout << "RealProviderClientFactoryTests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RealProviderClientFactoryTests: " << error.what() << '\n';
        return 1;
    }
}
