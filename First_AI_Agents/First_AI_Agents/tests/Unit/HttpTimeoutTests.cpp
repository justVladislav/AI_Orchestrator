#include "ai/http/HttpTimeout.hpp"

#include <cassert>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>

namespace {

using namespace std::chrono_literals;

void assertThrowsContaining(const std::function<void()>& action, const std::string& expected)
{
    bool thrown = false;
    try {
        action();
    } catch (const std::exception& ex) {
        thrown = true;
        assert(std::string {ex.what()}.find(expected) != std::string::npos);
    }

    assert(thrown);
}

} // namespace

int runHttpTimeoutTests()
{
    {
        ai::models::GenerationOptions options;
        options.timeouts.connectTimeout = 7s;
        options.timeouts.totalTimeout = 9s;
        options.metadata["http.connect_timeout_seconds"] = "11";
        options.metadata["http.total_timeout_seconds"] = "22";

        ai::http::HttpRequest request;
        ai::http::applyTimeoutOptions(request, options, "openai");

        assert(request.connectTimeout == 7s);
        assert(request.totalTimeout == 9s);
    }

    {
        ai::models::GenerationOptions options;
        options.metadata["http.connect_timeout_seconds"] = "11";
        options.metadata["http.total_timeout_seconds"] = "22";

        ai::http::HttpRequest request;
        ai::http::applyTimeoutOptions(request, options, "openai");

        assert(request.connectTimeout == 11s);
        assert(request.totalTimeout == 22s);
    }

    {
        ai::models::GenerationOptions options;
        options.metadata["openai.timeout_seconds"] = "33";

        ai::http::HttpRequest request;
        ai::http::applyTimeoutOptions(request, options, "openai");

        assert(!request.connectTimeout.has_value());
        assert(request.totalTimeout == 33s);
    }

    {
        ai::models::GenerationOptions options;
        options.metadata["timeout_seconds"] = "44";

        ai::http::HttpRequest request;
        ai::http::applyTimeoutOptions(request, options, "anthropic");

        assert(!request.connectTimeout.has_value());
        assert(request.totalTimeout == 44s);
    }

    assert(ai::http::defaultConnectTimeout() == 30s);
    assert(ai::http::defaultTotalTimeout() == 120s);

    {
        ai::models::GenerationOptions options;
        options.timeouts.disableTotalTimeout = true;
        options.timeouts.connectTimeout = 7s;
        bool cancelled = false;
        options.cancellationRequested = [&cancelled] { return cancelled; };

        ai::http::HttpRequest request;
        ai::http::applyTimeoutOptions(request, options, "anthropic");
        assert(request.disableTotalTimeout);
        assert(!request.totalTimeout.has_value());
        assert(request.connectTimeout == 7s);
        assert(request.cancellationRequested);
        assert(!request.cancellationRequested());
        cancelled = true;
        assert(request.cancellationRequested());
    }

    assertThrowsContaining([] {
        ai::models::GenerationOptions options;
        options.metadata["http.total_timeout_seconds"] = "0";

        ai::http::HttpRequest request;
        ai::http::applyTimeoutOptions(request, options, "openai");
    }, "positive number of seconds");

    assertThrowsContaining([] {
        ai::models::GenerationOptions options;
        options.timeouts.connectTimeout = 0s;

        ai::http::HttpRequest request;
        ai::http::applyTimeoutOptions(request, options, "openai");
    }, "positive duration");

    return 0;
}
