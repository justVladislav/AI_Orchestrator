#pragma once

#include <string>
#include <unordered_map>

namespace ai::http {

/**
 * @brief Normalized HTTP response returned by IHttpClient implementations.
 *
 * A response can fail in two different ways: the transport can fail before an
 * HTTP response exists, or the server can return a non-2xx status code. The
 * separate helpers below keep provider code explicit about those cases.
 */
struct HttpResponse {
    /// HTTP status code, or 0 when no server response was received.
    int statusCode {0};

    /// Raw response body. Providers parse it according to their own API schema.
    std::string body;

    /// Response headers captured by the transport layer.
    std::unordered_map<std::string, std::string> headers;

    /// Transport error text. Empty means a server response was received.
    std::string errorMessage;

    /**
     * @brief Returns true only for completed transport and HTTP 2xx status.
     */
    [[nodiscard]] bool isSuccess() const noexcept
    {
        return !hasTransportError() && statusCode >= 200 && statusCode < 300;
    }

    /**
     * @brief Returns true when the request failed before receiving HTTP status.
     */
    [[nodiscard]] bool hasTransportError() const noexcept
    {
        return !errorMessage.empty();
    }
};

} // namespace ai::http
