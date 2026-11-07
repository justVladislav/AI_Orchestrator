// C2-B/S2b-2: the PURE accounting policy of project-memory compaction (ProjectMemoryCompactionAccounting.hpp). No service, no
// repository, no database, no clock: every rule that decides whether the usage a provider already charged for may be recorded
// another way is checked here, alone. The checks use throwing require(), so they stay active under NDEBUG.
#include "ai/application/conversations/ProjectMemoryCompactionAccounting.hpp"
#include "ai/application/text/DiagnosticText.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ai::application;
namespace policy = ai::application::compaction_accounting;
using std::chrono::microseconds;
using std::chrono::milliseconds;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

ProjectMemoryCompactionRecord attemptedCompleted()
{
    ProjectMemoryCompactionRecord record;
    record.id = "rec-1";
    record.projectId = "project-1";
    record.providerId = "provider-1";
    record.modelId = "model-1";
    record.sourceSegmentIds = {"s3", "s1", "s2"};   // deliberately not sorted: the order is part of the identity
    record.sourceContentHash = std::string(64, 'c');
    record.resultSegmentId = "result-1";
    record.inputTokens = 1234;
    record.outputTokens = 56;
    record.costMicros = 789;
    record.costPriceKnown = false;
    record.status = ProjectMemoryCompactionStatus::Completed;
    record.createdAt = std::chrono::system_clock::time_point(milliseconds(1'700'000'000'123LL) + microseconds(456));
    return record;
}

CompactionPersistOutcome persisted(bool success, CompactionPersistState state, std::optional<CompactionConnectionState> connection,
    const char* errorCode = "persistence.transaction_failed")
{
    CompactionPersistOutcome outcome;
    outcome.result = success ? ai::common::Result<void>::ok()
                             : ai::common::Result<void>::failure({errorCode, "text that must never reach a fallback row"});
    outcome.state = state;
    outcome.connection = connection;
    return outcome;
}

// ---- the fallback row ---------------------------------------------------------------------------------------------------------------
void testTheFallbackRowCarriesTheAccountingFieldsAndNothingElse()
{
    const ProjectMemoryCompactionRecord attempted = attemptedCompleted();
    const ai::common::Error primary {"persistence.transaction_failed", "FOREIGN KEY constraint failed: C:\\secret\\path"};
    const ProjectMemoryCompactionRecord fallback = policy::makeFailedAccountingRecord(attempted, primary);

    require(fallback.id == attempted.id && fallback.projectId == attempted.projectId && fallback.providerId == attempted.providerId
                && fallback.modelId == attempted.modelId,
        "the fallback keeps the id, project, provider and model");
    require(fallback.sourceSegmentIds == attempted.sourceSegmentIds && fallback.sourceContentHash == attempted.sourceContentHash,
        "...the ordered sources and the hash");
    require(fallback.createdAt == attempted.createdAt, "...and the creation time to the last tick");
    require(fallback.inputTokens == 1234 && fallback.outputTokens == 56 && fallback.costMicros == 789,
        "...the usage and the amount (nothing is recalculated)");
    require(fallback.costPriceKnown == std::optional<bool> {false}, "...and the price flag as it was");
    require(fallback.status == ProjectMemoryCompactionStatus::Failed && !fallback.resultSegmentId.has_value(),
        "it is a Failed row with no result segment");
    require(fallback.errorCode == "persistence.transaction_failed", "the code is the primary category");
    require(fallback.errorMessage == policy::kFallbackErrorMessage && fallback.errorMessage.find("secret") == std::string::npos,
        "the text is the fixed one: nothing of the primary message (SQLite text, paths) is copied");
    require(attempted.status == ProjectMemoryCompactionStatus::Completed && attempted.resultSegmentId == std::optional<std::string> {"result-1"},
        "the attempted record is not touched");

    for (const std::optional<bool> flag : {std::optional<bool> {}, std::optional<bool> {false}, std::optional<bool> {true}}) {
        ProjectMemoryCompactionRecord variant = attempted;
        variant.costPriceKnown = flag;
        require(policy::makeFailedAccountingRecord(variant, primary).costPriceKnown == flag,
            "nullopt, false and true are each carried as they are (nullopt is not false)");
    }
    ProjectMemoryCompactionRecord zero = attempted;
    zero.costMicros = 0;
    zero.costPriceKnown = true;   // a true zero stays a true zero
    const auto zeroFallback = policy::makeFailedAccountingRecord(zero, primary);
    require(zeroFallback.costMicros == 0 && zeroFallback.costPriceKnown == std::optional<bool> {true}, "a known zero stays a known zero");
}

