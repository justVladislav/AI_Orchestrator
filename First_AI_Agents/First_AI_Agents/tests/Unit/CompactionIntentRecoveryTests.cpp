// B3-II-B: the owner-startup recovery of the compaction intents against a SCRIPTED intent port (no database, no process, no provider).
// Every verdict of the port is scripted and labelled as such; what real SQL, a real WAL hook and a really killed process do is proven in
// CompactionIntentRecoverySqliteTests.cpp and in the AIOrchestrator_CompactionIntentCrash_tests target. The recovery under test has no
// provider, client, record, ledger or segment dependency at all - its only collaborator is this port - so "no financial effect" is
// also a fact of its type; the tests check that nothing but the guarded mark/close of the port is ever asked for.
// The checks use throwing require().
#include "ai/application/conversations/CompactionIntentRecovery.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionIntentPolicy.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ai::application;
namespace common = ai::common;
using St = CompactionIntentRecoveryStatus;
using O = CompactionIntentObservation;
using M = CompactionIntentMutation;
using Id = CompactionIntentIdentityField;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::chrono::system_clock::time_point at(std::int64_t ms) { return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms)); }

// The instant of the pass, with a sub-millisecond tail that must be cut like the storage cuts it.
const std::chrono::system_clock::time_point kInstant = at(9'000'000) + std::chrono::microseconds(789);
const std::chrono::system_clock::time_point kInstantMs = at(9'000'000);

ProjectMemoryCompactionIntent intentRow(const std::string& id, CompactionIntentState state = CompactionIntentState::Open)
{
    ProjectMemoryCompactionIntent value;
    value.id = id;
    value.projectId = "p1";
    value.providerId = "prov1";
    value.modelId = "model1";
    value.sourceSegmentIds = {"s1", "s2"};
    value.sourceContentHash = std::string(64, 'a');
    value.createdAt = at(2000);
    value.state = state;
    if (state == CompactionIntentState::Unresolved) value.unresolvedMarkedAt = at(5000);
    return value;
}

// ---- the scripted port ---------------------------------------------------------------------------------------------------------------
class Port final : public IProjectMemoryCompactionIntentRepository {
public:
    common::Result<std::vector<ProjectMemoryCompactionIntent>> open {common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok({})};
    common::Result<std::vector<ProjectMemoryCompactionIntent>> unresolved {common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok({})};
    std::deque<CompactionIntentObservationOutcome> observeScript;
    std::deque<CompactionIntentMutationOutcome> markScript;
    std::deque<CompactionIntentMutationOutcome> closeScript;

    int listCalls {0};
    mutable int observeCalls {0};
    int markCalls {0};
    int closeCalls {0};
    int otherCalls {0};
    mutable int unscripted {0};
    std::vector<std::string> log;
    std::vector<std::chrono::system_clock::time_point> markInstants;

