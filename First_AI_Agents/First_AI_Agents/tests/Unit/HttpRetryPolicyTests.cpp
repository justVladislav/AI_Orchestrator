#include "ai/http/HttpRetryPolicy.hpp"

#include <cassert>
#include <chrono>
#include <deque>
#include <vector>

namespace {

using namespace std::chrono_literals;
using ai::http::HttpRequest;
using ai::http::HttpResponse;

HttpResponse transportErrorResponse()
{
    return HttpResponse {0, {}, {}, "transport timeout"};
}

HttpResponse statusResponse(int statusCode)
{
    return HttpResponse {statusCode, {}, {}, {}};
}

} // namespace

int runHttpRetryPolicyTests()
{
    // isRetryableResponse classification.
    assert(ai::http::isRetryableResponse(transportErrorResponse()));
    assert(ai::http::isRetryableResponse(statusResponse(429)));
    assert(ai::http::isRetryableResponse(statusResponse(500)));
    assert(ai::http::isRetryableResponse(statusResponse(502)));
    assert(ai::http::isRetryableResponse(statusResponse(599)));
    assert(!ai::http::isRetryableResponse(statusResponse(200)));
    assert(!ai::http::isRetryableResponse(statusResponse(404)));
    assert(!ai::http::isRetryableResponse(statusResponse(401)));
    assert(!ai::http::isRetryableResponse(statusResponse(600)));

    // Success on the first attempt: no retry, no sleep.
    {
        int attempts = 0;
        std::vector<std::chrono::milliseconds> sleeps;

        HttpRequest request;
        const HttpResponse result = ai::http::sendWithRetry(
            request,
            [&attempts] {
                ++attempts;
                return statusResponse(200);
            },
            [&sleeps](std::chrono::milliseconds d) { sleeps.push_back(d); },
            3,
            1000ms);

        assert(attempts == 1);
        assert(sleeps.empty());
        assert(result.statusCode == 200);
    }

    // 502, 502, 200: retries twice, succeeds on the third attempt, backoff doubles.
    {
        std::deque<HttpResponse> scripted {statusResponse(502), statusResponse(502), statusResponse(200)};
        int attempts = 0;
        std::vector<std::chrono::milliseconds> sleeps;

        HttpRequest request;
        const HttpResponse result = ai::http::sendWithRetry(
            request,
            [&scripted, &attempts] {
                ++attempts;
                HttpResponse next = scripted.front();
                scripted.pop_front();
                return next;
            },
            [&sleeps](std::chrono::milliseconds d) { sleeps.push_back(d); },
            3,
            1000ms);

        assert(attempts == 3);
        assert(result.statusCode == 200);
        assert(sleeps.size() == 2);
        assert(sleeps[0] == 1000ms);
        assert(sleeps[1] == 2000ms);
    }

    // Non-retryable failure stops immediately, no sleep.
    {
        int attempts = 0;
        std::vector<std::chrono::milliseconds> sleeps;

        HttpRequest request;
        const HttpResponse result = ai::http::sendWithRetry(
            request,
            [&attempts] {
                ++attempts;
                return statusResponse(404);
            },
            [&sleeps](std::chrono::milliseconds d) { sleeps.push_back(d); },
            3,
            1000ms);

        assert(attempts == 1);
        assert(sleeps.empty());
        assert(result.statusCode == 404);
    }

    // Persistent transient failure exhausts maxAttempts and returns the last failure.
    {
        int attempts = 0;

        HttpRequest request;
        const HttpResponse result = ai::http::sendWithRetry(
            request,
            [&attempts] {
                ++attempts;
                return statusResponse(503);
            },
            [](std::chrono::milliseconds) {},
            3,
            1000ms);

        assert(attempts == 3);
        assert(result.statusCode == 503);
    }

    // Cancellation requested before the first retry stops the loop early.
    {
        int attempts = 0;
        bool cancelled = false;

        HttpRequest request;
        request.cancellationRequested = [&cancelled] { return cancelled; };

        const HttpResponse result = ai::http::sendWithRetry(
            request,
            [&attempts, &cancelled] {
                ++attempts;
                cancelled = true;
                return statusResponse(502);
            },
            [](std::chrono::milliseconds) {},
            3,
            1000ms);

        assert(attempts == 1);
        assert(result.statusCode == 502);
    }

    // Cancellation requested during backoff (after sleep, before the next attempt) also stops.
    {
        int attempts = 0;
        int sleepCalls = 0;
        bool cancelled = false;

        HttpRequest request;
        request.cancellationRequested = [&cancelled] { return cancelled; };

        const HttpResponse result = ai::http::sendWithRetry(
            request,
            [&attempts] {
                ++attempts;
                return statusResponse(502);
            },
            [&sleepCalls, &cancelled](std::chrono::milliseconds) {
                ++sleepCalls;
                cancelled = true;
            },
            3,
            1000ms);

        assert(attempts == 1);
        assert(sleepCalls == 1);
        assert(result.statusCode == 502);
    }

    return 0;
}