void testTheFallbackCodeIsBoundedAndSafe()
{
    require(policy::safeFallbackErrorCode("project_memory.compaction_conflict") == "project_memory.compaction_conflict", "a backend code is kept");
    require(policy::safeFallbackErrorCode("persistence.write_failed") == "persistence.write_failed", "a persistence code is kept");
    const std::string fixed = policy::kFallbackDefaultErrorCode;
    require(fixed == "project_memory.compaction_persistence_failed", "the default code is the documented one");
    require(policy::safeFallbackErrorCode("") == fixed, "an empty code is replaced");
    require(policy::safeFallbackErrorCode(std::string(129, 'a')) == fixed, "a code above 128 bytes is replaced");
    require(policy::safeFallbackErrorCode(std::string(128, 'a')) == std::string(128, 'a'), "128 bytes is allowed");
    for (const char* bad : {"has space", "semi;colon", "slash/x", "quote\"x", "new\nline", "\xD0\xB1\xD1\x83\xD0\xBA\xD0\xB2\xD0\xB0", "C:\\path"}) {
        require(policy::safeFallbackErrorCode(bad) == fixed, std::string("a code with unsafe characters is replaced: ") + bad);
    }
    ProjectMemoryCompactionRecord attempted = attemptedCompleted();
    require(policy::makeFailedAccountingRecord(attempted, {"bad code!", "m"}).errorCode == fixed, "the row uses the replaced code");
}

// ---- when the fallback may be written ------------------------------------------------------------------------------------------------
void testTheFallbackDecisionMatrix()
{
    const std::vector<std::optional<CompactionConnectionState>> connections {
        std::nullopt, CompactionConnectionState::Healthy, CompactionConnectionState::Quarantined, CompactionConnectionState::IsolationFailed};
    for (const auto& connection : connections) {
        const bool healthy = connection.has_value() && *connection == CompactionConnectionState::Healthy;

        const auto notCommitted = policy::decideFallback(persisted(false, CompactionPersistState::NotCommitted, connection));
        require(notCommitted.attempt == healthy, "a fallback is attempted for NotCommitted exactly when the health is EXPLICITLY Healthy");
        if (!healthy) {
            require(notCommitted.accountingWithoutFallback == CompactionAccounting::NotSaved,
                "NotCommitted with a connection that is not explicitly healthy: this write is proven not stored -> NotSaved");
            require(notCommitted.reason.has_value() && notCommitted.reason->error.code == "project_memory.fallback_not_attempted"
                        && notCommitted.reason->stage == CompactionDiagnosticStage::FallbackWrite,
                "...with the reason apart from the primary error");
            require(!connection.has_value() ? notCommitted.reason->error.message.find("not observed") != std::string::npos
                                            : notCommitted.reason->error.message.find("connection") != std::string::npos,
                "...naming the unobserved/unusable connection");
        } else {
            require(!notCommitted.reason.has_value(), "no 'not attempted' reason when it is attempted");
        }

        const auto unknown = policy::decideFallback(persisted(false, CompactionPersistState::Unknown, connection));
        require(!unknown.attempt && unknown.accountingWithoutFallback == CompactionAccounting::Unknown && unknown.reason.has_value(),
            "Unknown: never a fallback, whatever the health says; the accounting is Unknown");

        const auto contradictory = policy::decideFallback(persisted(false, CompactionPersistState::Committed, connection));
        require(!contradictory.attempt && contradictory.accountingWithoutFallback == CompactionAccounting::Unknown,
            "a failure that claims to be committed contradicts itself: no insert, Unknown");

        const auto success = policy::decideFallback(persisted(true, CompactionPersistState::Committed, connection));
        require(!success.attempt && success.accountingWithoutFallback == CompactionAccounting::Saved && !success.reason.has_value(),
            "a successful primary write needs no fallback");
    }
}