    CompactionIntentWriteOutcome recordIntentObserved(const ProjectMemoryCompactionIntent&) override
    {
        ++otherCalls;   // recovery must never admit anything
        return {};
    }
    [[nodiscard]] common::Result<std::optional<ProjectMemoryCompactionIntent>> findIntent(const ProjectMemoryCompactionId&) const override
    {
        ++const_cast<Port*>(this)->otherCalls;
        return common::Result<std::optional<ProjectMemoryCompactionIntent>>::ok(std::nullopt);
    }
    [[nodiscard]] common::Result<std::vector<ProjectMemoryCompactionIntent>> listIntents(
        const ProjectId&, std::optional<CompactionIntentState>) const override
    {
        ++const_cast<Port*>(this)->otherCalls;
        return common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok({});
    }
    [[nodiscard]] common::Result<std::vector<ProjectMemoryCompactionIntent>> listIntentsByState(CompactionIntentState state) const override
    {
        ++const_cast<Port*>(this)->listCalls;
        const_cast<Port*>(this)->log.push_back(state == CompactionIntentState::Open ? "list-open" : "list-unresolved");
        return state == CompactionIntentState::Open ? open : unresolved;
    }
    CompactionIntentMutationOutcome markUnresolvedObserved(
        const CompactionIntentIdentity& expected, std::chrono::system_clock::time_point markedAt) override
    {
        ++markCalls;
        log.push_back("mark:" + expected.id);
        markInstants.push_back(markedAt);
        return next(markScript);
    }
    CompactionIntentMutationOutcome closeIntentObserved(const CompactionIntentIdentity& expected) override
    {
        ++closeCalls;
        log.push_back("close:" + expected.id);
        return next(closeScript);
    }
    [[nodiscard]] CompactionIntentObservationOutcome observeOperation(const CompactionIntentIdentity& expected) const override
    {
        ++observeCalls;
        const_cast<Port*>(this)->log.push_back("observe:" + expected.id);
        if (observeScript.empty()) {
            ++unscripted;
            CompactionIntentObservationOutcome outcome;
            outcome.observation = O::Unreadable;
            outcome.error = common::Error {"test.unscripted_observation", "no observation was scripted"};
            return outcome;
        }
        CompactionIntentObservationOutcome outcome = std::move(const_cast<Port*>(this)->observeScript.front());
        const_cast<Port*>(this)->observeScript.pop_front();
        return outcome;
    }

private:
    CompactionIntentMutationOutcome next(std::deque<CompactionIntentMutationOutcome>& script)
    {
        if (script.empty()) {
            ++unscripted;
            CompactionIntentMutationOutcome outcome;
            outcome.persist.result = common::Result<void>::failure({"test.unscripted_mutation", "no mutation was scripted"});
            return outcome;
        }
        CompactionIntentMutationOutcome outcome = std::move(script.front());
        script.pop_front();
        return outcome;
    }
};

CompactionIntentRecoveryPermit allowed()
{
    return CompactionIntentRecoveryPermit([] { return true; });
}

CompactionIntentRecoveryReport run(Port& port, CompactionIntentRecoveryPermit permit = allowed())
{
    CompactionIntentRecovery recovery(port, std::move(permit), [] { return kInstant; });
    return recovery.run();
}

// ---- scripted observations / mutations -----------------------------------------------------------------------------------------------
CompactionIntentObservationOutcome intentOnly(const ProjectMemoryCompactionIntent& intent)
{
    CompactionIntentObservationOutcome outcome;
    outcome.observation = O::IntentPresent;
    outcome.source = CompactionIntentObservationSource::AtomicPairRead;
    outcome.intent = intent;
    return outcome;
}

CompactionIntentObservationOutcome recordOnly(const CompactionIntentIdentity& identity)
{
    CompactionIntentObservationOutcome outcome;
    outcome.observation = O::FinalRecordPresent;
    outcome.source = CompactionIntentObservationSource::AtomicPairRead;
    outcome.recordIdentity = identity;
    return outcome;
}

CompactionIntentObservationOutcome both(const ProjectMemoryCompactionIntent& intent, const CompactionIntentIdentity& identity)
{
    CompactionIntentObservationOutcome outcome;
    outcome.observation = O::Inconsistent;
    outcome.source = CompactionIntentObservationSource::AtomicPairRead;
    outcome.intent = intent;
    outcome.recordIdentity = identity;
    outcome.error = common::Error {kIntentObservationInconsistentCode, "both rows exist"};
    return outcome;
}

CompactionIntentObservationOutcome plain(O observation)
{
    CompactionIntentObservationOutcome outcome;
    outcome.observation = observation;
    if (observation != O::NotObserved && observation != O::Unreadable) outcome.source = CompactionIntentObservationSource::AtomicPairRead;
    return outcome;
}

CompactionIntentObservationOutcome unreadable(const char* code = "persistence.connection_unusable")
{
    CompactionIntentObservationOutcome outcome = plain(O::Unreadable);
    outcome.error = common::Error {code, std::string("text of ") + code};
    CompactionNativeDetail detail;
    detail.code = 5;
    detail.extendedCode = 5;
    detail.message = "database is locked";
    outcome.primaryNative = detail;
    outcome.diagnostics.push_back({CompactionDiagnosticStage::ProofRead, common::Error {"persistence.read_failed", "layer below"}});
    return outcome;
}

