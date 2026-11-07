// C2-B/S2b-3: the PURE Qt projection of one compaction outcome (projectMemoryOutcomeMap). No event loop, no network, no database:
// every case builds the neutral outcome the application would return and reads the map. The checks throw, so they stay active under
// NDEBUG. Nothing here recomputes a price or an accounting verdict - the projection must only present what it was given.
#include <ai/application/conversations/ProjectMemoryCompactionAccounting.hpp>
#include <ai/application/conversations/ProjectMemoryCompactionOutcome.hpp>
#include <ai/ui/ProjectMemoryOutcomeProjection.hpp>

#include <QCoreApplication>
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

using namespace ai::application;
using A = CompactionAccounting;
using C = CompactionConnectionState;
using St = ProjectMemoryCompactionStatus;
using Stage = CompactionDiagnosticStage;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

QString str(const QVariantMap& map, const char* key) { return map.value(QString::fromLatin1(key)).toString(); }
QVariantMap sub(const QVariantMap& map, const char* key) { return map.value(QString::fromLatin1(key)).toMap(); }
bool flag(const QVariantMap& map, const char* key) { return map.value(QString::fromLatin1(key)).toBool(); }

ProjectMemoryCompactionRecord record(St status, std::optional<bool> priceKnown, std::uint64_t cost, std::uint64_t in = 40, std::uint64_t out = 15)
{
    ProjectMemoryCompactionRecord value;
    value.id = "rec-1";
    value.projectId = "record-project";   // deliberately NOT the project the call was accepted for
    value.providerId = "p";
    value.modelId = "m";
    value.sourceSegmentIds = {"a", "b"};
    value.sourceContentHash = "hash";
    value.inputTokens = in;
    value.outputTokens = out;
    value.costMicros = cost;
    value.costPriceKnown = priceKnown;
    value.status = status;
    if (status != St::Completed) {
        value.errorCode = "project_memory.provider_failed";
        value.errorMessage = "The provider failed";
    }
    value.createdAt = std::chrono::system_clock::now();
    return value;
}

ProjectMemoryCompactionOutcome succeeded(const ProjectMemoryCompactionRecord& stored, A accounting, bool observed = true)
{
    ProjectMemoryCompactionOutcome outcome;
    outcome.result = ai::common::Result<ProjectMemoryCompactionRecord>::ok(stored);
    outcome.responseObserved = observed;
    outcome.accounting = accounting;
    outcome.observedRecord = stored;
    outcome.connection = C::Healthy;
    return outcome;
}

ProjectMemoryCompactionOutcome failedCall(const char* code, const char* message, A accounting, bool observed,
    std::optional<ProjectMemoryCompactionRecord> attempted)
{
    ProjectMemoryCompactionOutcome outcome;
    outcome.result = ai::common::Result<ProjectMemoryCompactionRecord>::failure({code, message});
    outcome.responseObserved = observed;
    outcome.accounting = accounting;
    outcome.observedRecord = std::move(attempted);
    return outcome;
}

// ---- the operation: success of the call is not success of the compaction -------------------------------------------------------------

