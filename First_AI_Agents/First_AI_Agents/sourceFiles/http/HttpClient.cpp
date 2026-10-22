#include "ai/http/HttpClient.hpp"

#include "ai/http/HttpRetryPolicy.hpp"
#include "ai/http/HttpTimeout.hpp"

#include <curl/curl.h>

#include <array>
#include <chrono>
#include <cctype>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace ai::http {

namespace {

class CurlGlobal {
public:
    CurlGlobal()
        : code_(curl_global_init(CURL_GLOBAL_DEFAULT))
    {
    }

    CurlGlobal(const CurlGlobal&) = delete;
    CurlGlobal& operator=(const CurlGlobal&) = delete;

    ~CurlGlobal()
    {
        if (code_ == CURLE_OK) {
            curl_global_cleanup();
        }
    }

    [[nodiscard]] CURLcode code() const noexcept
    {
        return code_;
    }

private:
    CURLcode code_;
};

class CurlHandle {
public:
    CurlHandle()
        : handle_(curl_easy_init())
    {
    }

    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;

    ~CurlHandle()
    {
        if (handle_ != nullptr) {
            curl_easy_cleanup(handle_);
        }
    }

    [[nodiscard]] CURL* get() const noexcept
    {
        return handle_;
    }

private:
    CURL* handle_;
};

class CurlHeaderList {
public:
    CurlHeaderList() = default;

    CurlHeaderList(const CurlHeaderList&) = delete;
    CurlHeaderList& operator=(const CurlHeaderList&) = delete;

    ~CurlHeaderList()
    {
        if (headers_ != nullptr) {
            curl_slist_free_all(headers_);
        }
    }

    [[nodiscard]] curl_slist* get() const noexcept
    {
        return headers_;
    }

    [[nodiscard]] bool append(const std::string& header)
    {
        curl_slist* updated = curl_slist_append(headers_, header.c_str());
        if (updated == nullptr) {
            return false;
        }

        headers_ = updated;
        return true;
    }

private:
    curl_slist* headers_ {nullptr};
};

const CurlGlobal& curlGlobal()
{
    // libcurl requires process-wide initialization before any easy handle is
    // created. A function-local static keeps the setup lazy and shared by all
    // providers that use this transport boundary.
    static const CurlGlobal global;
    return global;
}

std::string toLower(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const char ch : value) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return result;
}