CompactionIntentMutationOutcome applied()
{
    CompactionIntentMutationOutcome outcome;
    outcome.mutation = M::Applied;
    outcome.persist.state = CompactionPersistState::Committed;
    outcome.persist.connection = CompactionConnectionState::Healthy;
    return outcome;
}

CompactionIntentMutationOutcome mutation(M kind)
{
    CompactionIntentMutationOutcome outcome;
    outcome.mutation = kind;
    outcome.persist.state = kind == M::Unconfirmed ? CompactionPersistState::Unknown : CompactionPersistState::NotCommitted;
    return outcome;
}

CompactionIntentMutationOutcome unknownWrite()
{
    CompactionIntentMutationOutcome outcome = mutation(M::Unconfirmed);
    outcome.persist.result = common::Result<void>::failure({"persistence.commit_unknown", "the commit may or may not be stored"});
    CompactionNativeDetail detail;
    detail.code = 10;
    detail.extendedCode = 522;
    detail.message = "disk I/O error";
    outcome.persist.primaryNative = detail;
    outcome.persist.diagnostics.push_back({CompactionDiagnosticStage::Rollback, common::Error {"persistence.rollback_failed", "no transaction"}});
    return outcome;
}

int anomalyCauses(const CompactionIntentRecoveryReport& report)
{
    int count = 0;
    for (const auto& entry : report.diagnostics) {
        if (entry.stage == CompactionDiagnosticStage::IntentRecovery && entry.error.code == kIntentObservationInconsistentCode
            && entry.error.message.rfind("The intent and a final record with the expected identity were observed together", 0) == 0) {
            ++count;
        }
    }
    return count;
}

// R1a: the native detail of the Unknown write (see unknownWrite()) stays bound to that write's own error, confirmed or not.
void requireUnknownWriteNative(const CompactionIntentRecoveryReport& report, const std::string& what)
{
    require(report.writeCauses.size() == 1 && report.writeCauses[0].cause.code == "persistence.commit_unknown"
                && report.writeCauses[0].cause.message == "the commit may or may not be stored" && report.writeCauses[0].native.code == 10
                && report.writeCauses[0].native.extendedCode == 522 && report.writeCauses[0].native.message == "disk I/O error",
        what + ": the write's native detail (code, extendedCode, message) stays bound to ITS cause, also when the transition was confirmed");
}

void requireNothingUnscripted(const Port& port)
{
    require(port.unscripted == 0, "every port answer the pass asked for was scripted (an unscripted call is a failure)");
    require(port.otherCalls == 0, "the pass never admits, finds or lists by project: only the two state lists, the pair read, mark and close");
}

// ---- 1. the permit -------------------------------------------------------------------------------------------------------------------
void testARefusedCallTouchesNoPort()
{
    for (int variant = 0; variant < 2; ++variant) {
        Port port;
        port.open = common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok({intentRow("a")});
        const auto report = variant == 0 ? run(port, CompactionIntentRecoveryPermit {})
                                         : run(port, CompactionIntentRecoveryPermit([] { return false; }));
        require(report.status == St::Unavailable && report.firstError.has_value() && report.firstError->code == kIntentRecoveryForbiddenCode,
            "a default or a false permit is refused with intent_recovery_forbidden");
        require(port.listCalls == 0 && port.observeCalls == 0 && port.markCalls == 0 && port.closeCalls == 0 && port.otherCalls == 0,
            "...BEFORE any port is touched");
        require(report.inspected == 0 && report.markedUnresolved == 0 && report.closedConfirmed == 0, "and nothing is counted");
    }
}

