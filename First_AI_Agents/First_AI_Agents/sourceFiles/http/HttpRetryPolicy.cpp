#include "ai/http/HttpRetryPolicy.hpp"

namespace ai::http {

bool isRetryableResponse(const HttpResponse& response)
{
    if (response.hasTransportError())
        return true;

    return response.statusCode == 429
        || (response.statusCode >= 500 && response.statusCode < 600);
}

HttpResponse sendWithRetry(
    const HttpRequest& request,
    const std::function<HttpResponse()>& sendOnce,
    const std::function<void(std::chrono::milliseconds)>& sleepFn,
    int maxAttempts,
    std::chrono::milliseconds baseDelay)
{
    HttpResponse response = sendOnce();

    for (int attempt = 1; attempt < maxAttempts && isRetryableResponse(response); ++attempt) {
        if (request.cancellationRequested && request.cancellationRequested())
            break;

        sleepFn(baseDelay * attempt);

        if (request.cancellationRequested && request.cancellationRequested())
            break;

        response = sendOnce();
    }

    return response;
}

} // namespace ai::http
