#include "ai/application/conversations/ProjectMemoryCompactionService.hpp"

#include "ai/application/conversations/ChatContextAssembler.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionAccounting.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionIntentTracker.hpp"
#include "ai/application/budget/CostCalculation.hpp"
#include "ai/application/IdGenerator.hpp"
#include "ai/application/Sha256.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>

namespace ai::application {
namespace {

namespace accounting = compaction_accounting;

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

CostBreakdown costFor(const Model& model, const models::TokenUsage& usage)
{
    // MW-P2: see CostCalculation.hpp. A compaction is ONE provider response, so it is classified
    // against the model's long-context threshold by its own input_tokens (M2b). Both results of the ONE calculation are
    // kept: the amount AND whether the price was complete (C2-B/S1) - the record owns both.
    return computeSingleResponseCostMicros(model, usage);
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

// The call failed before there was anything to record (validation, lookups): no attempt, no record, nothing to account for.
ProjectMemoryCompactionOutcome refusedBeforeAttempt(common::Error failure)
{
    ProjectMemoryCompactionOutcome outcome;
    outcome.result = common::Result<ProjectMemoryCompactionRecord>::failure(std::move(failure));
    return outcome;
}

// What one storage call reports about itself, copied into the outcome as it is: the storage's health as last observed, the native
// codes of ITS failed step, its secondary causes (bounded, in order) and its own "a cause was lost" flag.
void takeFirstWrite(ProjectMemoryCompactionOutcome& outcome, const CompactionPersistOutcome& write)
{
    outcome.connection = write.connection;
    outcome.primaryNative = write.primaryNative;
    accounting::appendDiagnostics(outcome.diagnostics, outcome.diagnosticsIncomplete, write.diagnostics);
    if (write.diagnosticsIncomplete) outcome.diagnosticsIncomplete = true;
}

// A final write is CONFIRMED only when the storage says both: success and Committed. Never inferred from the call's result or from
// accounting == Saved (a Saved can also come from an identical row found after a failed insert).
bool isConfirmedCommit(const CompactionPersistOutcome& write) noexcept
{
    return write.result.success() && write.state == CompactionPersistState::Committed;
}

// A terminal record (Failed / Cancelled) is written by ONE insert. The result is the failure OF THAT INSERT (the terminal cause stays
// in the record, which is also returned as observedRecord); there is no second insert.
ProjectMemoryCompactionOutcome finishWithTerminalInsert(IProjectMemoryCompactionRepository& repository,
    CompactionIntentTracker& intent, ProjectMemoryCompactionRecord record, bool responseObserved)
{
    const CompactionPersistOutcome persisted = repository.insertCompactionRecordObserved(record);
    ProjectMemoryCompactionOutcome outcome;
    outcome.responseObserved = responseObserved;
    // Without a received response there is nothing to account for; the storage failure itself is not hidden by that: it is the result.
    outcome.accounting = responseObserved ? accounting::accountingOfWrite(persisted) : CompactionAccounting::NotApplicable;
    takeFirstWrite(outcome, persisted);
    outcome.result = persisted.result.success()
        ? common::Result<ProjectMemoryCompactionRecord>::ok(record)
        : common::Result<ProjectMemoryCompactionRecord>::failure(*persisted.result.error());
    outcome.observedRecord = std::move(record);
    intent.conclude(outcome, isConfirmedCommit(persisted));
    return outcome;
}

// The Completed record is written by the ONE atomic commit. If that fails, the usage the provider already charged for may get its
// own Failed row - once, and only on proof (accounting::decideFallback). The result stays the primary failure either way.
ProjectMemoryCompactionOutcome finishWithCommit(IProjectMemoryCompactionRepository& repository, CompactionIntentTracker& intent,
    ProjectMemoryCompactionRecord record, ProjectMemorySegment resultSegment)
{
    const CompactionPersistOutcome primary = repository.commitSuccessfulCompactionObserved(record, std::move(resultSegment));
    ProjectMemoryCompactionOutcome outcome;
    outcome.responseObserved = true;
    takeFirstWrite(outcome, primary);
    if (primary.result.success()) {
        outcome.accounting = accounting::accountingOfWrite(primary);
        outcome.result = common::Result<ProjectMemoryCompactionRecord>::ok(record);
        outcome.observedRecord = std::move(record);
        intent.conclude(outcome, isConfirmedCommit(primary));
        return outcome;
    }

    outcome.result = common::Result<ProjectMemoryCompactionRecord>::failure(*primary.result.error());
    outcome.observedRecord = record;   // as attempted; replaced below only if a fallback row is really tried

    const accounting::FallbackDecision decision = accounting::decideFallback(primary);
    if (!decision.attempt) {
        outcome.accounting = decision.accountingWithoutFallback;
        if (decision.reason.has_value()) {
            accounting::appendDiagnostic(outcome.diagnostics, outcome.diagnosticsIncomplete, *decision.reason);
        }
        intent.conclude(outcome, /*finalWriteConfirmed=*/false);   // no further final write: the pair proof decides what is known
        return outcome;
    }

    ProjectMemoryCompactionRecord fallback = accounting::makeFailedAccountingRecord(record, *primary.result.error());
    const CompactionPersistOutcome written = repository.insertCompactionRecordObserved(fallback);
    outcome.observedRecord = fallback;
    if (written.connection.has_value()) outcome.connection = written.connection;   // the last health actually observed
    outcome.fallbackNative = written.primaryNative;
    // The fallback write's own error (the repository returns it only in its result), then its secondary causes renamed into the
    // fallback phase. The primary error and the primary causes above are not touched.
    if (!written.result.success()) {
        accounting::appendDiagnostic(outcome.diagnostics, outcome.diagnosticsIncomplete, accounting::fallbackWriteCause(*written.result.error()));
    }
    accounting::appendDiagnostics(outcome.diagnostics, outcome.diagnosticsIncomplete, accounting::inFallbackPhase(written.diagnostics));
    if (written.diagnosticsIncomplete) outcome.diagnosticsIncomplete = true;

    if (accounting::fallbackCollidedWithExistingId(written)) {
        // The id is taken: ONE strict lookup of that id (never the history listing) decides whether the row is this very accounting
        // row; nothing is written or overwritten.
        const auto stored = repository.findCompactionRecord(fallback.id);
        const accounting::CollisionResolution resolution = accounting::resolveFallbackCollision(fallback, stored);
        outcome.accounting = resolution.accounting;
        if (resolution.lookupCause.has_value()) {
            accounting::appendDiagnostic(outcome.diagnostics, outcome.diagnosticsIncomplete, *resolution.lookupCause);
        }
        accounting::appendDiagnostic(outcome.diagnostics, outcome.diagnosticsIncomplete, resolution.diagnostic);
        // The insert failed: an identical row found by the lookup is NOT a confirmed write of this call.
        intent.conclude(outcome, /*finalWriteConfirmed=*/false);
        return outcome;
    }
    outcome.accounting = accounting::accountingOfWrite(written);
    intent.conclude(outcome, isConfirmedCommit(written));
    return outcome;
}

} // namespace

ProjectMemoryCompactionService::ProjectMemoryCompactionService(
    IProjectMemoryRepository& memoryRepository,
    IProjectMemoryCompactionRepository& compactionRepository,
    IProjectMemoryCompactionIntentRepository& intentRepository,
    IProjectRepository& projects,
    IProviderRepository& providers,
    IModelRepository& models,
    IProviderClientFactory& clients)
    : memoryRepository_(memoryRepository), compactionRepository_(compactionRepository), intentRepository_(intentRepository),
      projects_(projects), providers_(providers), models_(models), clients_(clients)
{
}

common::Result<ProjectMemoryCompactionRecord> ProjectMemoryCompactionService::compact(
    const ProjectId& projectId,
    const ProviderId& providerId,
    const ModelId& modelId,
    std::stop_token stopToken)
{
    // The view: the result of the ONE observed call, no second execution.
    return compactObserved(projectId, providerId, modelId, stopToken).result;
}

ProjectMemoryCompactionOutcome ProjectMemoryCompactionService::compactObserved(
    const ProjectId& projectId,
    const ProviderId& providerId,
    const ModelId& modelId,
    std::stop_token stopToken)
{
    auto project = projects_.findById(projectId);
    if (!project.success()) return refusedBeforeAttempt(*project.error());
    if (!project.value().has_value()) {
        return refusedBeforeAttempt(error("project_memory.project_not_found", "Project not found"));
    }

    auto providerResult = providers_.findById(providerId);
    auto modelResult = models_.findById(modelId);
    if (!providerResult.success()) return refusedBeforeAttempt(*providerResult.error());
    if (!modelResult.success()) return refusedBeforeAttempt(*modelResult.error());
    if (!providerResult.value().has_value() || !providerResult.value()->enabled) {
        return refusedBeforeAttempt(error("project_memory.provider_unavailable", "Selected provider is unavailable"));
    }
    if (!modelResult.value().has_value() || !modelResult.value()->enabled
        || modelResult.value()->providerId != providerId) {
        return refusedBeforeAttempt(error("project_memory.model_unavailable", "Selected model is unavailable"));
    }
    const Provider& provider = *providerResult.value();
    const Model& model = *modelResult.value();

    auto segmentsResult = memoryRepository_.listMemorySegments(projectId);
    if (!segmentsResult.success()) return refusedBeforeAttempt(*segmentsResult.error());
    const std::vector<ProjectMemorySegment> segments = std::move(segmentsResult.value());
    if (segments.size() < 2) {
        return refusedBeforeAttempt(error(
            "project_memory.insufficient_segments_to_compact",
            "At least two memory segments are required to compact"));
    }
    if (segments.size() > kMaxSourceSegments) {
        return refusedBeforeAttempt(error(
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
            return refusedBeforeAttempt(error(
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

    const auto markCancelled = [&record] {
        record.status = ProjectMemoryCompactionStatus::Cancelled;
        record.errorCode = "provider.cancelled";
        record.errorMessage = "Compaction was cancelled";
    };

    // B3-II-A: the intent shares the identity of the record prepared above (one id, one clock reading). It is only written - once -
    // right before a request could be issued; every path that ends earlier leaves it untouched (admission NotApplicable).
    CompactionIntentTracker intent(intentRepository_, record);

    // A stop that arrived before anything was sent prevents the call: ONE Cancelled terminal record with no usage (0/0/NULL = no
    // recorded estimate, not a claim that nothing was spent), no client resolved or called, no intent written, the source segments
    // untouched.
    if (stopToken.stop_requested()) {
        markCancelled();
        return finishWithTerminalInsert(compactionRepository_, intent, std::move(record), /*responseObserved=*/false);
    }

    auto clientResult = clients_.clientFor(provider.id, provider.kind);
    ProviderChatOutcome outcome {common::Result<ProviderChatResponse>::failure(common::Error {}), std::nullopt};
    if (!clientResult.success()) {
        // No client, no request, no intent: the same Failed terminal record as before B3.
        outcome = ProviderChatOutcome {common::Result<ProviderChatResponse>::failure(*clientResult.error()), std::nullopt};
    } else {
        // Admission: the request below is reachable ONLY through a confirmed, new intent row of this call. A refusal is a complete
        // outcome of its own - no request, no record, no accounting.
        CompactionIntentTracker::Admission admission = intent.admit();
        if (!admission.dispatch) return std::move(*admission.refusal);
        // A stop that arrived after the admission needs no request: the call ends as an ordinary Cancelled terminal record (the
        // closing trigger takes the intent with it). No response exists, so there is no usage to keep.
        if (stopToken.stop_requested()) {
            outcome = ProviderChatOutcome {
                common::Result<ProviderChatResponse>::failure(common::Error {"provider.cancelled", "Compaction was cancelled"}),
                std::nullopt};
        } else {
            // ONE call. sendObserved() is send() plus the received-but-discarded response of a call that was cancelled.
            outcome = clientResult.value()->sendObserved(request, stopToken);
        }
    }
    const common::Result<ProviderChatResponse>& response = outcome.result;

    // Whether a successful provider response was RECEIVED (the usage below is its usage): stated from the branch that read it,
    // never from tokens, an amount or a price flag.
    bool responseObserved = false;

    if (stopToken.stop_requested()) {
        markCancelled();
        // The operation stays Cancelled and replaces nothing; but a response that WAS received is paid for. Its usage is taken from
        // exactly ONE place: the successful result (the stop came after the client returned) or, failing that, the response the
        // client reports as discarded because of this cancellation. No other failure is evidence of a response. Without evidence
        // the record keeps 0/0/NULL. The content of the cancelled answer is never kept.
        const ProviderChatResponse* received = nullptr;
        if (response.success()) {
            received = &response.value();
        } else if (outcome.discardedResponse.has_value() && response.error()->code == "provider.cancelled") {
            received = &*outcome.discardedResponse;
        }
        if (received != nullptr) {
            responseObserved = true;
            record.inputTokens = received->usage.inputTokens;
            record.outputTokens = received->usage.outputTokens;
            const CostBreakdown cost = costFor(model, received->usage);
            record.costMicros = cost.costMicros;
            record.costPriceKnown = cost.priceKnown;
        }
    } else if (!response.success()) {
        record.status = ProjectMemoryCompactionStatus::Failed;
        record.errorCode = safeProviderErrorCode(response.error()->code);
        // Provider messages can contain raw response fragments or request
        // details. Persist a stable bounded explanation, never that payload.
        record.errorMessage = "Compaction provider request failed";
    } else {
        responseObserved = true;
        record.inputTokens = response.value().usage.inputTokens;
        record.outputTokens = response.value().usage.outputTokens;
        const CostBreakdown cost = costFor(model, response.value().usage);
        record.costMicros = cost.costMicros;
        record.costPriceKnown = cost.priceKnown;

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
            return finishWithCommit(compactionRepository_, intent, std::move(record), std::move(resultSegment));
        }
    }

    return finishWithTerminalInsert(compactionRepository_, intent, std::move(record), responseObserved);
}

common::Result<std::vector<ProjectMemoryCompactionRecord>> ProjectMemoryCompactionService::listCompactionRecords(
    const ProjectId& projectId) const
{
    return compactionRepository_.listCompactionRecords(projectId);
}

} // namespace ai::application