void testTheVerdictOfAWriteIsNeverGuessed()
{
    using S = CompactionPersistState;
    require(policy::accountingOfWrite(persisted(true, S::Committed, std::nullopt)) == CompactionAccounting::Saved, "success + Committed -> Saved");
    require(policy::accountingOfWrite(persisted(false, S::NotCommitted, std::nullopt)) == CompactionAccounting::NotSaved, "failure + NotCommitted -> NotSaved");
    require(policy::accountingOfWrite(persisted(false, S::Unknown, std::nullopt)) == CompactionAccounting::Unknown, "failure + Unknown -> Unknown");
    require(policy::accountingOfWrite(persisted(false, S::Committed, std::nullopt)) == CompactionAccounting::Unknown, "failure + Committed -> Unknown");
    require(policy::accountingOfWrite(persisted(true, S::NotCommitted, std::nullopt)) == CompactionAccounting::Unknown, "success + NotCommitted -> Unknown");
    require(policy::accountingOfWrite(persisted(true, S::Unknown, std::nullopt)) == CompactionAccounting::Unknown, "success + Unknown -> Unknown");
}

void testOnlyADuplicateIdOnAHealthyConnectionIsLookedUp()
{
    using C = CompactionConnectionState;
    require(policy::fallbackCollidedWithExistingId(persisted(false, CompactionPersistState::NotCommitted, C::Healthy, "project_memory.duplicate_id")),
        "duplicate_id + Healthy -> the id is looked up");
    for (const std::optional<C> connection : {std::optional<C> {}, std::optional<C> {C::Quarantined}, std::optional<C> {C::IsolationFailed}}) {
        require(!policy::fallbackCollidedWithExistingId(persisted(false, CompactionPersistState::NotCommitted, connection, "project_memory.duplicate_id")),
            "a connection that is not explicitly healthy is never read");
    }
    require(!policy::fallbackCollidedWithExistingId(persisted(false, CompactionPersistState::NotCommitted, C::Healthy, "persistence.write_failed")),
        "another error is not a collision");
    require(!policy::fallbackCollidedWithExistingId(persisted(true, CompactionPersistState::Committed, C::Healthy)), "a success is not a collision");
}

