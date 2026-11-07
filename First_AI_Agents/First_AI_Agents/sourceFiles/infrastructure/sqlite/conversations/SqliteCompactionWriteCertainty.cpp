#include "SqliteCompactionWriteCertainty.hpp"
#include "../SqlitePrimitives.hpp"

#include "ai/application/text/DiagnosticText.hpp"

#include <utility>

namespace ai::infrastructure::sqlite_detail {

namespace {

using application::CompactionDiagnosticStage;
using application::CompactionPersistState;

enum class Presence { Absent, Present, Unreadable };

Presence rowPresence(sqlite3* db, const char* sql, const std::string& id, std::vector<application::CompactionDiagnostic>& diagnostics)
{
    Statement statement;
    const auto prepared = prepare(db, sql, statement);
    if (!prepared.success()) {
        appendDiagnostic(diagnostics, CompactionDiagnosticStage::ProofRead, *prepared.error());
        return Presence::Unreadable;
    }
    const auto bound = bindText(db, statement.get(), 1, id);
    if (!bound.success()) {
        appendDiagnostic(diagnostics, CompactionDiagnosticStage::ProofRead, *bound.error());
        return Presence::Unreadable;
    }
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_ROW) return Presence::Present;
    if (step == SQLITE_DONE) return Presence::Absent;
    appendDiagnostic(diagnostics, CompactionDiagnosticStage::ProofRead,
        databaseError(db, "persistence.read_failed", "Could not read the stored rows that prove the write"));
    return Presence::Unreadable;
}

} // namespace

void appendDiagnostic(std::vector<application::CompactionDiagnostic>& into, CompactionDiagnosticStage stage, common::Error error)
{
    if (into.size() >= application::kMaxCompactionDiagnostics) return;
    if (error.message.size() > application::kMaxCompactionDiagnosticMessageBytes) {
        const std::string clean = application::toValidUtf8Diagnostic(error.message);
        error.message = clean.substr(0, application::utf8SafePrefixLength(clean, application::kMaxCompactionDiagnosticMessageBytes));
    }
    into.push_back({stage, std::move(error)});
}

std::optional<application::CompactionNativeDetail> toNativeDetail(const std::optional<NativeError>& native)
{
    if (!native.has_value()) return std::nullopt;
    application::CompactionNativeDetail detail;
    detail.code = native->code;
    detail.extendedCode = native->extendedCode;
    detail.message = native->message;
    if (detail.message.size() > application::kMaxCompactionDiagnosticMessageBytes) {
        const std::string clean = application::toValidUtf8Diagnostic(detail.message);
        detail.message = clean.substr(0, application::utf8SafePrefixLength(clean, application::kMaxCompactionDiagnosticMessageBytes));
    }
    return detail;
}

WriteProof proveWriteNotStored(sqlite3* db, FaultPlan& fault, const WriteProofRequest& request)
{
    WriteProof proof;
    if (sqlite3_get_autocommit(db) == 0) {
        appendDiagnostic(proof.diagnostics, CompactionDiagnosticStage::ProofRead,
            {"persistence.proof_unavailable", "A transaction is still open, so the stored rows cannot be proven"});
        return proof;
    }
    if (fault.failProofRead) {
        appendDiagnostic(proof.diagnostics, CompactionDiagnosticStage::ProofRead,
            {"persistence.read_failed", "The proof read was not performed (fault injection)"});
        return proof;
    }

    const Presence record = rowPresence(db, "SELECT 1 FROM project_memory_compaction_records WHERE id=?;",
        request.recordId, proof.diagnostics);
    const Presence segment = request.resultSegmentId.has_value()
        ? rowPresence(db, "SELECT 1 FROM project_memory_segments WHERE id=?;", *request.resultSegmentId, proof.diagnostics)
        : Presence::Absent;
    if (record == Presence::Unreadable || segment == Presence::Unreadable) return proof;   // Unknown, causes already recorded

    const bool recordIsOurs = record == Presence::Present && !request.recordIdCollision;
    const bool segmentIsOurs = segment == Presence::Present && !request.segmentIdCollision;
    if (recordIsOurs || segmentIsOurs) {
        appendDiagnostic(proof.diagnostics, CompactionDiagnosticStage::ProofRead,
            {"project_memory.proof_contradiction",
             "The write reported a failure but a row with its id is stored; the outcome is not proven"});
        return proof;   // Unknown
    }
    proof.state = CompactionPersistState::NotCommitted;
    if (record == Presence::Present) {
        appendDiagnostic(proof.diagnostics, CompactionDiagnosticStage::ProofRead,
            {"project_memory.record_id_present",
             "A record with this id already exists; it was not written by this call and it was not compared"});
    }
    if (segment == Presence::Present) {
        appendDiagnostic(proof.diagnostics, CompactionDiagnosticStage::ProofRead,
            {"project_memory.segment_id_present", "A segment with this id already exists; it was not written by this call"});
    }
    return proof;
}

} // namespace ai::infrastructure::sqlite_detail
