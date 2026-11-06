#include "ai/application/conversations/ProjectMemoryCompactionAccounting.hpp"

#include "ai/application/text/DiagnosticText.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <utility>

namespace ai::application::compaction_accounting {

namespace {

constexpr std::size_t kMaxFallbackCodeBytes = 128;

CompactionDiagnostic diagnostic(CompactionDiagnosticStage stage, std::string code, std::string message)
{
    return CompactionDiagnostic {stage, common::Error {std::move(code), std::move(message)}};
}

std::chrono::milliseconds millisecondsOf(std::chrono::system_clock::time_point value) noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch());
}

} // namespace

std::string safeFallbackErrorCode(std::string_view primaryCode)
{
    if (primaryCode.empty() || primaryCode.size() > kMaxFallbackCodeBytes
        || !std::all_of(primaryCode.begin(), primaryCode.end(), [](unsigned char ch) {
            return std::isalnum(ch) != 0 || ch == '.' || ch == '_' || ch == '-';
        })) {
        return kFallbackDefaultErrorCode;
    }
    return std::string(primaryCode);
}

ProjectMemoryCompactionRecord makeFailedAccountingRecord(
    const ProjectMemoryCompactionRecord& attempted, const common::Error& primary)
{
    ProjectMemoryCompactionRecord fallback = attempted;   // every accounting field is carried over, none is recomputed
    fallback.status = ProjectMemoryCompactionStatus::Failed;
    fallback.resultSegmentId.reset();                     // no result segment is written by a Failed row
    fallback.errorCode = safeFallbackErrorCode(primary.code);
    fallback.errorMessage = kFallbackErrorMessage;
    return fallback;
}

CompactionAccounting accountingOfWrite(const CompactionPersistOutcome& write) noexcept
{
    if (write.result.success()) {
        return write.state == CompactionPersistState::Committed ? CompactionAccounting::Saved : CompactionAccounting::Unknown;
    }
    switch (write.state) {
    case CompactionPersistState::NotCommitted: return CompactionAccounting::NotSaved;
    case CompactionPersistState::Unknown: return CompactionAccounting::Unknown;
    case CompactionPersistState::Committed: break;   // a failure that claims to be committed contradicts itself
    }
    return CompactionAccounting::Unknown;
}

FallbackDecision decideFallback(const CompactionPersistOutcome& primary)
{
    FallbackDecision decision;
    if (primary.result.success()) {
        // Nothing failed; there is nothing to account for another way.
        decision.accountingWithoutFallback = accountingOfWrite(primary);
        return decision;
    }
    switch (primary.state) {
    case CompactionPersistState::NotCommitted:
        if (primary.connection.has_value() && *primary.connection == CompactionConnectionState::Healthy) {
            decision.attempt = true;
            return decision;
        }
        decision.accountingWithoutFallback = CompactionAccounting::NotSaved;
        decision.reason = diagnostic(CompactionDiagnosticStage::FallbackWrite, "project_memory.fallback_not_attempted",
            !primary.connection.has_value()
                ? "The accounting fallback was not written: the state of the storage connection was not observed"
                : *primary.connection == CompactionConnectionState::Quarantined
                    ? "The accounting fallback was not written: the storage connection is isolated"
                    : "The accounting fallback was not written: the storage connection could not be isolated and is unusable");
        return decision;
    case CompactionPersistState::Unknown:
        decision.accountingWithoutFallback = CompactionAccounting::Unknown;
        decision.reason = diagnostic(CompactionDiagnosticStage::FallbackWrite, "project_memory.fallback_not_attempted",
            "The accounting fallback was not written: it is not proven that the compaction was not stored");
        return decision;
    case CompactionPersistState::Committed:
        break;
    }
    decision.accountingWithoutFallback = CompactionAccounting::Unknown;
    decision.reason = diagnostic(CompactionDiagnosticStage::FallbackWrite, "project_memory.fallback_not_attempted",
        "The accounting fallback was not written: the storage reported a failure together with a committed write");
    return decision;
}

bool fallbackCollidedWithExistingId(const CompactionPersistOutcome& fallback)
{
    return !fallback.result.success() && fallback.result.error()->code == "project_memory.duplicate_id"
        && fallback.connection.has_value() && *fallback.connection == CompactionConnectionState::Healthy;
}

