#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace ai::http {

/**
 * @brief Provider-neutral HTTP request description.
 *
 * Providers fill this structure after translating their domain request into an
 * API call. Keeping it simple avoids leaking transport-library types into
 * provider code.
 */
struct HttpRequest {
    /// Absolute URL including scheme, host, path, and optional query string.
    std::string url;

    /// HTTP verb. Empty values are treated as GET by HttpClient.
    std::string method {"GET"};

    /// Header map already formatted as logical name/value pairs.
    std::unordered_map<std::string, std::string> headers;

    /// Raw request body. Providers are responsible for JSON serialization.
    std::string body;

    /// Optional limit for DNS/TCP/TLS connection establishment.
    std::optional<std::chrono::milliseconds> connectTimeout;

    /// Optional limit for the complete non-streaming HTTP request.
    std::optional<std::chrono::milliseconds> totalTimeout;

    /// When true, do not install a total request deadline. The independent
    /// connect timeout still applies.
    bool disableTotalTimeout {false};

    /// Optional callback checked by libcurl while the transfer is active.
    std::function<bool()> cancellationRequested;
};

} // namespace ai::http
