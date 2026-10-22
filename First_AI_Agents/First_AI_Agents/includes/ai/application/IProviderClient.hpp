#pragma once

#include "ai/application/ProviderCatalogTypes.hpp"
#include "ai/common/Result.hpp"
#include "ai/models/PromptCachePolicy.hpp"
#include "ai/models/TokenUsage.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>

namespace ai::application {

/**
 * Provider-neutral chat request/response shapes used by the production
 * execution-profile runner. These are deliberately not ai::models::
 * ModelRequest/ModelResponse: those types (and the concrete OpenAI/
 * Anthropic providers that consume them) live in the CURL-dependent legacy
 * Runtime target, which Application/Core must never depend on. A real
 * IProviderClient implementation (infrastructure detail, wired only by
 * BackendRuntime when real providers are compiled in) is what translates
 * between the two.
 */
struct ProviderChatRequest {
    ProviderId providerId;
    ModelId modelId;
    // The provider's own model identifier (Model::apiModelId), passed
    // explicitly so a client implementation never needs its own catalog
    // lookup just to know what to ask the remote API for.
    std::string apiModelId;
    // Optional system-level instructions, kept separate from userContent so
    // a client can map it onto whichever mechanism its provider uses
    // (OpenAI/Anthropic both support a distinct system role/field).
    std::string systemPrompt;
    // Fully assembled user-turn content: project description, run task, and
    // any relevant prior-stage output the caller chose to include.
    std::string userContent;
    std::chrono::milliseconds timeout {30000};
    std::uint32_t maxOutputTokens {4096};
    double temperature {0.2};
    // Long-running chat/Run calls may wait for the provider response until
    // explicitly cancelled. The independent connection timeout still applies.
    bool disableTotalTimeout {false};
    // Disabled by default. When enabled, the concrete provider may cache only
    // the stable system prompt; callers quote a worst-case cache MISS first.
    models::PromptCachePolicy promptCache;
};

struct ProviderChatResponse {
    std::string content;
    models::TokenUsage usage;
};

/** One resolved, ready-to-call connection to a single provider. */
class IProviderClient {
public:
    virtual ~IProviderClient() = default;

    // Implementations must normalize every transport/auth/rate-limit/
    // provider-side failure into a typed, provider-neutral ai::common::Error
    // instead of throwing - stable codes this project's callers match on:
    // "provider.auth_failed", "provider.rate_limited", "provider.timeout",
    // "provider.network_error", "provider.invalid_response", and the
    // catch-all "provider.request_failed". The Error's message must never
    // contain the API key/secret used for the call. stopToken is checked
    // cooperatively for cancellation in addition to request.timeout bounding
    // the call.
    virtual common::Result<ProviderChatResponse> send(
        const ProviderChatRequest& request,
        std::stop_token stopToken) = 0;
};

/**
 * Resolves a live IProviderClient for a given catalog Provider. Owns
 * credential resolution internally (through ICredentialStore) - a caller
 * never sees the secret, only the Result of trying to use it.
 */
class IProviderClientFactory {
public:
    virtual ~IProviderClientFactory() = default;

    // providerKind is the catalog Provider::kind ("openai", "anthropic",
    // "openai_compatible", "local") so an implementation can pick the right
    // transport without a second repository lookup. Typed failures a caller
    // must expect: "credential.missing" (no credentialReference configured
    // or ICredentialStore has nothing stored for it), "credential.unavailable"
    // (the credential store itself is unavailable), "provider.unsupported_kind",
    // and "provider_runtime.unavailable" (no real provider adapter is linked
    // into this build at all - see BackendRuntime's controlled fallback).
    // Shared ownership keeps one resolved client alive for the full
    // asynchronous call. This matters when two Comparison candidates use
    // the same provider while a rotated credential resolves a newer client.
    virtual common::Result<std::shared_ptr<IProviderClient>> clientFor(
        const ProviderId& providerId,
        const std::string& providerKind) = 0;
};

} // namespace ai::application
