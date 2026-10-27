#include "ai/application/ProjectMemoryCompactionService.hpp"

#include "ai/application/conversations/ChatContextAssembler.hpp"
#include "ai/application/CostCalculation.hpp"
#include "ai/application/IdGenerator.hpp"
#include "ai/application/Sha256.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>

namespace ai::application {
namespace {

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

std::string trim(std::string value)
{
    const auto notSpace = [](unsigned char ch) { return std::isspace(ch) == 0; };
    const auto first = std::find_if(value.begin(), value.end(), notSpace);
    const auto last = std::find_if(value.rbegin(), value.rend(), notSpace).base();
    return first < last ? std::string(first, last) : std::string {};
}

std::uint64_t costFor(const Model& model, const models::TokenUsage& usage)
{
    // MW-P2: see CostCalculation.hpp. A compaction is ONE provider response, so it is classified
    // against the model's long-context threshold by its own input_tokens (M2b).
    return computeSingleResponseCostMicros(model, usage).costMicros;
}

std::string safeProviderErrorCode(std::string_view code)
{
    if (code.empty() || code.size() > 128
        || !std::all_of(code.begin(), code.end(), [](unsigned char ch) {
            return std::isalnum(ch) != 0 || ch == '.' || ch == '_' || ch == '-';
        })) {
        return "provider.failed";
    }
    return std::string(code);
}

} // namespace

ProjectMemoryCompactionService::ProjectMemoryCompactionService(
    IProjectMemoryRepository& memoryRepository,
    IProjectMemoryCompactionRepository& compactionRepository,
    IProjectRepository& projects,
    IProviderRepository& providers,
    IModelRepository& models,
    IProviderClientFactory& clients)
    : memoryRepository_(memoryRepository), compactionRepository_(compactionRepository),
      projects_(projects), providers_(providers), models_(models), clients_(clients)
{
}

common::Result<ProjectMemoryCompactionRecord> ProjectMemoryCompactionService::compact(
    const ProjectId& projectId,
    const ProviderId& providerId,
    const ModelId& modelId,
    std::stop_token stopToken)
{
    auto project = projects_.findById(projectId);
    if (!project.success()) return common::Result<ProjectMemoryCompactionRecord>::failure(*project.error());
    if (!project.value().has_value()) {
        return common::Result<ProjectMemoryCompactionRecord>::failure(
            error("project_memory.project_not_found", "Project not found"));
    }

    auto providerResult = providers_.findById(providerId);
    auto modelResult = models_.findById(modelId);
    if (!providerResult.success())
        return common::Result<ProjectMemoryCompactionRecord>::failure(*providerResult.error());
    if (!modelResult.success())
        return common::Result<ProjectMemoryCompactionRecord>::failure(*modelResult.error());
    if (!providerResult.value().has_value() || !providerResult.value()->enabled) {
        return common::Result<ProjectMemoryCompactionRecord>::failure(
            error("project_memory.provider_unavailable", "Selected provider is unavailable"));
    }
    if (!modelResult.value().has_value() || !modelResult.value()->enabled
        || modelResult.value()->providerId != providerId) {
        return common::Result<ProjectMemoryCompactionRecord>::failure(
            error("project_memory.model_unavailable", "Selected model is unavailable"));
    }
    const Provider& provider = *providerResult.value();
    const Model& model = *modelResult.value();

    auto segmentsResult = memoryRepository_.listMemorySegments(projectId);
    if (!segmentsResult.success())
        return common::Result<ProjectMemoryCompactionRecord>::failure(*segmentsResult.error());
    const std::vector<ProjectMemorySegment> segments = std::move(segmentsResult.value());
    if (segments.size() < 2) {
        return common::Result<ProjectMemoryCompactionRecord>::failure(error(
            "project_memory.insufficient_segments_to_compact",
            "At least two memory segments are required to compact"));
    }
    if (segments.size() > kMaxSourceSegments) {
        return common::Result<ProjectMemoryCompactionRecord>::failure(error(
            "project_memory.compaction_input_too_large",
            "Project memory has too many segments for one bounded compaction"));
    }

    // Deterministic, order-preserving concatenation - listMemorySegments()
    // returns rows ordered by (created_at_ms, id), so two compaction attempts
    // over an unchanged segment set always hash to the same sourceContentHash.
    std::string sourceText;
    std::vector<ProjectMemorySegmentId> sourceIds;
    sourceIds.reserve(segments.size());
    for (const auto& segment : segments) {
        const std::size_t addedBytes = segment.id.size() + 1 + segment.summary.size() + 1;
        if (addedBytes > kMaxSourceBytes || sourceText.size() > kMaxSourceBytes - addedBytes) {
            return common::Result<ProjectMemoryCompactionRecord>::failure(error(
                "project_memory.compaction_input_too_large",
                "Project memory exceeds the bounded compaction input size"));
        }
        sourceIds.push_back(segment.id);
        sourceText += segment.id;
        sourceText += ':';
        sourceText += segment.summary;
        sourceText += '\n';
    }
    const std::string sourceHash = sha256Hex(sourceText);

    const std::string systemPrompt =
        "You are compacting a project's accumulated run-memory summaries into a single, "
        "concise summary. Preserve concrete facts, decisions, and constraints; remove "
        "redundancy. Respond with the consolidated summary as plain text only.";
    const std::string userContent = "Combine the following " + std::to_string(segments.size())
        + " memory segments into one consolidated summary:\n\n" + sourceText;

    const std::uint32_t requestMaxOutputTokens = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(model.maxOutputTokens > 0 ? model.maxOutputTokens : 4096, 16384));
    const ProviderChatRequest request {
        provider.id, model.id, model.apiModelId, systemPrompt, userContent,
        std::chrono::milliseconds {30000}, requestMaxOutputTokens, 0.2, true};

