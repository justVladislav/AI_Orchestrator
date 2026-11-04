#include "ai/http/HttpRetryPolicy.hpp"
#include "ai/providers/OpenAIResponseParser.hpp"

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

    // ---- Retry veto (a durable refusal is never re-sent) driven through the REAL retry driver ----
    {
        const auto quotaBody = [](const std::string& code, const std::string& type) {
            return std::string(R"({"error":{"message":"m","type":")") + type + R"(","code":")" + code + R"("}})";
        };
        const auto response = [](int status, std::string body) { return HttpResponse {status, std::move(body), {}, {}}; };
        // The very predicate OpenAIHttpTransport hands to the driver.
        const ai::http::RetryVeto openAiVeto = [](const HttpResponse& received) {
            return !received.hasTransportError()
                && ai::providers::openai_detail::isDurableOpenAIQuotaRefusal(received.statusCode, received.body);
        };
        const auto drive = [&](std::deque<HttpResponse> scripted, const ai::http::RetryVeto& veto,
                               int& attempts, std::vector<std::chrono::milliseconds>& sleeps) {
            HttpRequest request;
            return ai::http::sendWithRetry(
                request,
                [&scripted, &attempts] {
                    ++attempts;
                    HttpResponse next = scripted.size() > 1 ? scripted.front() : scripted.back();
                    if (scripted.size() > 1) scripted.pop_front();
                    return next;
                },
                [&sleeps](std::chrono::milliseconds d) { sleeps.push_back(d); },
                3, 1000ms, veto);
        };

        // Every durable code: exactly ONE send, no wait, and that very response comes back untouched.
        for (const char* code : {"credit_balance_exhausted", "organization_spend_limit_exceeded",
                 "project_spend_limit_exceeded", "organization_usage_limit_exceeded", "insufficient_quota"}) {
            int attempts = 0;
            std::vector<std::chrono::milliseconds> sleeps;
            const auto body = quotaBody(code, "billing");
            const HttpResponse result = drive({response(429, body)}, openAiVeto, attempts, sleeps);
            assert(attempts == 1 && sleeps.empty());
            assert(result.statusCode == 429 && result.body == body);
        }
        {   // the legacy type-only shape
            int attempts = 0;
            std::vector<std::chrono::milliseconds> sleeps;
            (void)drive({response(429, quotaBody("x", "insufficient_quota"))}, openAiVeto, attempts, sleeps);
            assert(attempts == 1 && sleeps.empty());
        }
        // Temporary / unstructured 429 keep the previous policy: three sends, waits 1000 and 2000.
        for (const std::string& body : {quotaBody("rate_limit_exceeded", "requests"), quotaBody("slow_down", "requests"),
                 std::string(), std::string("not json")}) {
            int attempts = 0;
            std::vector<std::chrono::milliseconds> sleeps;
            (void)drive({response(429, body)}, openAiVeto, attempts, sleeps);
            assert(attempts == 3);
            assert(sleeps.size() == 2 && sleeps[0] == 1000ms && sleeps[1] == 2000ms);
        }
        {   // A durable body on a non-429 status is not a quota refusal: 5xx keeps its retries.
            int attempts = 0;
            std::vector<std::chrono::milliseconds> sleeps;
            (void)drive({response(500, quotaBody("insufficient_quota", "insufficient_quota"))}, openAiVeto, attempts, sleeps);
            assert(attempts == 3 && sleeps.size() == 2);
        }
        {   // Transport failures keep their retries.
            int attempts = 0;
            std::vector<std::chrono::milliseconds> sleeps;
            (void)drive({transportErrorResponse()}, openAiVeto, attempts, sleeps);
            assert(attempts == 3 && sleeps.size() == 2);
        }
        {   // The veto is consulted on every response: a temporary 429 is retried, a durable second answer stops at once.
            int attempts = 0;
            std::vector<std::chrono::milliseconds> sleeps;
            const HttpResponse result = drive(
                {response(429, quotaBody("rate_limit_exceeded", "requests")),
                 response(429, quotaBody("credit_balance_exhausted", "billing"))},
                openAiVeto, attempts, sleeps);
            assert(attempts == 2 && sleeps.size() == 1 && result.body.find("credit_balance_exhausted") != std::string::npos);
        }
        {   // Success after a temporary failure is unaffected.
            int attempts = 0;
            std::vector<std::chrono::milliseconds> sleeps;
            const HttpResponse result = drive({response(429, quotaBody("rate_limit_exceeded", "requests")), response(200, "ok")},
                openAiVeto, attempts, sleeps);
            assert(attempts == 2 && result.statusCode == 200);
        }
        {   // Without a veto the generic layer is unchanged: even a quota body is retried like any 429.
            int attempts = 0;
            std::vector<std::chrono::milliseconds> sleeps;
            (void)drive({response(429, quotaBody("credit_balance_exhausted", "billing"))}, ai::http::RetryVeto {}, attempts, sleeps);
            assert(attempts == 3 && sleeps.size() == 2);
        }
        {   // Cancellation keeps its priority over the veto path: nothing is sent after a cancelled wait.
            int attempts = 0;
            int sleepCalls = 0;
            bool cancelled = false;
            HttpRequest request;
            request.cancellationRequested = [&cancelled] { return cancelled; };
            const HttpResponse result = ai::http::sendWithRetry(
                request,
                [&attempts] { ++attempts; return HttpResponse {429, "", {}, {}}; },
                [&](std::chrono::milliseconds) { ++sleepCalls; cancelled = true; },
                3, 1000ms, openAiVeto);
            assert(attempts == 1 && sleepCalls == 1 && result.statusCode == 429);
        }
    }

    return 0;
}