const char* identityDifference(const ProjectMemoryCompactionRecord& expected, const ProjectMemoryCompactionRecord& stored)
{
    if (expected.id != stored.id) return "id";
    if (expected.projectId != stored.projectId) return "project_id";
    if (expected.providerId != stored.providerId) return "provider_id";
    if (expected.modelId != stored.modelId) return "model_id";
    if (expected.sourceSegmentIds != stored.sourceSegmentIds) return "source_segment_ids";
    if (expected.sourceContentHash != stored.sourceContentHash) return "source_content_hash";
    if (expected.resultSegmentId != stored.resultSegmentId) return "result_segment_id";
    if (expected.inputTokens != stored.inputTokens) return "input_tokens";
    if (expected.outputTokens != stored.outputTokens) return "output_tokens";
    if (expected.costMicros != stored.costMicros) return "cost_micros";
    if (expected.costPriceKnown != stored.costPriceKnown) return "cost_price_known";   // nullopt != false != true
    if (expected.status != stored.status) return "status";
    if (expected.errorCode != stored.errorCode) return "error_code";
    if (expected.errorMessage != stored.errorMessage) return "error_message";
    if (millisecondsOf(expected.createdAt) != millisecondsOf(stored.createdAt)) return "created_at";
    return nullptr;
}

CollisionResolution resolveFallbackCollision(const ProjectMemoryCompactionRecord& expected,
    const common::Result<std::optional<ProjectMemoryCompactionRecord>>& lookup)
{
    CollisionResolution resolution;
    if (!lookup.success()) {
        resolution.accounting = CompactionAccounting::Unknown;
        resolution.lookupCause = CompactionDiagnostic {CompactionDiagnosticStage::IdentityCheck, *lookup.error()};   // the original, as it came
        resolution.diagnostic = diagnostic(CompactionDiagnosticStage::IdentityCheck, "project_memory.identity_check_failed",
            "The record that owns the id could not be read back (" + lookup.error()->code + "); it is not known what is stored");
        return resolution;
    }
    if (!lookup.value().has_value()) {
        resolution.accounting = CompactionAccounting::Unknown;
        resolution.diagnostic = diagnostic(CompactionDiagnosticStage::IdentityCheck, "project_memory.identity_contradiction",
            "The id was reported as taken but no record with it was found; it is not known what is stored");
        return resolution;
    }
    if (const char* field = identityDifference(expected, *lookup.value())) {
        resolution.accounting = CompactionAccounting::NotSaved;
        resolution.diagnostic = diagnostic(CompactionDiagnosticStage::IdentityCheck, "project_memory.record_id_conflict",
            std::string("A different compaction record already owns this id (first difference: ") + field + "); nothing was overwritten");
        return resolution;
    }
    resolution.accounting = CompactionAccounting::Saved;
    resolution.diagnostic = diagnostic(CompactionDiagnosticStage::IdentityCheck, "project_memory.record_already_stored",
        "An identical accounting record was already stored under this id; nothing was written again");
    return resolution;
}

CompactionDiagnostic fallbackWriteCause(const common::Error& error)
{
    return CompactionDiagnostic {CompactionDiagnosticStage::FallbackWrite, error};
}

std::vector<CompactionDiagnostic> inFallbackPhase(const std::vector<CompactionDiagnostic>& causes)
{
    std::vector<CompactionDiagnostic> moved = causes;
    for (auto& cause : moved) {
        switch (cause.stage) {
        case CompactionDiagnosticStage::Rollback: cause.stage = CompactionDiagnosticStage::FallbackRollback; break;
        case CompactionDiagnosticStage::ProofRead: cause.stage = CompactionDiagnosticStage::FallbackProofRead; break;
        case CompactionDiagnosticStage::PrimaryWrite: cause.stage = CompactionDiagnosticStage::FallbackWrite; break;
        case CompactionDiagnosticStage::FallbackWrite:
        case CompactionDiagnosticStage::FallbackRollback:
        case CompactionDiagnosticStage::FallbackProofRead:
        case CompactionDiagnosticStage::IdentityCheck: break;
        }
    }
    return moved;
}

void appendDiagnostic(std::vector<CompactionDiagnostic>& into, bool& incomplete, CompactionDiagnostic entry)
{
    if (into.size() >= kMaxCompactionDiagnostics) {
        incomplete = true;
        return;
    }
    if (entry.error.message.size() > kMaxCompactionDiagnosticMessageBytes) {
        const std::string clean = toValidUtf8Diagnostic(entry.error.message);
        entry.error.message = clean.substr(0, utf8SafePrefixLength(clean, kMaxCompactionDiagnosticMessageBytes));
        incomplete = true;
    }
    into.push_back(std::move(entry));
}

void appendDiagnostics(std::vector<CompactionDiagnostic>& into, bool& incomplete, const std::vector<CompactionDiagnostic>& from)
{
    for (const auto& entry : from) appendDiagnostic(into, incomplete, entry);
}

} // namespace ai::application::compaction_accounting