// ---- 2. the transitions --------------------------------------------------------------------------------------------------------------
void testAnOpenIntentBecomesUnresolvedOnceAndTheInstantIsWholeMilliseconds()
{
    Port port;
    const auto row = intentRow("op1");
    port.open = common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok({row});
    port.observeScript.push_back(intentOnly(row));
    port.markScript.push_back(applied());
    const auto report = run(port);
    require(report.status == St::Complete && report.inspected == 1 && report.markedUnresolved == 1 && report.alreadyUnresolved == 0
                && report.closedConfirmed == 0 && report.conflicts == 0 && report.unconfirmed == 0 && !report.firstError.has_value(),
        "open -> unresolved: confirmed, Complete");
    require(port.markCalls == 1 && port.closeCalls == 0 && port.observeCalls == 1, "ONE read, ONE guarded mark, no close");
    require(port.markInstants.size() == 1 && port.markInstants[0] == kInstantMs, "the mark carries the pass instant cut to whole milliseconds");
    require(port.listCalls == 2 && port.log.front() == "list-open" && port.log[1] == "list-unresolved",
        "both strict lists are read up front, open first");
    requireNothingUnscripted(port);
}

void testAnAlreadyUnresolvedIntentIsNotTouched()
{
    Port port;
    const auto row = intentRow("op1", CompactionIntentState::Unresolved);
    port.unresolved = common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok({row});
    port.observeScript.push_back(intentOnly(row));
    const auto report = run(port);
    require(report.status == St::Complete && report.alreadyUnresolved == 1 && report.markedUnresolved == 0 && report.unconfirmed == 0,
        "unresolved stays unresolved: counted as such, not as a change");
    require(port.markCalls == 0 && port.closeCalls == 0, "its markedAt is never rewritten: no mark, no close");
    requireNothingUnscripted(port);
}

void testAConsistentFinalRecordIsLeftAloneAndNotCounted()
{
    Port port;
    const auto row = intentRow("op1");
    port.open = common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok({row});
    port.observeScript.push_back(recordOnly(compactionIntentIdentityOf(row)));
    const auto report = run(port);
    require(report.status == St::Complete && report.inspected == 1 && report.markedUnresolved == 0 && report.closedConfirmed == 0
                && report.alreadyUnresolved == 0 && report.conflicts == 0 && report.unconfirmed == 0,
        "a record without an intent is a finished operation: nothing written, nothing counted but 'inspected'");
    require(port.markCalls == 0 && port.closeCalls == 0, "no mutation");
    requireNothingUnscripted(port);
}

void testAMatchingPairIsClosedDefensivelyAndTheAnomalyIsKept()
{
    Port port;
    const auto row = intentRow("op1");
    port.open = common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok({row});
    port.observeScript.push_back(both(row, compactionIntentIdentityOf(row)));
    port.closeScript.push_back(applied());
    const auto report = run(port);
    require(report.status == St::Complete && report.closedConfirmed == 1 && report.markedUnresolved == 0 && report.conflicts == 0,
        "a pair whose BOTH sides carry the expected identity is closed (confirmed), Complete");
    require(port.closeCalls == 1 && port.markCalls == 0, "one guarded close, no mark");
    bool anomalyKept = false;
    for (const auto& entry : report.diagnostics) {
        if (entry.stage == CompactionDiagnosticStage::IntentRecovery && entry.error.code == kIntentObservationInconsistentCode) anomalyKept = true;
    }
    require(anomalyKept && anomalyCauses(report) == 1, "the anomaly itself is kept as ONE diagnostic");
    require(report.writeCauses.empty(), "a plain confirmed close has no write error, so no native cause is invented");
    requireNothingUnscripted(port);
}

