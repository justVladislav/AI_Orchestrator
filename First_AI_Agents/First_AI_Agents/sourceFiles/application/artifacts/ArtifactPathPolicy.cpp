#include "ai/application/artifacts/ArtifactPathPolicy.hpp"

#include <cctype>
#include <chrono>

namespace ai::application {

namespace {

bool isSafeChar(unsigned char ch)
{
    return std::isalnum(ch) != 0 || ch == '.' || ch == '_' || ch == '-';
}

std::string sanitize(std::string_view value, const char* fallback)
{
    std::string result;
    result.reserve(value.size());
    for (const char rawChar : value) {
        const auto ch = static_cast<unsigned char>(rawChar);
        result.push_back(isSafeChar(ch) ? static_cast<char>(ch) : '_');
    }
    // A name made only of dots/dashes could still resolve to "." or ".."
    // once combined with a separator, so strip every leading one.
    std::size_t start = 0;
    while (start < result.size() && (result[start] == '.' || result[start] == '-')) {
        ++start;
    }
    result.erase(0, start);
    if (result.empty()) {
        return fallback;
    }
    return result;
}

} // namespace

std::string sanitizeArtifactFileName(std::string_view suggestedName)
{
    return sanitize(suggestedName, "artifact");
}

std::string sanitizePathSegment(std::string_view id)
{
    return sanitize(id, "unknown");
}

std::string buildArtifactRelativePath(
    std::string_view runId,
    std::string_view suggestedName,
    std::uint64_t uniqueCounter)
{
    const std::string safeRunId = sanitizePathSegment(runId);
    const std::string safeName = sanitizeArtifactFileName(suggestedName);
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                            .count();
    return safeRunId + "/" + std::to_string(nowMs) + "-" + std::to_string(uniqueCounter) + "-" + safeName;
}

} // namespace ai::application