// ---- the identity of a stored row -----------------------------------------------------------------------------------------------------
void testTheIdentityComparisonCoversEveryStoredField()
{
    const ProjectMemoryCompactionRecord expected = attemptedCompleted();
    require(policy::identityDifference(expected, expected) == nullptr, "a record is identical to itself");

    struct Case {
        const char* field;
        ProjectMemoryCompactionRecord (*mutate)(ProjectMemoryCompactionRecord);
    };
    const Case cases[] = {
        {"id", [](auto r) { r.id = "rec-2"; return r; }},
        {"project_id", [](auto r) { r.projectId = "project-2"; return r; }},
        {"provider_id", [](auto r) { r.providerId = "provider-2"; return r; }},
        {"model_id", [](auto r) { r.modelId = "model-2"; return r; }},
        {"source_segment_ids", [](auto r) { r.sourceSegmentIds = {"s1", "s2", "s3"}; return r; }},          // the same SET, another order
        {"source_segment_ids", [](auto r) { r.sourceSegmentIds = {"s3", "s1"}; return r; }},                // a missing source
        {"source_segment_ids", [](auto r) { r.sourceSegmentIds = {"s3", "s1", "s2", "s4"}; return r; }},    // an extra source
        {"source_segment_ids", [](auto r) { r.sourceSegmentIds = {"s3", "s1", "sX"}; return r; }},          // another source
        {"source_content_hash", [](auto r) { r.sourceContentHash = std::string(64, 'd'); return r; }},
        {"result_segment_id", [](auto r) { r.resultSegmentId = "result-2"; return r; }},
        {"result_segment_id", [](auto r) { r.resultSegmentId.reset(); return r; }},
        {"input_tokens", [](auto r) { r.inputTokens += 1; return r; }},
        {"output_tokens", [](auto r) { r.outputTokens += 1; return r; }},
        {"cost_micros", [](auto r) { r.costMicros += 1; return r; }},
        {"cost_price_known", [](auto r) { r.costPriceKnown = true; return r; }},            // false -> true
        {"cost_price_known", [](auto r) { r.costPriceKnown = std::nullopt; return r; }},    // false -> nullopt: NOT the same thing
        {"status", [](auto r) { r.status = ProjectMemoryCompactionStatus::Failed; return r; }},
        {"error_code", [](auto r) { r.errorCode = "x.y"; return r; }},
        {"error_message", [](auto r) { r.errorMessage = "another text"; return r; }},
        {"created_at", [](auto r) { r.createdAt += milliseconds(1); return r; }},           // the NEXT millisecond is a conflict
        {"created_at", [](auto r) { r.createdAt -= milliseconds(1); return r; }},
    };
    for (const Case& c : cases) {
        const char* difference = policy::identityDifference(expected, c.mutate(expected));
        require(difference != nullptr && std::string(difference) == c.field,
            std::string("a difference in ") + c.field + " is a difference (got " + (difference != nullptr ? difference : "none") + ")");
    }

    // nullopt vs false vs true, from every side
    ProjectMemoryCompactionRecord noEvidence = expected;
    noEvidence.costPriceKnown = std::nullopt;
    ProjectMemoryCompactionRecord known = expected;
    known.costPriceKnown = true;
    require(policy::identityDifference(noEvidence, expected) != nullptr && policy::identityDifference(expected, noEvidence) != nullptr
                && policy::identityDifference(known, noEvidence) != nullptr && policy::identityDifference(noEvidence, known) != nullptr
                && policy::identityDifference(known, expected) != nullptr,
        "nullopt, false and true are three different values in both directions");

    // the time is compared at the precision the storage keeps
    ProjectMemoryCompactionRecord sameMillisecond = expected;
    sameMillisecond.createdAt = std::chrono::system_clock::time_point(milliseconds(1'700'000'000'123LL));   // the stored form: .456 ms cut off
    require(policy::identityDifference(expected, sameMillisecond) == nullptr, "a difference inside one millisecond is the storage's precision, not a conflict");
    sameMillisecond.createdAt += microseconds(999);
    require(policy::identityDifference(expected, sameMillisecond) == nullptr, "...up to the last microsecond of that millisecond");
    sameMillisecond.createdAt += microseconds(1);
    require(policy::identityDifference(expected, sameMillisecond) != nullptr, "the next millisecond is a conflict");
}