// ---- 3. conflicts: every field, on either side ------------------------------------------------------------------------------------------
void testAnyDifferingIdentityFieldIsAConflictAndChangesNothing()
{
    const std::vector<std::pair<Id, std::function<void(ProjectMemoryCompactionIntent&)>>> variants = {
        {Id::ProjectId, [](ProjectMemoryCompactionIntent& i) { i.projectId = "p2"; }},
        {Id::ProviderId, [](ProjectMemoryCompactionIntent& i) { i.providerId = "prov2"; }},
        {Id::ModelId, [](ProjectMemoryCompactionIntent& i) { i.modelId = "model2"; }},
        {Id::SourceSegmentIds, [](ProjectMemoryCompactionIntent& i) { i.sourceSegmentIds = {"s2", "s1"}; }},
        {Id::SourceContentHash, [](ProjectMemoryCompactionIntent& i) { i.sourceContentHash = std::string(64, 'b'); }},
        {Id::CreatedAt, [](ProjectMemoryCompactionIntent& i) { i.createdAt = at(2001); }},
        {Id::Id, [](ProjectMemoryCompactionIntent& i) { i.id = "other"; }},
    };
    for (const auto& [field, apply] : variants) {
        for (int side = 0; side < 2; ++side) {   // 0: the INTENT differs, 1: the RECORD identity differs
            Port port;
            const auto row = intentRow("op1");
            ProjectMemoryCompactionIntent changed = row;
            apply(changed);
            port.open = common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok({row});
            port.observeScript.push_back(side == 0 ? both(changed, compactionIntentIdentityOf(row)) : both(row, compactionIntentIdentityOf(changed)));
            const auto report = run(port);
            const std::string what = std::string(toString(field)) + (side == 0 ? " (intent)" : " (record)");
            require(report.status == St::Partial && report.conflicts == 1 && report.closedConfirmed == 0 && report.markedUnresolved == 0,
                what + ": a conflict, Partial, nothing confirmed");
            require(port.markCalls == 0 && port.closeCalls == 0, what + ": nothing is closed or marked");
            require(report.firstError.has_value()
                        && report.firstError->code == (side == 0 ? kIntentIdentityConflictCode : kIntentRecordConflictCode)
                        && report.firstError->message.find(toString(field)) != std::string::npos,
                what + ": the right conflict code, naming the first differing field");
            requireNothingUnscripted(port);
        }
    }
}

void testAnInconsistentRowWithNoRowsIsAConflictWithTheOriginalCause()
{
    Port port;
    const auto row = intentRow("op1");
    port.open = common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok({row});
    CompactionIntentObservationOutcome odd = plain(O::Inconsistent);
    odd.error = common::Error {kIntentObservationInconsistentCode, "the pair contradicts itself"};
    port.observeScript.push_back(odd);
    const auto report = run(port);
    require(report.conflicts == 1 && report.firstError.has_value() && report.firstError->message == "the pair contradicts itself"
                && port.closeCalls == 0 && port.markCalls == 0,
        "an Inconsistent that does not carry both matching rows is never cleaned up; its cause is kept");
}

// ---- 4. unreadable / unavailable -----------------------------------------------------------------------------------------------------
void testUnreadableAndVanishedRowsAreUnconfirmedAndKeepTheirCauses()
{
    for (int variant = 0; variant < 3; ++variant) {
        Port port;
        const auto row = intentRow("op1");
        port.open = common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok({row});
        port.observeScript.push_back(variant == 0 ? unreadable() : variant == 1 ? plain(O::NotObserved) : plain(O::NeitherPresent));
        const auto report = run(port);
        require(report.status == St::Partial && report.unconfirmed == 1 && report.markedUnresolved == 0 && port.markCalls == 0 && port.closeCalls == 0,
            "an unreadable / unobserved / vanished row is unconfirmed, Partial, and nothing is written");
        require(report.firstError.has_value(), "and it always carries a reason");
        if (variant == 0) {
            require(report.firstError->code == "persistence.connection_unusable" && report.firstNative.has_value() && report.firstNative->extendedCode == 5,
                "the ORIGINAL code, message and native detail");
            bool renamed = false;
            for (const auto& entry : report.diagnostics) {
                if (entry.stage == CompactionDiagnosticStage::IntentRecovery && entry.error.code == "persistence.read_failed") renamed = true;
            }
            require(renamed, "the layer below's generic cause is named for the recovery");
        }
        if (variant == 2) require(report.firstError->code == kIntentRecoveryUnconfirmedCode, "a vanished row concludes nothing about delivery");
    }
}

