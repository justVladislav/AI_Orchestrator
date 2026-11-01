#pragma once

#include "ai/http/HttpClient.hpp"
#include "ai/http/IHttpClient.hpp"
#include "ai/providers/OpenAIResponseParser.hpp"

namespace ai::providers {

/**
 * @brief The production transport of EVERY OpenAI path (single-shot provider and agent session).
 *
 * The real libcurl client with the project's bounded retry policy, plus one OpenAI-specific rule supplied through
 * the explicit retry veto: a received response that the shared OpenAI classifier calls a durable credit/quota
 * refusal is returned at once, without waiting and without a second send. Temporary rate limits, 5xx responses and
 * transport failures keep the unchanged retry behavior; the generic HTTP layer knows no OpenAI codes.
 */
class OpenAIHttpTransport final : public http::IHttpClient {
public:
    http::HttpResponse send(const http::HttpRequest& request) override
    {
        return client_.send(request, [](const http::HttpResponse& response) {
            return !response.hasTransportError()
                && openai_detail::isDurableOpenAIQuotaRefusal(response.statusCode, response.body);
        });
    }

private:
    http::HttpClient client_;
};

} // namespace ai::providers