void testACollisionIsResolvedOnlyByTheExactStoredRow()
{
    using Lookup = ai::common::Result<std::optional<ProjectMemoryCompactionRecord>>;
    ProjectMemoryCompactionRecord expected = policy::makeFailedAccountingRecord(attemptedCompleted(), {"persistence.transaction_failed", "m"});

    const auto same = policy::resolveFallbackCollision(expected, Lookup::ok(expected));
    require(same.accounting == CompactionAccounting::Saved && same.diagnostic.error.code == "project_memory.record_already_stored"
                && same.diagnostic.stage == CompactionDiagnosticStage::IdentityCheck,
        "an identical row -> Saved (idempotent), reported as such");

    ProjectMemoryCompactionRecord stored = expected;
    stored.createdAt = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<milliseconds>(expected.createdAt.time_since_epoch()));
    require(policy::resolveFallbackCollision(expected, Lookup::ok(stored)).accounting == CompactionAccounting::Saved,
        "the row as the storage keeps it (millisecond precision) is the same row");

    ProjectMemoryCompactionRecord otherProject = expected;
    otherProject.projectId = "project-2";
    const auto conflict = policy::resolveFallbackCollision(expected, Lookup::ok(otherProject));
    require(conflict.accounting == CompactionAccounting::NotSaved && conflict.diagnostic.error.code == "project_memory.record_id_conflict"
                && conflict.diagnostic.error.message.find("project_id") != std::string::npos,
        "a row of another project that owns the id -> NotSaved + record_id_conflict, naming the field");
    ProjectMemoryCompactionRecord completed = attemptedCompleted();   // the primary's own Completed form is NOT the fallback row
    require(policy::resolveFallbackCollision(expected, Lookup::ok(completed)).accounting == CompactionAccounting::NotSaved,
        "a different row (here: a Completed one) is a conflict");

    require(!same.lookupCause.has_value() && !conflict.lookupCause.has_value(), "a lookup that worked has no failure cause");
    for (const char* code : {"persistence.read_failed", "project_memory.lookup_unsupported", "persistence.lookup_unavailable", "persistence.connection_unusable"}) {
        const std::string message = std::string("original message of ") + code;
        const auto failed = policy::resolveFallbackCollision(expected, Lookup::failure({code, message}));
        require(failed.accounting == CompactionAccounting::Unknown && failed.diagnostic.error.code == "project_memory.identity_check_failed",
            std::string("a lookup that failed (") + code + ") -> Unknown, the verdict in words");
        require(failed.lookupCause.has_value() && failed.lookupCause->stage == CompactionDiagnosticStage::IdentityCheck
                    && failed.lookupCause->error.code == code && failed.lookupCause->error.message == message,
            std::string("...and the ORIGINAL error (") + code + ") is a separate typed cause: code and message as the repository returned them");
    }
    const auto missing = policy::resolveFallbackCollision(expected, Lookup::ok(std::nullopt));
    require(missing.accounting == CompactionAccounting::Unknown && missing.diagnostic.error.code == "project_memory.identity_contradiction",
        "'taken' yet no row -> Unknown, never Saved");
}

// ---- the phase of a fallback write's causes ---------------------------------------------------------------------------------------
void testTheApplicationNamesThePhaseOfTheFallbackWritesCauses()
{
    const std::vector<CompactionDiagnostic> repositoryShaped {
        {CompactionDiagnosticStage::PrimaryWrite, {"a.write_detail", "m1"}},
        {CompactionDiagnosticStage::Rollback, {"b.rollback", "m2"}},
        {CompactionDiagnosticStage::Rollback, {"c.rollback", "m3"}},
        {CompactionDiagnosticStage::ProofRead, {"d.proof", "m4"}},
        {CompactionDiagnosticStage::FallbackWrite, {"e.already", "m5"}},
        {CompactionDiagnosticStage::FallbackRollback, {"f.already", "m6"}},
        {CompactionDiagnosticStage::FallbackProofRead, {"g.already", "m7"}},
        {CompactionDiagnosticStage::IdentityCheck, {"h.identity", "m8"}},
    };
    const auto moved = policy::inFallbackPhase(repositoryShaped);
    const CompactionDiagnosticStage expected[] {
        CompactionDiagnosticStage::FallbackWrite, CompactionDiagnosticStage::FallbackRollback, CompactionDiagnosticStage::FallbackRollback,
        CompactionDiagnosticStage::FallbackProofRead, CompactionDiagnosticStage::FallbackWrite, CompactionDiagnosticStage::FallbackRollback,
        CompactionDiagnosticStage::FallbackProofRead, CompactionDiagnosticStage::IdentityCheck};
    require(moved.size() == repositoryShaped.size(), "no cause is added or dropped");
    for (std::size_t i = 0; i < moved.size(); ++i) {
        require(moved[i].stage == expected[i], "stage " + std::to_string(i) + " is named for the fallback phase");
        require(moved[i].error.code == repositoryShaped[i].error.code && moved[i].error.message == repositoryShaped[i].error.message,
            "...its code and message, and the ORDER, are untouched");
    }
    require(repositoryShaped[1].stage == CompactionDiagnosticStage::Rollback, "the input is not modified (the primary write's causes stay as they are)");
    require(policy::inFallbackPhase({}).empty(), "nothing in, nothing out");

    const auto cause = policy::fallbackWriteCause({"persistence.write_failed", "Could not insert: disk full"});
    require(cause.stage == CompactionDiagnosticStage::FallbackWrite && cause.error.code == "persistence.write_failed"
                && cause.error.message == "Could not insert: disk full",
        "the fallback insert's own error is a FallbackWrite cause with its original code and message");
}