void testAListThatCannotBeReadIsUnavailableAndNothingIsChanged()
{
    for (int variant = 0; variant < 2; ++variant) {
        Port port;
        port.open = common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok({intentRow("op1")});
        const common::Error failure {"persistence.read_failed", "a damaged row"};
        if (variant == 0) {
            port.open = common::Result<std::vector<ProjectMemoryCompactionIntent>>::failure(failure);
        } else {
            port.unresolved = common::Result<std::vector<ProjectMemoryCompactionIntent>>::failure(failure);
        }
        const auto report = run(port);
        require(report.status == St::Unavailable && report.firstError.has_value() && report.firstError->code == failure.code
                    && report.firstError->message == failure.message && report.inspected == 0,
            "an unreadable list is Unavailable with the original error - not '0 unfinished operations'");
        require(port.observeCalls == 0 && port.markCalls == 0 && port.closeCalls == 0,
            "and NOTHING is changed, even when the other list was readable");
    }
}

// ---- 5. an undetermined write ---------------------------------------------------------------------------------------------------------
void testAnUnknownMarkIsConfirmedOnlyByOneProofOfTheExpectedTransition()
{
    struct Case {
        const char* label;
        CompactionIntentObservationOutcome proof;
        bool confirmed;
    };
    ProjectMemoryCompactionIntent marked = intentRow("op1", CompactionIntentState::Unresolved);
    marked.unresolvedMarkedAt = kInstantMs;
    ProjectMemoryCompactionIntent markedByOthers = intentRow("op1", CompactionIntentState::Unresolved);   // markedAt 5000: not ours
    const std::vector<Case> cases = {
        {"proof shows OUR mark (scripted)", intentOnly(marked), true},
        {"proof shows it still open (scripted)", intentOnly(intentRow("op1")), false},
        {"proof shows a mark with another instant (scripted)", intentOnly(markedByOthers), false},
        {"proof is unreadable (scripted)", unreadable("persistence.read_failed"), false},
    };
    for (const Case& item : cases) {
        Port port;
        const auto row = intentRow("op1");
        port.open = common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok({row});
        port.observeScript.push_back(intentOnly(row));
        port.observeScript.push_back(item.proof);
        port.markScript.push_back(unknownWrite());
        const auto report = run(port);
        const std::string what = item.label;
        require(port.markCalls == 1 && port.observeCalls == 2 && port.closeCalls == 0, what + ": ONE mark, ONE initial read, ONE confirming read - no retry");
        require((report.markedUnresolved == 1) == item.confirmed && (report.unconfirmed == 1) == !item.confirmed, what + ": counted only when confirmed");
        require(report.status == (item.confirmed ? St::Complete : St::Partial), what + ": status follows the confirmation");
        bool writeKept = false;
        for (const auto& entry : report.diagnostics) {
            if (entry.error.code == "persistence.commit_unknown" || entry.error.code == "persistence.rollback_failed") writeKept = true;
        }
        require(writeKept, what + ": the write's own error and cleanup cause stay as causes");
        requireUnknownWriteNative(report, what);
        require(!report.firstNative.has_value() || !item.confirmed, what + ": a confirmed change leaves firstNative empty");
        if (!item.confirmed) {
            require(report.firstError.has_value() && report.firstError->code == "persistence.commit_unknown"
                        && report.firstNative.has_value() && report.firstNative->extendedCode == 522,
                what + ": the first error is the ORIGINAL write error with its native detail");
        } else {
            require(!report.firstError.has_value(), what + ": a confirmed change is not an error");
        }
        requireNothingUnscripted(port);
    }
}

