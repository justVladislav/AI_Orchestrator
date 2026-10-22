#pragma once

#include "ai/http/HttpRequest.hpp"
#include "ai/models/GenerationOptions.hpp"

#include <chrono>
#include <string_view>

namespace ai::http {

/**
 * @brief Applies provider-neutral timeout settings to an HTTP request.
 *
 * Priority:
 * 1. typed GenerationOptions::timeouts;
 * 2. generic metadata keys: http.connect_timeout_seconds / http.total_timeout_seconds;
 * 3. provider-specific legacy total timeout: <provider>.timeout_seconds;
 * 4. unqualified legacy total timeout: timeout_seconds;
 * 5. HttpClient defaults.
 */
void applyTimeoutOptions(
    HttpRequest& httpRequest,
    const models::GenerationOptions& options,
    std::string_view providerMetadataPrefix);

[[nodiscard]] std::chrono::milliseconds defaultConnectTimeout();
[[nodiscard]] std::chrono::milliseconds defaultTotalTimeout();

} // namespace ai::http