void testTheOperationStateIsNeverTakenFromResultSuccessAlone()
{
    // A returned Completed record: the only "completed".
    const auto done = projectMemoryOutcomeMap("proj", succeeded(record(St::Completed, true, 250'700), A::Saved));
    require(flag(done, "resultSuccess") && str(done, "operationState") == "completed" && str(done, "recordStatus") == "completed", "completed");
    require(str(done, "primaryCode").isEmpty() && str(done, "terminalCode").isEmpty(), "completed carries no error");
    require(str(done, "headline") == "Memory compacted.", "completed headline");

    // ok(Failed record): the CALL succeeded, the compaction did not.
    const auto failedRecord = projectMemoryOutcomeMap("proj", succeeded(record(St::Failed, true, 120'000), A::Saved));
    require(flag(failedRecord, "resultSuccess") && str(failedRecord, "operationState") == "failed"
        && str(failedRecord, "recordStatus") == "failed", "a returned Failed record is not a successful compaction");
    require(str(failedRecord, "headline") == "Compaction failed.", "failed headline");
    require(str(failedRecord, "primaryCode").isEmpty() && str(failedRecord, "terminalCode") == "project_memory.provider_failed"
        && str(failedRecord, "terminalMessage") == "The provider failed", "the reason of a returned record is the terminal one");

    // ok(Cancelled record).
    const auto cancelled = projectMemoryOutcomeMap("proj", succeeded(record(St::Cancelled, std::nullopt, 0, 0, 0), A::NotApplicable, false));
    require(flag(cancelled, "resultSuccess") && str(cancelled, "operationState") == "cancelled" && str(cancelled, "recordStatus") == "cancelled",
        "cancelled");
    require(str(cancelled, "headline") == "Compaction cancelled.", "cancelled headline");

    // Result::failure is always "failed" and keeps the PRIMARY error whole.
    const auto refused = projectMemoryOutcomeMap("proj", failedCall("project_memory.provider_unavailable", "Provider is not available",
        A::NotApplicable, false, std::nullopt));
    require(!flag(refused, "resultSuccess") && str(refused, "operationState") == "failed", "a refusal is a failure");
    require(str(refused, "primaryCode") == "project_memory.provider_unavailable" && str(refused, "primaryMessage") == "Provider is not available",
        "the primary error is kept");
    require(str(refused, "recordStatus").isEmpty() && str(refused, "observedRecordStatus").isEmpty(), "no record exists");
}

void testTheProjectIsTheOneTheCallWasAcceptedForNotTheRecordsOne()
{
    const auto noRecord = projectMemoryOutcomeMap("accepted-project",
        failedCall("project_memory.insufficient_segments_to_compact", "Too few", A::NotApplicable, false, std::nullopt));
    require(str(noRecord, "projectId") == "accepted-project", "a validation failure has no record, yet the project is known");
    const auto withRecord = projectMemoryOutcomeMap("accepted-project", succeeded(record(St::Completed, true, 1), A::Saved));
    require(str(withRecord, "projectId") == "accepted-project", "the record's own project id is not used");
}

// ---- the fate of the received usage: four verdicts x the meaningful results ---------------------------------------------------------

void testEveryAccountingVerdictKeepsItsOwnWordsAndCodes()
{
    const auto attempted = record(St::Completed, true, 250'700);

    // Saved + success: an ordinary success.
    const auto saved = projectMemoryOutcomeMap("p", succeeded(attempted, A::Saved));
    require(str(saved, "accounting") == "saved" && str(saved, "accountingCode").isEmpty(), "saved has no diagnostic code");
    require(str(saved, "accountingText") == "The usage received from the provider is saved in the history.", "saved words");

    // Saved fallback + primary failure: the compaction was NOT done, the usage IS kept.
    auto fallbackRecord = record(St::Failed, true, 250'700);
    auto savedFallback = failedCall("persistence.transaction_failed", "disk full", A::Saved, true, fallbackRecord);
    const auto fallback = projectMemoryOutcomeMap("p", savedFallback);
    require(!flag(fallback, "resultSuccess") && str(fallback, "operationState") == "failed", "the fallback never makes the call a success");
    require(str(fallback, "primaryCode") == "persistence.transaction_failed" && str(fallback, "accounting") == "saved", "primary stays, accounting saved");
    require(str(fallback, "accountingText").contains("not completed") && str(fallback, "accountingText").contains("failed attempt"),
        "the fallback is described as a failed attempt");
    require(str(fallback, "observedRecordStatus") == "failed" && str(fallback, "recordStatus").isEmpty(), "the attempted record is the fallback");

    // NotSaved: its own code and words; the primary is unchanged.
    const auto notSaved = projectMemoryOutcomeMap("p", failedCall("persistence.transaction_failed", "disk full", A::NotSaved, true, attempted));
    require(str(notSaved, "accounting") == "not_saved" && str(notSaved, "accountingCode") == "project_memory.accounting_not_saved", "not saved code");
    require(str(notSaved, "primaryCode") == "persistence.transaction_failed" && str(notSaved, "primaryMessage") == "disk full", "primary untouched");
    require(!str(notSaved, "accountingMessage").isEmpty() && str(notSaved, "accountingText") == str(notSaved, "accountingMessage"), "not saved words");

    // Unknown: it is NOT claimed that nothing is stored, that memory is unchanged or that a retry is safe.
    const auto unknown = projectMemoryOutcomeMap("p", failedCall("persistence.commit_unknown", "commit unknown", A::Unknown, true, attempted));
    require(str(unknown, "accounting") == "unknown" && str(unknown, "accountingCode") == "project_memory.accounting_unknown", "unknown code");
    const QString words = str(unknown, "accountingText").toLower();
    require(words.contains("not confirmed"), "unknown says it is not confirmed");
    for (const char* forbidden : {"was not stored", "no record", "nothing", "unchanged", "safe", "retry", "try again", "was lost"})
        require(!words.contains(QString::fromLatin1(forbidden)), std::string("unknown must not claim: ") + forbidden);
    require(str(unknown, "accountingCode") != str(notSaved, "accountingCode"), "the two codes are distinct");

    // NotApplicable: no response; NOT "free".
    const auto none = projectMemoryOutcomeMap("p", failedCall("project_memory.provider_unavailable", "x", A::NotApplicable, false, std::nullopt));
    require(str(none, "accounting") == "not_applicable" && str(none, "accountingCode").isEmpty(), "not applicable");
    const QString noneWords = str(none, "accountingText").toLower();
    require(noneWords.contains("no provider response") && !noneWords.contains("free") && !noneWords.contains("$"), "not applicable is not 'free'");
}

void testACancelledCallWhoseRecordCouldNotBeStoredShowsBothCauses()
{
    // The provider call was cancelled, then the Cancelled record could not be written: the PRIMARY failure and the terminal cause.
    auto cancelled = record(St::Cancelled, std::nullopt, 0, 0, 0);
    cancelled.errorCode = "project_memory.cancelled";
    cancelled.errorMessage = "The compaction was cancelled";
    const auto map = projectMemoryOutcomeMap("p", failedCall("persistence.transaction_failed", "the write failed", A::NotApplicable, false, cancelled));
    require(!flag(map, "resultSuccess") && str(map, "operationState") == "failed", "the call failed");
    require(str(map, "primaryCode") == "persistence.transaction_failed", "primary is the storage failure");
    require(str(map, "observedRecordStatus") == "cancelled" && str(map, "recordStatus").isEmpty(), "the attempted record was a Cancelled one");
    require(str(map, "terminalCode") == "project_memory.cancelled" && str(map, "terminalMessage") == "The compaction was cancelled",
        "the terminal cause is its own field");
    require(str(map, "terminalCode") != str(map, "primaryCode"), "neither replaces the other");
}

// ---- the observed cost: the stored record's own price state, only with a response ----------------------------------------------------

void testTheObservedCostUsesTheRecordsOwnPriceStateAndNeverInventsZero()
{
    struct Row { const char* name; St status; std::optional<bool> known; std::uint64_t cost; std::uint64_t in; const char* state; const char* amount; bool incomplete; };
    const Row rows[] = {
        {"known", St::Completed, true, 250'700, 1'000, "known", "$0.2507", false},
        {"known zero", St::Completed, true, 0, 40, "known", "$0.0000", false},
        {"partial", St::Failed, false, 250'700, 1'000, "partial", "$0.2507", true},
        {"unknown (false, zero)", St::Failed, false, 0, 40, "unknown", "", true},
        {"unknown (legacy, tokens)", St::Completed, std::nullopt, 0, 100, "unknown", "", true},
        {"none (nothing recorded)", St::Cancelled, std::nullopt, 0, 0, "none", "", false},
        {"legacy amount is partial", St::Cancelled, std::nullopt, 33'000, 0, "partial", "$0.0330", true},
    };
    for (const auto& row : rows) {
        const auto stored = record(row.status, row.known, row.cost, row.in, row.in == 0 ? 0 : 15);
        const auto map = projectMemoryOutcomeMap("p", failedCall("persistence.transaction_failed", "disk full", A::NotSaved, true, stored));
        const QVariantMap cost = sub(map, "observedCost");
        require(flag(cost, "available"), std::string(row.name) + ": a response was observed and a record exists");
        require(str(cost, "priceState") == row.state, std::string(row.name) + ": price state");
        require(str(cost, "costText") == row.amount, std::string(row.name) + ": amount text");
        require(flag(cost, "priceIncomplete") == row.incomplete, std::string(row.name) + ": incompleteness");
        require(cost.value("costMicros").toULongLong() == row.cost, std::string(row.name) + ": micros are the record's");
        const QString line = str(map, "costLine");
        require(line.endsWith("not saved"), std::string(row.name) + ": the amount is always next to 'not saved': " + line.toStdString());
        if (std::string(row.state) == "unknown" || std::string(row.state) == "none")
            require(!line.contains("$0.0000") && !line.contains("$"), std::string(row.name) + ": no exact zero or amount for an unknown cost");
        if (std::string(row.state) == "partial")
            require(line.contains(row.amount) && line.contains("Incomplete estimate") && line.contains("total unknown"),
                std::string(row.name) + ": partial keeps the amount and the incompleteness");
        if (std::string(row.name) == "known zero")
            require(line.contains("$0.0000") && line.contains("not proof that nothing was billed"), "a reported zero is not proof of zero billing");
    }

    // Tokens are the record's.
    const auto tokens = sub(projectMemoryOutcomeMap("p", succeeded(record(St::Completed, true, 1, 1000, 200), A::Saved)), "observedTokens");
    require(flag(tokens, "available") && tokens.value("inputTokens").toULongLong() == 1000 && tokens.value("outputTokens").toULongLong() == 200, "tokens");
}

void testTheSaveStateAccompaniesTheAmountForEveryVerdict()
{
    const auto stored = record(St::Completed, true, 120'000);
    require(str(projectMemoryOutcomeMap("p", succeeded(stored, A::Saved)), "costLine") == "Estimated cost $0.1200 — saved", "saved");
    require(str(projectMemoryOutcomeMap("p", failedCall("e", "m", A::NotSaved, true, stored)), "costLine") == "Estimated cost $0.1200 — not saved", "not saved");
    require(str(projectMemoryOutcomeMap("p", failedCall("e", "m", A::Unknown, true, stored)), "costLine")
        == "Estimated cost $0.1200 — saving not confirmed", "saving not confirmed");
}

void testNoResponseOrNoRecordMeansNoAmount()
{
    // A record exists, but no response was observed: there is no received usage to show.
    const auto noResponse = projectMemoryOutcomeMap("p", succeeded(record(St::Failed, false, 5'000), A::NotApplicable, false));
    require(!flag(sub(noResponse, "observedCost"), "available") && !flag(sub(noResponse, "observedTokens"), "available"), "no response, no usage");
    require(str(noResponse, "costLine").isEmpty() && str(sub(noResponse, "observedCost"), "costText").isEmpty(), "no amount without a response");
    // A response was observed but no record exists (a failure before an attempt): nothing is invented.
    const auto noRecord = projectMemoryOutcomeMap("p", failedCall("e", "m", A::Unknown, true, std::nullopt));
    require(!flag(sub(noRecord, "observedCost"), "available") && str(noRecord, "costLine").isEmpty(), "no record, no amount");
    require(str(sub(noRecord, "observedCost"), "priceState") == "none" && sub(noRecord, "observedCost").value("costMicros").toULongLong() == 0, "neutral");
}

// ---- the connection: an unobserved connection is neither healthy nor unusable ------------------------------------------------------

void testTheConnectionIsNeverReadAsHealthyWhenItWasNotObserved()
{
    auto outcome = failedCall("e", "m", A::Unknown, true, record(St::Completed, true, 1));
    outcome.connection.reset();
    const auto unobserved = projectMemoryOutcomeMap("p", outcome);
    require(str(unobserved, "connectionState") == "unobserved", "unobserved");
    require(unobserved.value("connectionUsable").isNull() && !unobserved.value("connectionUsable").toBool(), "usable is null, not true and not false");
    require(!flag(unobserved, "connectionNeedsAttention") && !str(unobserved, "connectionText").toLower().contains("healthy"), "unobserved is not healthy");

    outcome.connection = C::Healthy;
    const auto healthy = projectMemoryOutcomeMap("p", outcome);
    require(str(healthy, "connectionState") == "healthy" && healthy.value("connectionUsable").isValid() && healthy.value("connectionUsable").toBool()
        && !healthy.value("connectionUsable").isNull() && !flag(healthy, "connectionNeedsAttention"), "healthy");

    outcome.connection = C::Quarantined;
    const auto quarantined = projectMemoryOutcomeMap("p", outcome);
    require(str(quarantined, "connectionState") == "quarantined" && !quarantined.value("connectionUsable").isNull()
        && !quarantined.value("connectionUsable").toBool() && flag(quarantined, "connectionNeedsAttention"), "quarantined");
    require(str(quarantined, "connectionText").contains("isolated"), "quarantined words");

    outcome.connection = C::IsolationFailed;
    const auto failed = projectMemoryOutcomeMap("p", outcome);
    require(str(failed, "connectionState") == "isolation_failed" && !failed.value("connectionUsable").toBool()
        && flag(failed, "connectionNeedsAttention"), "isolation failed");
    require(str(failed, "connectionText").contains("could not be isolated"), "isolation failed words");
    require(str(failed, "connectionState") != str(quarantined, "connectionState"), "the two bad states are distinct");
    for (const auto* text : {"retry", "recover", "restore", "repair"})
        require(!str(quarantined, "connectionText").toLower().contains(QString::fromLatin1(text))
            && !str(failed, "connectionText").toLower().contains(QString::fromLatin1(text)), "no recovery is promised");
}

// ---- native codes and diagnostics ---------------------------------------------------------------------------------------------------

void testNativeCodesAreOptionalAndNeverMerged()
{
    auto outcome = failedCall("e", "m", A::NotSaved, true, record(St::Completed, true, 1));
    const auto none = projectMemoryOutcomeMap("p", outcome);
    for (const char* key : {"primaryNative", "fallbackNative"}) {
        require(!flag(sub(none, key), "present") && sub(none, key).value("code").toInt() == 0 && sub(none, key).value("extendedCode").toInt() == 0
            && str(sub(none, key), "text").isEmpty(), "an absent native detail is present=false, not a zero");
    }

    outcome.primaryNative = CompactionNativeDetail {19, 787, "FOREIGN KEY constraint failed"};
    const auto onlyPrimary = projectMemoryOutcomeMap("p", outcome);
    require(flag(sub(onlyPrimary, "primaryNative"), "present") && sub(onlyPrimary, "primaryNative").value("extendedCode").toInt() == 787
        && sub(onlyPrimary, "primaryNative").value("code").toInt() == 19 && str(sub(onlyPrimary, "primaryNative"), "message") == "FOREIGN KEY constraint failed",
        "primary native");
    require(!flag(sub(onlyPrimary, "fallbackNative"), "present"), "a primary native detail is not copied to the fallback");

    outcome.fallbackNative = CompactionNativeDetail {19, 1555, "UNIQUE constraint failed: id"};
    const auto both = projectMemoryOutcomeMap("p", outcome);
    require(sub(both, "primaryNative").value("extendedCode").toInt() == 787 && sub(both, "fallbackNative").value("extendedCode").toInt() == 1555,
        "the two native details stay apart");
    require(str(sub(both, "fallbackNative"), "text").contains("1555") && str(sub(both, "primaryNative"), "text").contains("787"), "their texts name their own codes");
}

void testDiagnosticsKeepTheOrderTheStagesAndEveryOriginalCause()
{
    auto outcome = failedCall("persistence.transaction_failed", "same", A::Unknown, true, record(St::Completed, true, 1));
    outcome.diagnostics = {
        {Stage::PrimaryWrite, {"persistence.begin_failed", "same"}},
        {Stage::Rollback, {"persistence.rollback_failed", "same"}},
        {Stage::ProofRead, {"persistence.read_failed", "same"}},
        {Stage::FallbackWrite, {"persistence.transaction_failed", "same"}},
        {Stage::FallbackRollback, {"persistence.rollback_failed", "same"}},
        {Stage::FallbackProofRead, {"persistence.read_failed", "same"}},
        {Stage::IdentityCheck, {"persistence.read_failed", "same"}},
        {Stage::IdentityCheck, {"project_memory.identity_check_failed", "summary"}}};
    outcome.diagnosticsIncomplete = true;
    const auto map = projectMemoryOutcomeMap("p", outcome);
    const QVariantList list = map.value("diagnostics").toList();
    require(list.size() == 8, "every cause is kept, even with equal code and message");
    const QStringList stages {"primary_write", "rollback", "proof_read", "fallback_write", "fallback_rollback", "fallback_proof_read",
        "identity_check", "identity_check"};
    const QStringList texts {"Write", "Rollback", "Verification read", "Fallback write", "Fallback rollback", "Fallback verification read",
        "Identity check", "Identity check"};
    for (int index = 0; index < list.size(); ++index) {
        require(str(list[index].toMap(), "stage") == stages[index], "the stage is the application's, in order");
        require(str(list[index].toMap(), "stageText") == texts[index], "the stage text is not guessed from the message");
    }
    require(str(list[1].toMap(), "code") == "persistence.rollback_failed" && str(list[7].toMap(), "message") == "summary", "code and message are kept");
    require(str(map, "primaryCode") == "persistence.transaction_failed", "the primary error is never replaced by a cause");
    require(flag(map, "diagnosticsIncomplete") && !str(map, "diagnosticsIncompleteText").isEmpty(), "incomplete diagnostics are visible");

    outcome.diagnosticsIncomplete = false;
    const auto complete = projectMemoryOutcomeMap("p", outcome);
    require(!flag(complete, "diagnosticsIncomplete") && str(complete, "diagnosticsIncompleteText").isEmpty(), "a complete list says nothing");
}

// R1 of the review (conservative form): the attempted record is only a CANDIDATE. It counts as stored ONLY when it was returned or the
// accounting says Saved - never because of a diagnostic or a native detail, which can be lost. Otherwise the main screen says only what the
// attempt did and that its record is not confirmed saved; the original code and text stay whole in the details, labelled.
void testAnUnconfirmedCandidateIsNeverWordedAsRecorded()
{
    namespace policy = ai::application::compaction_accounting;
    const ai::common::Error primary {"persistence.transaction_failed", "the commit failed"};
    // The REAL fallback row the application builds (same text as production), for a Completed attempt that did not commit.
    const auto completed = record(St::Completed, true, 120'000);
    const auto fallbackRow = policy::makeFailedAccountingRecord(completed, primary);
    require(fallbackRow.status == St::Failed && fallbackRow.errorMessage == policy::kFallbackErrorMessage
        && QString::fromStdString(fallbackRow.errorMessage).contains("kept as a failed attempt"), "the fixture is the production fallback row");

    const auto claims = [&](const QVariantMap& map, const std::string& what) {
        // no text of the MAIN screen may claim that the unconfirmed usage is kept / stored / recorded
        for (const char* key : {"headline", "primaryMessage", "accountingText", "costLine", "terminalText"}) {
            const QString shown = str(map, key).toLower();
            require(!shown.contains("kept") && !shown.contains("recorded reason") && !shown.contains("is saved") && !shown.contains("failed attempt")
                    && !shown.contains("could not be stored"),
                what + ": the main text '" + key + "' claims a record: " + shown.toStdString());
        }
    };

    // The marks the application may or may not have left: the fallback's own cause and native detail (the old gating), eight early causes
    // that pushed them out with diagnosticsIncomplete, or no cause at all. NONE of them may change what the main screen says.
    enum class Marks { Full, FullWithNative, EightOtherCausesIncomplete, NoCausesIncomplete, NoCausesComplete };
    for (const Marks marks : {Marks::Full, Marks::FullWithNative, Marks::EightOtherCausesIncomplete, Marks::NoCausesIncomplete, Marks::NoCausesComplete}) {
        for (const A verdict : {A::NotSaved, A::Unknown}) {
            auto outcome = failedCall(primary.code.c_str(), primary.message.c_str(), verdict, true, fallbackRow);
            outcome.connection = C::Healthy;
            switch (marks) {
            case Marks::Full: outcome.diagnostics = {{Stage::FallbackWrite, {"persistence.write_failed", "the fallback insert failed"}}}; break;
            case Marks::FullWithNative:
                outcome.fallbackNative = CompactionNativeDetail {19, 1555, "UNIQUE"};
                outcome.diagnostics = {{Stage::FallbackWrite, {"persistence.write_failed", "the fallback insert failed"}}};
                break;
            case Marks::EightOtherCausesIncomplete:
                for (int index = 0; index < 8; ++index) outcome.diagnostics.push_back({Stage::Rollback, {"persistence.rollback_failed", "early cause"}});
                outcome.diagnosticsIncomplete = true;
                break;
            case Marks::NoCausesIncomplete: outcome.diagnosticsIncomplete = true; break;
            case Marks::NoCausesComplete: break;
            }
            const auto map = projectMemoryOutcomeMap("p", outcome);
            const std::string what = std::string(verdict == A::NotSaved ? "NotSaved" : "Unknown") + " marks#" + std::to_string(static_cast<int>(marks));
            require(str(map, "observedRecordStatus") == "failed" && str(map, "terminalMessage") == QString::fromStdString(policy::kFallbackErrorMessage),
                what + ": the candidate really is the Failed fallback row with its fixed words");
            require(str(map, "terminalOrigin") == "attempted", what + ": an unconfirmed candidate is 'attempted' whatever marks survived");
            require(str(map, "terminalText") == "The compaction attempt failed; its record is not confirmed saved.", what + ": a neutral status, no raw text");
            require(str(map, "terminalDetailText").contains("not confirmed saved") && str(map, "terminalDetailText").contains("kept as a failed attempt")
                    && str(map, "terminalDetailText").contains(QString::fromStdString(fallbackRow.errorCode)),
                what + ": the ORIGINAL text and code survive in the details, labelled");
            claims(map, what);
            require(str(map, "primaryCode") == "persistence.transaction_failed" && str(map, "primaryMessage") == "the commit failed", what + ": primary kept");
            require(str(map, "accounting") == (verdict == A::NotSaved ? "not_saved" : "unknown"), what + ": the verdict is the application's");
            require(str(map, "costLine").endsWith(verdict == A::NotSaved ? "not saved" : "saving not confirmed") && str(map, "costLine").contains("$0.1200"),
                what + ": the amount stays, qualified");
            require(static_cast<std::size_t>(map.value("diagnostics").toList().size()) == outcome.diagnostics.size(), what + ": diagnostics kept as given");
            require(flag(map, "diagnosticsIncomplete") == outcome.diagnosticsIncomplete, what + ": the incomplete mark is shown as given");
            if (verdict == A::Unknown) {
                const QString words = str(map, "accountingText").toLower();
                require(words.contains("not confirmed") && !words.contains("not saved") && !words.contains("no record"), what + ": unknown makes no claim either way");
            }
        }
    }

    // a failed TERMINAL insert: the cause of the original Cancelled / Failed record is not lost (details), and it is not called recorded
    {
        auto cancelled = record(St::Cancelled, std::nullopt, 0, 0, 0);
        cancelled.errorCode = "project_memory.cancelled";
        cancelled.errorMessage = "The compaction was cancelled";
        const auto map = projectMemoryOutcomeMap("p", failedCall("persistence.transaction_failed", "the write failed", A::NotApplicable, false, cancelled));
        require(str(map, "terminalOrigin") == "attempted", "a failed terminal insert: attempted");
        require(str(map, "terminalText") == "The compaction attempt was cancelled; its record is not confirmed saved.", "cancelled: neutral status on the main screen");
        require(str(map, "terminalDetailText").contains("The compaction was cancelled") && str(map, "terminalDetailText").contains("project_memory.cancelled")
            && str(map, "terminalDetailText").contains("not confirmed saved"), "its original text and code are kept in the details");
        claims(map, "terminal insert cancelled");

        auto emptySummary = record(St::Failed, false, 5'000);   // a provider answer that was empty: it carries usage, no fallback was involved
        emptySummary.errorCode = "project_memory.empty_summary";
        emptySummary.errorMessage = "Provider returned an empty compacted summary";
        const auto failedMap = projectMemoryOutcomeMap("p", failedCall("persistence.transaction_failed", "the write failed", A::NotSaved, true, emptySummary));
        require(str(failedMap, "terminalOrigin") == "attempted" && str(failedMap, "terminalText") == "The compaction attempt failed; its record is not confirmed saved."
            && str(failedMap, "terminalDetailText").contains("Provider returned an empty compacted summary")
            && str(failedMap, "terminalDetailText").contains("project_memory.empty_summary"), "an original Failed record whose insert failed: neutral main text, whole details");
        require(str(failedMap, "costLine").endsWith("not saved"), "its amount is qualified");
    }

    // controls: what IS confirmed stored keeps its exact meaning
    {
        // Saved fallback after a primary failure: the row is durable, so its words are true and are shown as recorded - with or without marks.
        for (const bool withMarks : {true, false}) {
            auto outcome = failedCall(primary.code.c_str(), primary.message.c_str(), A::Saved, true, fallbackRow);
            outcome.connection = C::Healthy;
            if (withMarks) outcome.diagnostics = {{Stage::FallbackWrite, {"persistence.write_failed", "x"}}};
            const auto map = projectMemoryOutcomeMap("p", outcome);
            require(str(map, "terminalOrigin") == "stored" && str(map, "terminalText").startsWith("Recorded reason:")
                && str(map, "terminalText").contains("kept as a failed attempt"), "a Saved fallback is a recorded reason");
            require(str(map, "terminalDetailText").contains("Recorded reason code"), "its code is labelled recorded");
        }
        // returned Failed / Cancelled records
        const auto returnedFailed = projectMemoryOutcomeMap("p", succeeded(record(St::Failed, true, 120'000), A::Saved));
        require(str(returnedFailed, "terminalOrigin") == "stored" && str(returnedFailed, "terminalText") == "Recorded reason: The provider failed", "returned Failed");
        const auto returnedCancelled = projectMemoryOutcomeMap("p", succeeded(record(St::Cancelled, true, 4'000, 40, 15), A::Saved));
        require(str(returnedCancelled, "terminalOrigin") == "stored" && str(returnedCancelled, "terminalText").startsWith("Recorded reason:"), "returned Cancelled");
        // no reason at all
        const auto done = projectMemoryOutcomeMap("p", succeeded(record(St::Completed, true, 1), A::Saved));
        require(str(done, "terminalOrigin").isEmpty() && str(done, "terminalText").isEmpty() && str(done, "terminalDetailText").isEmpty(), "Completed: no reason");
    }
}

void testTheMapNeverCarriesPromptsOrProviderPayloads()
{
    const auto map = projectMemoryOutcomeMap("p", succeeded(record(St::Completed, true, 1), A::Saved));
    const QSet<QString> expected {"projectId", "resultSuccess", "operationState", "headline", "primaryCode", "primaryMessage", "recordStatus",
        "observedRecordStatus", "terminalCode", "terminalMessage", "responseObserved", "accounting", "accountingCode", "accountingMessage",
        "accountingText", "terminalOrigin", "terminalText", "terminalDetailText", "observedCost", "observedTokens", "costLine", "connectionState", "connectionUsable", "connectionNeedsAttention",
        "connectionText", "primaryNative", "fallbackNative", "diagnostics", "diagnosticsIncomplete", "diagnosticsIncompleteText"};
    QSet<QString> actual;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) actual.insert(it.key());
    require(actual == expected, "the key set of the map is exactly the documented one: " + actual.values().join(",").toStdString());
}

void testTheAmountTextIsTheHistorysFourDecimals()
{
    require(compactionAmountText(250'700) == "$0.2507" && compactionAmountText(0) == "$0.0000" && compactionAmountText(120'000) == "$0.1200"
        && compactionAmountText(1'234'567'890) == "$1234.5679", "four decimals");
}

} // namespace

int runProjectMemoryOutcomeProjectionTests()
{
    struct Case {
        const char* name;
        void (*run)();
    };
    const Case cases[] = {
        {"testTheOperationStateIsNeverTakenFromResultSuccessAlone", testTheOperationStateIsNeverTakenFromResultSuccessAlone},
        {"testTheProjectIsTheOneTheCallWasAcceptedForNotTheRecordsOne", testTheProjectIsTheOneTheCallWasAcceptedForNotTheRecordsOne},
        {"testEveryAccountingVerdictKeepsItsOwnWordsAndCodes", testEveryAccountingVerdictKeepsItsOwnWordsAndCodes},
        {"testACancelledCallWhoseRecordCouldNotBeStoredShowsBothCauses", testACancelledCallWhoseRecordCouldNotBeStoredShowsBothCauses},
        {"testTheObservedCostUsesTheRecordsOwnPriceStateAndNeverInventsZero", testTheObservedCostUsesTheRecordsOwnPriceStateAndNeverInventsZero},
        {"testTheSaveStateAccompaniesTheAmountForEveryVerdict", testTheSaveStateAccompaniesTheAmountForEveryVerdict},
        {"testNoResponseOrNoRecordMeansNoAmount", testNoResponseOrNoRecordMeansNoAmount},
        {"testTheConnectionIsNeverReadAsHealthyWhenItWasNotObserved", testTheConnectionIsNeverReadAsHealthyWhenItWasNotObserved},
        {"testNativeCodesAreOptionalAndNeverMerged", testNativeCodesAreOptionalAndNeverMerged},
        {"testDiagnosticsKeepTheOrderTheStagesAndEveryOriginalCause", testDiagnosticsKeepTheOrderTheStagesAndEveryOriginalCause},
        {"testAnUnconfirmedCandidateIsNeverWordedAsRecorded", testAnUnconfirmedCandidateIsNeverWordedAsRecorded},
        {"testTheMapNeverCarriesPromptsOrProviderPayloads", testTheMapNeverCarriesPromptsOrProviderPayloads},
        {"testTheAmountTextIsTheHistorysFourDecimals", testTheAmountTextIsTheHistorysFourDecimals},
    };
    int failures = 0;
    for (const auto& item : cases) {
        try {
            item.run();
        } catch (const std::exception& exception) {
            std::cerr << "FAILED " << item.name << ": " << exception.what() << std::endl;
            ++failures;
        }
    }
    if (failures == 0) std::cerr << "[project-memory-outcome-projection] " << std::size(cases) << " cases passed" << std::endl;
    return failures == 0 ? 0 : 1;
}