void testAnUnknownCloseIsConfirmedOnlyByTheMatchingRecordWithoutIntent()
{
    for (int variant = 0; variant < 2; ++variant) {
        Port port;
        const auto row = intentRow("op1");
        port.open = common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok({row});
        port.observeScript.push_back(both(row, compactionIntentIdentityOf(row)));
        port.observeScript.push_back(variant == 0 ? recordOnly(compactionIntentIdentityOf(row)) : both(row, compactionIntentIdentityOf(row)));
        port.closeScript.push_back(unknownWrite());
        const auto report = run(port);
        require(port.closeCalls == 1 && port.observeCalls == 2, "ONE close, ONE confirming read");
        require(variant == 0 ? (report.closedConfirmed == 1 && report.status == St::Complete)
                             : (report.closedConfirmed == 0 && report.unconfirmed == 1 && report.status == St::Partial),
            "confirmed only by the record without the intent; the pair still being there is unconfirmed");
        const std::string what = variant == 0 ? "confirmed Unknown close" : "unconfirmed Unknown close";
        requireUnknownWriteNative(report, what);
        // R1b: the observation of the matching pair is kept on EVERY branch, once, before the cleanup is judged.
        require(anomalyCauses(report) == 1, what + ": the 'intent and matching record observed together' fact is kept exactly once");
        require(variant == 0 ? (!report.firstError.has_value() && !report.firstNative.has_value())
                             : (report.firstError.has_value() && report.firstError->code == "persistence.commit_unknown" && report.firstNative.has_value()
                                   && report.firstNative->extendedCode == 522),
            what + ": firstError/firstNative are those of an unconfirmed row only");
    }
}

void testAConfirmedWritesNativeIsNeverGluedToALaterRowsFirstError()
{
    Port port;
    const auto a = intentRow("a");
    const auto b = intentRow("b");
    port.open = common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok({a, b});
    ProjectMemoryCompactionIntent markedA = intentRow("a", CompactionIntentState::Unresolved);
    markedA.unresolvedMarkedAt = kInstantMs;
    port.observeScript.push_back(intentOnly(a));
    port.observeScript.push_back(intentOnly(markedA));   // row a: the Unknown mark is confirmed by its one proof
    port.observeScript.push_back(unreadable());          // row b: unreadable, native code 5
    port.markScript.push_back(unknownWrite());
    const auto report = run(port);
    require(report.status == St::Partial && report.markedUnresolved == 1 && report.unconfirmed == 1, "row a confirmed, row b unconfirmed");
    requireUnknownWriteNative(report, "row a");
    require(report.firstError.has_value() && report.firstError->code == "persistence.connection_unusable" && report.firstNative.has_value()
                && report.firstNative->extendedCode == 5 && report.firstNative->message == "database is locked",
        "firstError/firstNative are row b's own: the native detail of row a's confirmed write is not glued to them");
    requireNothingUnscripted(port);
}

void testZeroRowAndForeignResultsOfAMarkAreNeverCountedAsChanges()
{
    struct Case {
        const char* label;
        CompactionIntentMutationOutcome script;
        std::uint64_t unconfirmed;
        std::uint64_t conflicts;
        std::uint64_t already;
        int proofs;
    };
    CompactionIntentMutationOutcome appliedButUnknown = applied();
    appliedButUnknown.persist.state = CompactionPersistState::Unknown;   // a contradiction: Applied that is not Committed
    const std::vector<Case> cases = {
        {"NotFound", mutation(M::NotFound), 1, 0, 0, 0},
        {"IdentityConflict", mutation(M::IdentityConflict), 0, 1, 0, 0},
        {"AlreadyApplied", mutation(M::AlreadyApplied), 0, 0, 1, 0},
        {"Applied but not Committed", appliedButUnknown, 1, 0, 0, 1},
    };
    for (const Case& item : cases) {
        Port port;
        const auto row = intentRow("op1");
        port.open = common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok({row});
        port.observeScript.push_back(intentOnly(row));
        for (int i = 0; i < item.proofs; ++i) port.observeScript.push_back(intentOnly(row));
        port.markScript.push_back(item.script);
        const auto report = run(port);
        const std::string what = item.label;
        require(report.markedUnresolved == 0, what + ": never a confirmed change");
        require(report.unconfirmed == item.unconfirmed && report.conflicts == item.conflicts && report.alreadyUnresolved == item.already,
            what + ": the right classification");
        require(port.observeCalls == 1 + item.proofs && port.markCalls == 1, what + ": no retry, at most one confirming read");
    }
}