// ---- the diagnostics -----------------------------------------------------------------------------------------------------------------
void testDiagnosticsAreBoundedInNumberAndTextAndFlagged()
{
    std::vector<CompactionDiagnostic> list;
    bool incomplete = false;
    for (std::size_t i = 0; i < kMaxCompactionDiagnostics; ++i) {
        policy::appendDiagnostic(list, incomplete, {CompactionDiagnosticStage::Rollback, {"code." + std::to_string(i), "short"}});
    }
    require(list.size() == kMaxCompactionDiagnostics && !incomplete, "up to the limit nothing is lost");
    policy::appendDiagnostic(list, incomplete, {CompactionDiagnosticStage::Rollback, {"one.too.many", "short"}});
    require(list.size() == kMaxCompactionDiagnostics && incomplete && list.back().error.code == "code.7",
        "the ninth is dropped, the earlier ones keep their order, and the loss is flagged");

    std::vector<CompactionDiagnostic> longList;
    bool longIncomplete = false;
    // 3-byte characters, so that a cut at 512 falls inside one (512 = 170*3 + 2)
    std::string text;
    while (text.size() < 600) text += "\xE6\xB8\xAC";
    policy::appendDiagnostic(longList, longIncomplete, {CompactionDiagnosticStage::ProofRead, {"long", text}});
    require(longIncomplete && longList.size() == 1, "a shortened text is flagged");
    const std::string& cut = longList.front().error.message;
    require(cut.size() <= kMaxCompactionDiagnosticMessageBytes && cut.size() == 510 && isWellFormedUtf8(cut),
        "the text is at most 512 bytes and never cut inside a UTF-8 character");

    std::vector<CompactionDiagnostic> merged;
    bool mergedIncomplete = false;
    std::vector<CompactionDiagnostic> first(5, CompactionDiagnostic {CompactionDiagnosticStage::Rollback, {"a", "x"}});
    std::vector<CompactionDiagnostic> second(5, CompactionDiagnostic {CompactionDiagnosticStage::FallbackWrite, {"b", "x"}});
    policy::appendDiagnostics(merged, mergedIncomplete, first);
    policy::appendDiagnostics(merged, mergedIncomplete, second);
    require(merged.size() == kMaxCompactionDiagnostics && mergedIncomplete && merged[4].error.code == "a" && merged[5].error.code == "b",
        "merging two lists keeps the order, stops at the limit and flags the loss");
}

} // namespace

int runProjectMemoryCompactionAccountingTests()
{
    try {
        testTheFallbackRowCarriesTheAccountingFieldsAndNothingElse();
        testTheFallbackCodeIsBoundedAndSafe();
        testTheFallbackDecisionMatrix();
        testTheVerdictOfAWriteIsNeverGuessed();
        testOnlyADuplicateIdOnAHealthyConnectionIsLookedUp();
        testTheIdentityComparisonCoversEveryStoredField();
        testACollisionIsResolvedOnlyByTheExactStoredRow();
        testTheApplicationNamesThePhaseOfTheFallbackWritesCauses();
        testDiagnosticsAreBoundedInNumberAndTextAndFlagged();
    } catch (const std::exception& exception) {
        std::cerr << "ProjectMemoryCompactionAccountingTests failed: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "ProjectMemoryCompactionAccountingTests: OK\n";
    return 0;
}
