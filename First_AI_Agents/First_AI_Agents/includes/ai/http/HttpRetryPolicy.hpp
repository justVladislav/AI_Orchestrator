#pragma once

#include "ai/http/HttpRequest.hpp"
#include "ai/http/HttpResponse.hpp"

#include <chrono>
#include <functional>

namespace ai::http {

/**
 * @brief Returns true when a response represents a transient failure worth
 * retrying: a transport error, HTTP 429, or any 5xx server error.
 */
[[nodiscard]] bool isRetryableResponse(const HttpResponse& response);

/**
 * @brief Drives a bounded retry loop with linear backoff around a single
 * request attempt.
 *
 * Kept independent of libcurl so it can be exercised with a fake @p sendOnce
 * and an instant @p sleepFn in unit tests. Retries stop early on a
 * non-retryable response, on @p request.cancellationRequested becoming true,
 * or after @p maxAttempts attempts.
 */
[[nodiscard]] HttpResponse sendWithRetry(
    const HttpRequest& request,
    const std::function<HttpResponse()>& sendOnce,
    const std::function<void(std::chrono::milliseconds)>& sleepFn,
    int maxAttempts,
    std::chrono::milliseconds baseDelay);

} // namespace ai::http
