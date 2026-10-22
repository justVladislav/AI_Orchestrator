#include "ai/http/HttpTimeout.hpp"

#include <charconv>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace ai::http {

namespace {

using namespace std::chrono_literals;

constexpr auto defaultConnect = 30s;
constexpr auto defaultTotal = 120s;

std::string trim(std::string value)
{
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }

    std::size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }

    if (start > 0) {
        value.erase(0, start);
    }

    return value;
}

std::chrono::milliseconds validatePositive(
    std::chrono::milliseconds value,
    std::string_view timeoutName)
{
    if (value.count() <= 0) {
        throw std::invalid_argument(
            "Invalid HTTP " + std::string(timeoutName)
            + " timeout value: expected a positive duration.");
    }

    return value;
}

std::chrono::milliseconds parsePositiveSeconds(
    std::string value,
    std::string_view timeoutName)
{
    value = trim(std::move(value));
    if (value.empty()) {
        throw std::invalid_argument(
            "Invalid HTTP " + std::string(timeoutName)
            + " timeout value: expected a positive number of seconds.");
    }

    long long seconds = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, seconds);
    if (parsed.ec != std::errc {} || parsed.ptr != end || seconds <= 0) {
        throw std::invalid_argument(
            "Invalid HTTP " + std::string(timeoutName)
            + " timeout value: expected a positive number of seconds.");
    }

    constexpr long long maxMilliseconds = (std::numeric_limits<long long>::max)();
    if (seconds > maxMilliseconds / 1000) {
        throw std::out_of_range(
            "Invalid HTTP " + std::string(timeoutName)
            + " timeout value: seconds value is too large.");
    }

    return std::chrono::seconds {seconds};
}

std::optional<std::chrono::milliseconds> metadataSeconds(
    const models::GenerationOptions& options,
    std::string_view key,
    std::string_view timeoutName)
{
    const auto it = options.metadata.find(std::string {key});
    if (it == options.metadata.end()) {
        return std::nullopt;
    }

    return parsePositiveSeconds(it->second, timeoutName);
}

std::optional<std::chrono::milliseconds> resolveConnectTimeout(
    const models::GenerationOptions& options)
{
    if (options.timeouts.connectTimeout.has_value()) {
        return validatePositive(*options.timeouts.connectTimeout, "connect");
    }

    return metadataSeconds(options, "http.connect_timeout_seconds", "connect");
}

std::optional<std::chrono::milliseconds> resolveTotalTimeout(
    const models::GenerationOptions& options,
    std::string_view providerMetadataPrefix)
{
    if (options.timeouts.totalTimeout.has_value()) {
        return validatePositive(*options.timeouts.totalTimeout, "total");
    }

    if (auto timeout = metadataSeconds(options, "http.total_timeout_seconds", "total")) {
        return timeout;
    }

    if (!providerMetadataPrefix.empty()) {
        const std::string providerKey = std::string {providerMetadataPrefix} + ".timeout_seconds";
        if (auto timeout = metadataSeconds(options, providerKey, "total")) {
            return timeout;
        }
    }

    return metadataSeconds(options, "timeout_seconds", "total");
}

} // namespace

void applyTimeoutOptions(
    HttpRequest& httpRequest,
    const models::GenerationOptions& options,
    std::string_view providerMetadataPrefix)
{
    // Workflow can set typed timeouts directly; metadata remains a compatibility
    // layer for older per-provider timeout_seconds overrides.
    httpRequest.connectTimeout = resolveConnectTimeout(options);
    httpRequest.disableTotalTimeout = options.timeouts.disableTotalTimeout;
    httpRequest.totalTimeout = httpRequest.disableTotalTimeout
        ? std::nullopt
        : resolveTotalTimeout(options, providerMetadataPrefix);
    httpRequest.cancellationRequested = options.cancellationRequested;
}

std::chrono::milliseconds defaultConnectTimeout()
{
    return defaultConnect;
}

std::chrono::milliseconds defaultTotalTimeout()
{
    return defaultTotal;
}

} // namespace ai::http
