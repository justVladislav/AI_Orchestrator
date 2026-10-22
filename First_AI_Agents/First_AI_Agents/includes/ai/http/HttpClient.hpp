#pragma once

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
};

} // namespace ai::http
