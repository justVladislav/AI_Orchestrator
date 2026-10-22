#pragma once

#include "ai/http/HttpRequest.hpp"
#include "ai/http/HttpResponse.hpp"

namespace ai::http {

/**
 * @brief Abstract HTTP transport used by network-backed providers.
 *
 * Providers depend on this boundary so request construction and response
 * parsing stay independent from the concrete transport implementation. That
 * makes the provider easier to test and leaves room for a different client on
 * non-Windows platforms later.
 */
class IHttpClient {
public:
    virtual ~IHttpClient() = default;

    /**
     * @brief Sends a single HTTP request and returns a normalized response.
     *
     * Transport-level failures are represented in HttpResponse::errorMessage
     * with statusCode left at 0. HTTP status failures still return the server
     * response body so provider code can extract API-specific error details.
     */
    virtual HttpResponse send(const HttpRequest& request) = 0;
};

} // namespace ai::http
