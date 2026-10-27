#include "ai/application/ProjectMemoryService.hpp"

#include "ai/application/conversations/ChatContextAssembler.hpp"
#include "ai/application/IdGenerator.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>

namespace ai::application {

namespace {

// Explicit, documented budgets (PROJECT_MEMORY_PLAN.md Task B): the run task
// gets roughly 1000 tokens, the final output roughly 2000 tokens. Clipping
// only ever happens against these named budgets - never a silent/implicit
// truncation.
constexpr std::uint64_t kTaskTokenBudget = 1000;
constexpr std::uint64_t kFinalOutputTokenBudget = 2000;

// Mirrors the fixed framing constant inside estimateChatTokens() so the clip
// below stays the exact inverse of that estimator.
constexpr std::uint64_t kEstimatorFramingTokens = 8;

common::Error memoryError(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

// Largest prefix length <= maxBytes that does not split a UTF-8 codepoint:
// backs off over continuation bytes (top bits 10xxxxxx) one at a time.
std::size_t utf8SafePrefixLength(std::string_view text, std::size_t maxBytes)
{
    if (maxBytes >= text.size()) {
        return text.size();
    }
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return cut;
}

// Deterministically clips text to an explicit token budget, measured with the
// same estimateChatTokens() the caller records as summaryTokenCount. Text
// already inside the budget is returned unchanged. The clip point is derived
// as the exact inverse of estimateChatTokens' "framing + ceil(bytes/3)"
// formula, then backed off to the nearest UTF-8-safe boundary - so it never
// splits a multi-byte codepoint and never silently exceeds the budget.
std::string clipToTokenBudget(std::string_view text, std::uint64_t tokenBudget)
{
    if (estimateChatTokens(text) <= tokenBudget) {
        return std::string(text);
    }
    const std::uint64_t allowedContentTokens =
        tokenBudget > kEstimatorFramingTokens ? tokenBudget - kEstimatorFramingTokens : 0;
    const std::size_t maxBytes = static_cast<std::size_t>(allowedContentTokens * 3);
    const std::size_t safeLength = utf8SafePrefixLength(text, std::min(maxBytes, text.size()));
    return std::string(text.substr(0, safeLength));
}

} // namespace

ProjectMemoryService::ProjectMemoryService(IProjectMemoryRepository& repository)
    : repository_(repository)
{
}

common::Result<ProjectMemorySegment> ProjectMemoryService::recordRunOutcome(
    const Project& project,
    const Run& run,
    std::string_view finalOutput)
{
    if (run.status != RunStatus::Completed) {
        return common::Result<ProjectMemorySegment>::failure(memoryError(
            "project_memory.run_not_completed",
            "Project memory can only be recorded for a completed run"));
    }

    // sourceTokenCount reflects the full, unclipped source text so a future
    // re-summarization pass (Task D) knows exactly how much was cut.
    const std::uint64_t sourceTokenCount =
        estimateChatTokens(run.task) + estimateChatTokens(finalOutput);

    const std::string clippedTask = clipToTokenBudget(run.task, kTaskTokenBudget);
    const std::string clippedOutput = clipToTokenBudget(finalOutput, kFinalOutputTokenBudget);

    std::ostringstream summaryText;
    summaryText << "# Project Memory Segment\n\n";
    summaryText << "Project: " << project.name << "\n";
    summaryText << "Run: " << run.title << " (" << run.id << ")\n\n";
    summaryText << "## Task\n\n" << clippedTask << "\n\n";
    summaryText << "## Final Output\n\n" << clippedOutput << "\n";
    std::string summary = summaryText.str();

    ProjectMemorySegment segment;
    segment.id = generateUuid();
    segment.projectId = project.id;
    segment.runId = run.id;
    segment.summaryTokenCount = estimateChatTokens(summary);
    segment.sourceTokenCount = sourceTokenCount;
    segment.createdAt = std::chrono::system_clock::now();
    segment.summary = std::move(summary);

    return repository_.insertMemorySegment(std::move(segment));
}

common::Result<std::vector<ProjectMemorySegment>> ProjectMemoryService::listMemorySegments(
    const ProjectId& projectId) const
{
    return repository_.listMemorySegments(projectId);
}

common::Result<void> ProjectMemoryService::removeMemorySegments(const ProjectId& projectId)
{
    return repository_.removeMemorySegments(projectId);
}

} // namespace ai::application