std::string trim(std::string value)
{
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) {
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

std::string extractHost(std::string_view endpoint)
{
    const std::size_t schemeEnd = endpoint.find("://");
    if (schemeEnd == std::string_view::npos) {
        return {};
    }

    const std::size_t authorityStart = schemeEnd + 3;
    const std::size_t authorityEnd = endpoint.find_first_of("/?#", authorityStart);
    std::string authority(endpoint.substr(
        authorityStart,
        authorityEnd == std::string_view::npos ? std::string_view::npos : authorityEnd - authorityStart));

    const std::size_t userInfoEnd = authority.rfind('@');
    if (userInfoEnd != std::string::npos) {
        authority.erase(0, userInfoEnd + 1);
    }

    if (authority.starts_with('[')) {
        const std::size_t bracketEnd = authority.find(']');
        if (bracketEnd == std::string::npos) {
            return {};
        }
        return toLower(std::string_view {authority}.substr(1, bracketEnd - 1));
    }

    const std::size_t portStart = authority.find(':');
    if (portStart != std::string::npos) {
        authority.erase(portStart);
    }

    return toLower(authority);
}

bool isLocalhostEndpoint(std::string_view endpoint)
{
    const std::string lower = toLower(endpoint);
    if (!lower.starts_with("http://")) {
        return false;
    }

    const std::string host = extractHost(lower);
    return host == "localhost" || host == "127.0.0.1" || host == "::1";
}

bool isSecureEndpoint(std::string_view endpoint)
{
    // Provider API keys travel in headers, so remote endpoints must use TLS.
    // Plain HTTP is reserved for local development servers where TLS is often
    // intentionally absent.
    const std::string lower = toLower(endpoint);
    return lower.starts_with("https://") || isLocalhostEndpoint(endpoint);
}

long toCurlMilliseconds(std::chrono::milliseconds timeout, std::string_view timeoutName)
{
    if (timeout.count() <= 0) {
        throw std::invalid_argument(
            "Invalid HTTP " + std::string(timeoutName)
            + " timeout value: expected a positive duration.");
    }

    if (timeout.count() > (std::numeric_limits<long>::max)()) {
        throw std::out_of_range(
            "Invalid HTTP " + std::string(timeoutName)
            + " timeout value: milliseconds value is too large for libcurl.");
    }

    return static_cast<long>(timeout.count());
}

HttpResponse makeTransportError(std::string message)
{
    return HttpResponse {
        0,
        {},
        {},
        std::move(message)
    };
}

std::string secondsText(std::chrono::milliseconds timeout)
{
    std::ostringstream output;
    if (timeout.count() % 1000 == 0) {
        output << (timeout.count() / 1000) << " seconds";
    } else {
        output << timeout.count() << " ms";
    }
    return output.str();
}

std::string curlErrorMessage(
    CURLcode code,
    const std::array<char, CURL_ERROR_SIZE>& errorBuffer,
    std::chrono::milliseconds connectTimeout,
    std::optional<std::chrono::milliseconds> totalTimeout)
{
    std::ostringstream message;
    if (code == CURLE_OPERATION_TIMEDOUT) {
        if (totalTimeout.has_value()) {
            message << "transport timeout: request exceeded total timeout of "
                    << secondsText(*totalTimeout)
                    << " (connect timeout: "
                    << secondsText(connectTimeout)
                    << ")";
        } else {
            message << "transport timeout: connection exceeded connect timeout of "
                    << secondsText(connectTimeout);
        }
    } else {
        message << "curl error " << static_cast<int>(code);
    }

    if (errorBuffer[0] != '\0') {
        message << ": " << errorBuffer.data();
    } else {
        message << ": " << curl_easy_strerror(code);
    }

    return message.str();
}

size_t writeBodyCallback(char* contents, size_t size, size_t nmemb, void* userData)
{
    // libcurl streams response bytes through this callback; providers receive
    // the complete UTF-8 body later and parse it according to their own schema.
    const size_t totalSize = size * nmemb;
    auto* body = static_cast<std::string*>(userData);
    body->append(contents, totalSize);
    return totalSize;
}

size_t writeHeaderCallback(char* buffer, size_t size, size_t nitems, void* userData)
{
    // Headers are captured for diagnostics and future provider-specific needs.
    // Authentication request headers are never copied into HttpResponse.
    const size_t totalSize = size * nitems;
    std::string line(buffer, totalSize);
    line = trim(std::move(line));

    if (line.empty() || line.starts_with("HTTP/")) {
        return totalSize;
    }

    const std::size_t separator = line.find(':');
    if (separator == std::string::npos) {
        return totalSize;
    }

    auto* headers = static_cast<std::unordered_map<std::string, std::string>*>(userData);
    const std::string name = trim(line.substr(0, separator));
    const std::string value = trim(line.substr(separator + 1));
    if (!name.empty()) {
        (*headers)[name] = value;
    }

    return totalSize;
}

int transferProgressCallback(
    void* userData,
    curl_off_t,
    curl_off_t,
    curl_off_t,
    curl_off_t)
{
    const auto* cancellationRequested =
        static_cast<const std::function<bool()>*>(userData);
    if (cancellationRequested == nullptr || !*cancellationRequested) {
        return 0;
    }
    try {
        return (*cancellationRequested)() ? 1 : 0;
    } catch (...) {
        // Exceptions must never cross libcurl's C callback boundary. Treat a
        // broken cancellation probe as a request to abort safely.
        return 1;
    }
}

bool appendHeaders(const std::unordered_map<std::string, std::string>& headers, CurlHeaderList& curlHeaders)
{
    for (const auto& [name, value] : headers) {
        if (!curlHeaders.append(name + ": " + value)) {
            return false;
        }
    }

    return true;
}

// Retry parameters for transient provider/network failures (transport
// errors, HTTP 429, HTTP 5xx). Three attempts with linear 1s/2s backoff caps
// worst-case added latency at 3s while giving genuinely transient failures
// (e.g. the 502 seen in production) a real chance to succeed on retry.
constexpr int kMaxAttempts = 3;
constexpr std::chrono::milliseconds kRetryBaseDelay {1000};

HttpResponse performRequest(const HttpRequest& request)
{
    if (!isSecureEndpoint(request.url)) {
        return makeTransportError("HTTP endpoint must use HTTPS unless the host is localhost.");
    }

    const CurlGlobal& global = curlGlobal();
    if (global.code() != CURLE_OK) {
        return makeTransportError(std::string {"curl_global_init failed: "} + curl_easy_strerror(global.code()));
    }

    CurlHandle curl;
    if (curl.get() == nullptr) {
        return makeTransportError("curl_easy_init failed");
    }

    CurlHeaderList curlHeaders;
    if (!appendHeaders(request.headers, curlHeaders)) {
        return makeTransportError("Failed to allocate curl request headers");
    }

    std::string responseBody;
    std::unordered_map<std::string, std::string> responseHeaders;
    std::array<char, CURL_ERROR_SIZE> errorBuffer {};

    const std::string method = request.method.empty() ? "GET" : request.method;
    const std::chrono::milliseconds connectTimeout =
        request.connectTimeout.value_or(defaultConnectTimeout());
    const std::optional<std::chrono::milliseconds> totalTimeout =
        request.disableTotalTimeout
        ? std::nullopt
        : std::optional<std::chrono::milliseconds> {
              request.totalTimeout.value_or(defaultTotalTimeout())};

    long connectTimeoutMs = 0;
    long totalTimeoutMs = 0;
    try {
        connectTimeoutMs = toCurlMilliseconds(connectTimeout, "connect");
        if (totalTimeout.has_value()) {
            totalTimeoutMs = toCurlMilliseconds(*totalTimeout, "total");
        }
    } catch (const std::exception& ex) {
        return makeTransportError(ex.what());
    }

    curl_easy_setopt(curl.get(), CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, curlHeaders.get());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeBodyCallback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, writeHeaderCallback);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &responseHeaders);
    curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, errorBuffer.data());
    // The connect timeout covers only connection establishment. The total
    // timeout covers the entire non-streaming request, including model work.
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, connectTimeoutMs);
    if (totalTimeout.has_value()) {
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, totalTimeoutMs);
    }
    if (request.cancellationRequested) {
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, transferProgressCallback);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &request.cancellationRequested);
    }
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    // Keep TLS verification explicit because both OpenAI and Anthropic send
    // credentials in headers and rely on the transport layer for confidentiality.
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "AiOrchestrator/0.1");

    if (!request.body.empty() || method == "POST" || method == "PUT" || method == "PATCH") {
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, request.body.data());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
    }

    const CURLcode code = curl_easy_perform(curl.get());
    if (code != CURLE_OK) {
        return makeTransportError(curlErrorMessage(code, errorBuffer, connectTimeout, totalTimeout));
    }

    long statusCode = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &statusCode);

    HttpResponse response;
    response.statusCode = static_cast<int>(statusCode);
    response.body = std::move(responseBody);
    response.headers = std::move(responseHeaders);
    return response;
}

} // namespace

HttpResponse HttpClient::send(const HttpRequest& request)
{
    return sendWithRetry(
        request,
        [&request] { return performRequest(request); },
        [](std::chrono::milliseconds delay) { std::this_thread::sleep_for(delay); },
        kMaxAttempts,
        kRetryBaseDelay);
}

} // namespace ai::http
