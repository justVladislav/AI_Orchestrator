#pragma once

#include "ai/http/HttpRetryPolicy.hpp"
#include "ai/http/IHttpClient.hpp"

namespace ai::http {

/**
 * @brief libcurl-backed implementation of the project HTTP boundary.
 *
 * The concrete implementation lives behind IHttpClient so providers can remain
 * focused on API payloads and error mapping instead of transport calls.
 */
class HttpClient final : public IHttpClient {
public:
    /**
     * @brief Sends the request using libcurl and returns status, body, headers, or error.
     */
    HttpResponse send(const HttpRequest& request) override;

    /**
     * @brief The same request with the project's bounded retry policy, plus a caller-owned veto of retries for
     * a received response that is known to be a durable refusal (see RetryVeto). A non-virtual overload: it
     * changes neither IHttpClient nor the layout of this class.
     */
    HttpResponse send(const HttpRequest& request, const RetryVeto& retryVeto);
};

} // namespace ai::http