// ---- 6. rows are independent; the pass is bounded ---------------------------------------------------------------------------------------
void testAFailingRowNeverStopsTheOthersAndEachRowIsHandledOnce()
{
    Port port;
    const auto a = intentRow("a");
    const auto b = intentRow("b");
    const auto c = intentRow("c");
    const auto d = intentRow("d", CompactionIntentState::Unresolved);
    port.open = common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok({a, b, c});
    port.unresolved = common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok({d});
    port.observeScript.push_back(unreadable());
    port.observeScript.push_back(intentOnly(b));
    port.observeScript.push_back(intentOnly(c));
    port.observeScript.push_back(intentOnly(d));
    port.markScript.push_back(applied());
    port.markScript.push_back(applied());
    const auto report = run(port);
    require(report.status == St::Partial && report.inspected == 4 && report.unconfirmed == 1 && report.markedUnresolved == 2
                && report.alreadyUnresolved == 1,
        "the first row's failure does not stop the others; the counts are of confirmed changes");
    require(report.firstError.has_value() && report.firstError->code == "persistence.connection_unusable", "the FIRST cause is kept");
    const std::vector<std::string> expected = {"list-open", "list-unresolved", "observe:a", "observe:b", "mark:b", "observe:c", "mark:c", "observe:d"};
    require(port.log == expected, "lists first, then each row once in order (a row marked now is not read again from the other list)");
    require(port.listCalls == 2, "the lists are read exactly once");
    requireNothingUnscripted(port);
}

void testCausesAreBoundedAndUtf8Safe()
{
    Port port;
    std::vector<ProjectMemoryCompactionIntent> rows;
    for (int i = 0; i < 12; ++i) rows.push_back(intentRow("op" + std::to_string(i)));
    port.open = common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok(rows);
    std::string longText;
    for (int i = 0; i < 400; ++i) longText += "\xD0\xB4";   // 800 bytes of a 2-byte letter
    for (int i = 0; i < 12; ++i) {
        CompactionIntentObservationOutcome bad = plain(O::Unreadable);
        bad.error = common::Error {"persistence.read_failed", longText};
        port.observeScript.push_back(bad);
    }
    const auto report = run(port);
    require(report.unconfirmed == 12 && report.status == St::Partial, "twelve unconfirmed rows");
    require(report.diagnostics.size() == kMaxCompactionDiagnostics && report.diagnosticsIncomplete, "at most 8 causes and the loss is flagged");
    for (const auto& entry : report.diagnostics) {
        require(entry.error.message.size() <= kMaxCompactionDiagnosticMessageBytes && entry.error.message.size() % 2 == 0,
            "each cause is cut to 512 bytes without splitting a character");
    }
    require(report.firstError.has_value() && report.firstError->message.size() <= kMaxCompactionDiagnosticMessageBytes
                && report.firstError->message.size() % 2 == 0,
        "the first error is bounded the same way");
}

} // namespace

int runCompactionIntentRecoveryTests()
{
    try {
        testARefusedCallTouchesNoPort();
        testAnOpenIntentBecomesUnresolvedOnceAndTheInstantIsWholeMilliseconds();
        testAnAlreadyUnresolvedIntentIsNotTouched();
        testAConsistentFinalRecordIsLeftAloneAndNotCounted();
        testAMatchingPairIsClosedDefensivelyAndTheAnomalyIsKept();
        testAnyDifferingIdentityFieldIsAConflictAndChangesNothing();
        testAnInconsistentRowWithNoRowsIsAConflictWithTheOriginalCause();
        testUnreadableAndVanishedRowsAreUnconfirmedAndKeepTheirCauses();
        testAListThatCannotBeReadIsUnavailableAndNothingIsChanged();
        testAnUnknownMarkIsConfirmedOnlyByOneProofOfTheExpectedTransition();
        testAnUnknownCloseIsConfirmedOnlyByTheMatchingRecordWithoutIntent();
        testAConfirmedWritesNativeIsNeverGluedToALaterRowsFirstError();
        testZeroRowAndForeignResultsOfAMarkAreNeverCountedAsChanges();
        testAFailingRowNeverStopsTheOthersAndEachRowIsHandledOnce();
        testCausesAreBoundedAndUtf8Safe();
    } catch (const std::exception& exception) {
        std::cerr << "CompactionIntentRecoveryTests failed: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "CompactionIntentRecoveryTests: OK\n";
    return 0;
}