    ProjectMemoryCompactionRecord record;
    record.id = generateUuid();
    record.projectId = projectId;
    record.providerId = provider.id;
    record.modelId = model.id;
    record.sourceSegmentIds = sourceIds;
    record.sourceContentHash = sourceHash;
    record.createdAt = std::chrono::system_clock::now();

    auto clientResult = clients_.clientFor(provider.id, provider.kind);
    common::Result<ProviderChatResponse> response = clientResult.success()
        ? clientResult.value()->send(request, stopToken)
        : common::Result<ProviderChatResponse>::failure(*clientResult.error());

    if (stopToken.stop_requested()) {
        record.status = ProjectMemoryCompactionStatus::Cancelled;
        record.errorCode = "provider.cancelled";
        record.errorMessage = "Compaction was cancelled";
    } else if (!response.success()) {
        record.status = ProjectMemoryCompactionStatus::Failed;
        record.errorCode = safeProviderErrorCode(response.error()->code);
        // Provider messages can contain raw response fragments or request
        // details. Persist a stable bounded explanation, never that payload.
        record.errorMessage = "Compaction provider request failed";
    } else {
        record.inputTokens = response.value().usage.inputTokens;
        record.outputTokens = response.value().usage.outputTokens;
        record.costMicros = costFor(model, response.value().usage);

        const std::string summary = trim(response.value().content);
        if (summary.empty()) {
            record.status = ProjectMemoryCompactionStatus::Failed;
            record.errorCode = "project_memory.empty_summary";
            record.errorMessage = "Provider returned an empty compacted summary";
        } else if (summary.size() > kMaxSummaryBytes) {
            record.status = ProjectMemoryCompactionStatus::Failed;
            record.errorCode = "project_memory.summary_too_large";
            record.errorMessage = "Provider returned a compacted summary above the storage limit";
        } else {
            ProjectMemorySegment resultSegment;
            resultSegment.id = generateUuid();
            resultSegment.projectId = projectId;
            // Not tied to a single run: this segment synthesizes multiple
            // prior segments, each of which may belong to a different run.
            resultSegment.runId = {};
            resultSegment.summary = summary;
            resultSegment.sourceTokenCount = estimateChatTokens(sourceText);
            resultSegment.summaryTokenCount = estimateChatTokens(summary);
            resultSegment.createdAt = record.createdAt;

            record.status = ProjectMemoryCompactionStatus::Completed;
            record.resultSegmentId = resultSegment.id;
            const auto committed = compactionRepository_.commitSuccessfulCompaction(
                record, std::move(resultSegment));
            if (!committed.success()) {
                return common::Result<ProjectMemoryCompactionRecord>::failure(*committed.error());
            }
            return common::Result<ProjectMemoryCompactionRecord>::ok(std::move(record));
        }
    }

    const auto inserted = compactionRepository_.insertCompactionRecord(record);
    if (!inserted.success())
        return common::Result<ProjectMemoryCompactionRecord>::failure(*inserted.error());
    return common::Result<ProjectMemoryCompactionRecord>::ok(std::move(record));
}

common::Result<std::vector<ProjectMemoryCompactionRecord>> ProjectMemoryCompactionService::listCompactionRecords(
    const ProjectId& projectId) const
{
    return compactionRepository_.listCompactionRecords(projectId);
}

} // namespace ai::application
