// B3-II-A: the compaction SERVICE against a scripted RECORD repository, a scripted INTENT port and a fake provider (no network, no
// database; every scripted verdict is labelled as scripted - what real atomicity does is proven on real SQLite in
// CompactionIntentDispatchSqliteTests.cpp). Each test states what the storage said and checks what the service did with it: how many
// provider calls, how many intent writes and proofs, which record, which accounting verdict, and that the S2a/S2b guarantees (the
// primary cause, the usage, the accounting, the native codes) are exactly what they were without an intent. throwing require().
#include "CompactionIntentTestSupport.hpp"

#include "ai/application/conversations/ProjectMemoryCompactionAccounting.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionIntentPolicy.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionIntentTracker.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionService.hpp"
#include "ai/application/inmemory/InMemoryRepositories.hpp"

#include <chrono>
#include <deque>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace {

using namespace ai::application;
using ai::application::test_support::ScriptedCompactionIntentRepository;
namespace policy = ai::application::compaction_accounting;
namespace common = ai::common;
using C = CompactionConnectionState;
using S = CompactionPersistState;
using A = CompactionAccounting;
using O = CompactionIntentObservation;
using Src = CompactionIntentObservationSource;
using Stage = CompactionDiagnosticStage;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

// ---- scaffolding (local to this file) ------------------------------------------------------------------------------------------------
class ScriptedMemory final : public IProjectMemoryRepository {
public:
    std::vector<ProjectMemorySegment> segments;
    int writes {0};

    common::Result<ProjectMemorySegment> insertMemorySegment(ProjectMemorySegment value) override
    {
        ++writes;
        return common::Result<ProjectMemorySegment>::ok(std::move(value));
    }
    [[nodiscard]] common::Result<std::vector<ProjectMemorySegment>> listMemorySegments(const ProjectId&) const override
    {
        return common::Result<std::vector<ProjectMemorySegment>>::ok(segments);
    }
    [[nodiscard]] common::Result<std::optional<ProjectMemorySegment>> findMemorySegment(const ProjectMemorySegmentId&) const override
    {
        return common::Result<std::optional<ProjectMemorySegment>>::ok(std::nullopt);
    }
    common::Result<void> removeMemorySegments(const ProjectId&) override
    {
        ++writes;
        return common::Result<void>::ok();
    }
};

class ScriptedRepository final : public IProjectMemoryCompactionRepository {
public:
    std::deque<CompactionPersistOutcome> commitScript;
    std::deque<CompactionPersistOutcome> insertScript;
    std::function<common::Result<std::optional<ProjectMemoryCompactionRecord>>(const ProjectMemoryCompactionRecord&)> lookupFrom;

    std::vector<ProjectMemoryCompactionRecord> commits;
    std::vector<ProjectMemoryCompactionRecord> inserts;
    std::vector<ProjectMemoryCompactionId> lookups;
    int unscripted {0};

    common::Result<void> insertCompactionRecord(const ProjectMemoryCompactionRecord&) override { return common::Result<void>::ok(); }
    common::Result<void> commitSuccessfulCompaction(const ProjectMemoryCompactionRecord&, ProjectMemorySegment) override
    {
        return common::Result<void>::ok();
    }
    [[nodiscard]] common::Result<std::vector<ProjectMemoryCompactionRecord>> listCompactionRecords(const ProjectId&) const override
    {
        return common::Result<std::vector<ProjectMemoryCompactionRecord>>::ok({});
    }
    CompactionPersistOutcome commitSuccessfulCompactionObserved(const ProjectMemoryCompactionRecord& record, ProjectMemorySegment) override
    {
        commits.push_back(record);
        return next(commitScript);
    }
    CompactionPersistOutcome insertCompactionRecordObserved(const ProjectMemoryCompactionRecord& record) override
    {
        inserts.push_back(record);
        return next(insertScript);
    }
    [[nodiscard]] common::Result<std::optional<ProjectMemoryCompactionRecord>> findCompactionRecord(const ProjectMemoryCompactionId& id) const override
    {
        const_cast<ScriptedRepository*>(this)->lookups.push_back(id);
        if (lookupFrom && !inserts.empty()) return lookupFrom(inserts.back());
        return common::Result<std::optional<ProjectMemoryCompactionRecord>>::ok(std::nullopt);
    }

private:
    CompactionPersistOutcome next(std::deque<CompactionPersistOutcome>& script)
    {
        if (script.empty()) {
            ++unscripted;
            CompactionPersistOutcome outcome;
            outcome.result = common::Result<void>::failure({"test.unscripted_write", "no outcome was scripted for this write"});
            return outcome;
        }
        CompactionPersistOutcome outcome = std::move(script.front());
        script.pop_front();
        return outcome;
    }
};

class ScriptedClient final : public IProviderClient {
public:
    int sendCalls {0};
    int observedCalls {0};
    std::string content {"CONSOLIDATED"};
    ai::models::TokenUsage usage = [] {
        ai::models::TokenUsage initial;
        initial.inputTokens = 40;
        initial.outputTokens = 15;
        return initial;
    }();
    std::optional<common::Error> failure;
    std::stop_source* stopSource {nullptr};
    std::function<void()> onSend;

    common::Result<ProviderChatResponse> send(const ProviderChatRequest&, std::stop_token) override
    {
        ++sendCalls;
        return common::Result<ProviderChatResponse>::failure({"test.send_used", "the service must call sendObserved"});
    }
    ProviderChatOutcome sendObserved(const ProviderChatRequest&, std::stop_token) override
    {
        ++observedCalls;
        if (onSend) onSend();
        if (stopSource != nullptr) stopSource->request_stop();
        if (failure.has_value()) return {common::Result<ProviderChatResponse>::failure(*failure), std::nullopt};
        ProviderChatResponse response;
        response.content = content;
        response.usage = usage;
        return {common::Result<ProviderChatResponse>::ok(std::move(response)), std::nullopt};
    }
};

class ClientFactory final : public IProviderClientFactory {
public:
    explicit ClientFactory(IProviderClient& client) : client_(client) {}
    int resolutions {0};
    std::optional<common::Error> refusal;
    common::Result<std::shared_ptr<IProviderClient>> clientFor(const ProviderId&, const std::string&) override
    {
        ++resolutions;
        if (refusal.has_value()) return common::Result<std::shared_ptr<IProviderClient>>::failure(*refusal);
        return common::Result<std::shared_ptr<IProviderClient>>::ok(std::shared_ptr<IProviderClient>(&client_, [](IProviderClient*) {}));
    }

private:
    IProviderClient& client_;
};

struct Fixture {
    InMemoryProjectRepository projects;
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    ScriptedMemory memory;
    ScriptedRepository repository;
    ScriptedCompactionIntentRepository intents;
    ScriptedClient client;
    ClientFactory factory {client};
    ProjectMemoryCompactionService service {memory, repository, intents, projects, providers, models, factory};
    std::string projectId {"cp1"};
    std::string providerId {"prov1"};
    std::string modelId {"model1"};

    Fixture()
    {
        Project project;
        project.id = projectId;
        project.name = "Compaction";
        project.description = "d";
        project.rootPath = "C:/fixture";
        project.createdAt = std::chrono::system_clock::now();
        project.updatedAt = project.createdAt;
        require(projects.insert(project).success(), "setup: project");
        Provider provider;
        provider.id = providerId;
        provider.name = "Fake";
        provider.kind = "openai_compatible";
        provider.enabled = true;
        provider.createdAt = project.createdAt;
        provider.updatedAt = project.createdAt;
        require(providers.insert(provider).success(), "setup: provider");
        Model model;
        model.id = modelId;
        model.providerId = providerId;
        model.apiModelId = "fake-model-1";
        model.displayName = "Fake Model";
        model.contextWindowTokens = 128000;
        model.maxOutputTokens = 4096;
        model.inputPriceMicrosPerMillionTokens = 5'000'000;
        model.outputPriceMicrosPerMillionTokens = 15'000'000;
        model.enabled = true;
        model.createdAt = project.createdAt;
        model.updatedAt = project.createdAt;
        require(models.insert(model).success(), "setup: model");
        for (int i = 0; i < 3; ++i) {
            ProjectMemorySegment segment;
            segment.id = "seg" + std::to_string(i);
            segment.projectId = projectId;
            segment.runId = "run" + std::to_string(i);
            segment.summary = "Segment " + std::to_string(i);
            segment.sourceTokenCount = 50;
            segment.summaryTokenCount = 20;
            segment.createdAt = project.createdAt + std::chrono::milliseconds(i);
            memory.segments.push_back(segment);
        }
    }

    ProjectMemoryCompactionOutcome run(std::stop_token stop = {}) { return service.compactObserved(projectId, providerId, modelId, stop); }
};

// ---- scripted storage verdicts (SCRIPTED: not what a real database proved) ---------------------------------------------------------
CompactionPersistOutcome committed(std::optional<C> connection = C::Healthy)
{
    CompactionPersistOutcome outcome;
    outcome.state = S::Committed;
    outcome.connection = connection;
    return outcome;
}

CompactionNativeDetail native(int extended, const char* text)
{
    CompactionNativeDetail detail;
    detail.extendedCode = extended;
    detail.code = extended & 0xFF;
    detail.message = text;
    return detail;
}

CompactionPersistOutcome failed(S state, std::optional<C> connection, const char* code = "persistence.transaction_failed",
    std::optional<CompactionNativeDetail> nativeDetail = std::nullopt)
{
    CompactionPersistOutcome outcome;
    outcome.result = common::Result<void>::failure({code, std::string("primary text of ") + code});
    outcome.state = state;
    outcome.connection = connection;
    outcome.primaryNative = std::move(nativeDetail);
    return outcome;
}

CompactionIntentWriteOutcome intentRefused(S state, std::optional<C> connection, const char* code,
    std::optional<CompactionNativeDetail> nativeDetail = std::nullopt)
{
    CompactionIntentWriteOutcome outcome;
    outcome.persist = failed(state, connection, code, std::move(nativeDetail));
    return outcome;
}

CompactionDiagnostic cause(Stage stage, const char* code)
{
    return CompactionDiagnostic {stage, common::Error {code, std::string("text of ") + code}};
}

bool hasOriginalCause(const ProjectMemoryCompactionOutcome& outcome, Stage stage, const std::string& code, const std::string& message)
{
    for (const auto& entry : outcome.diagnostics) {
        if (entry.stage == stage && entry.error.code == code && entry.error.message == message) return true;
    }
    return false;
}

bool hasStage(const ProjectMemoryCompactionOutcome& outcome, Stage stage, const std::string& code)
{
    for (const auto& entry : outcome.diagnostics) {
        if (entry.stage == stage && entry.error.code == code) return true;
    }
    return false;
}

CompactionIntentIdentity identityOf(const ProjectMemoryCompactionRecord& record) { return compactionIntentIdentityOf(record); }

void requireNoRequestAndNoRecord(const Fixture& f, const std::string& what)
{
    require(f.client.observedCalls == 0 && f.client.sendCalls == 0, what + ": NO provider request");
    require(f.repository.commits.empty() && f.repository.inserts.empty() && f.repository.lookups.empty() && f.repository.unscripted == 0,
        what + ": no final record, no accounting fallback, no lookup");
    require(f.memory.writes == 0, what + ": the memory is untouched");
}

// ---- 1. admission happens BEFORE the request, on the identity of the final record -------------------------------------------------
void testTheIntentIsAdmittedBeforeTheRequestAndSharesTheIdentityOfTheFinalRecord()
{
    Fixture f;
    int intentWritesSeenBySend = -1;
    int proofsSeenBySend = -1;
    f.client.onSend = [&] {
        intentWritesSeenBySend = f.intents.recordCalls;
        proofsSeenBySend = f.intents.observeCalls;
    };
    f.repository.commitScript.push_back(committed());
    const auto outcome = f.run();
    require(outcome.result.success() && outcome.result.value().status == ProjectMemoryCompactionStatus::Completed, "Completed");
    require(intentWritesSeenBySend == 1 && proofsSeenBySend == 0, "the intent was written exactly once BEFORE the provider request was entered");
    require(f.client.observedCalls == 1 && f.client.sendCalls == 0 && f.factory.resolutions == 1, "ONE request, through sendObserved");
    require(f.intents.recorded.size() == 1, "one intent");
    const ProjectMemoryCompactionIntent& intent = f.intents.recorded[0];
    require(intent.state == CompactionIntentState::Open && !intent.unresolvedMarkedAt.has_value(), "an Open, unmarked intent");
    require(!compactionIntentIdentityDifference(intent, outcome.result.value()).has_value()
                && !compactionIntentIdentityDifference(intent, f.repository.commits[0]).has_value(),
        "the intent and the final record carry ONE identity (the same seven fields, no second id or clock reading)");
    require(intent.sourceSegmentIds == std::vector<std::string> {"seg0", "seg1", "seg2"}, "the ordered sources of the request");
    require(outcome.intent.admission == CompactionIntentAdmission::Admitted && outcome.intent.writeState == S::Committed
                && !outcome.intent.writeNative.has_value(),
        "Admitted, the write is Committed");
    require(outcome.intent.observation == O::FinalRecordPresent && outcome.intent.source == Src::ConfirmedFinalWrite
                && f.intents.observeCalls == 0 && f.intents.otherCalls == 0,
        "a confirmed Committed final write is the observation: NO proof read, and the service never finds/lists/marks/closes");
    require(outcome.accounting == A::Saved && outcome.responseObserved && !outcome.primaryNative.has_value(), "the accounting is what it was without an intent");
}

// ---- 2. an unconfirmed admission never reaches the provider ---------------------------------------------------------------------------
struct RefusalCase {
    const char* label;
    CompactionIntentWriteOutcome script;
    const char* expectedCode;
    const char* expectedMessage;
    std::optional<CompactionNativeDetail> expectedNative;
};

void testAnUnconfirmedAdmissionSendsNothingAndWritesNothingElse()
{
    CompactionIntentWriteOutcome identicalButNotNew = ScriptedCompactionIntentRepository::admitted();
    identicalButNotNew.newIntentInserted = false;   // a Committed result that is not the insert of THIS call
    CompactionIntentWriteOutcome occupied = intentRefused(S::NotCommitted, C::Healthy, kIntentDuplicateIdCode, native(1811, "AIO_COMPACTION_INTENT_ID_OCCUPIED"));
    const std::vector<RefusalCase> cases = {
        {"NotCommitted (scripted)", intentRefused(S::NotCommitted, C::Healthy, "persistence.write_failed", native(1555, "disk text")),
            kIntentNotRecordedCode, kIntentNotRecordedMessage, native(1555, "disk text")},
        {"Unknown (scripted: the row may be committed)", intentRefused(S::Unknown, C::Healthy, "persistence.commit_unknown", native(522, "io")),
            kIntentNotRecordedCode, kIntentNotRecordedMessage, native(522, "io")},
        {"Unknown (scripted: the row may be absent)", intentRefused(S::Unknown, std::nullopt, "persistence.transaction_failed"),
            kIntentNotRecordedCode, kIntentNotRecordedMessage, std::nullopt},
        {"Quarantined", intentRefused(S::NotCommitted, C::Quarantined, "persistence.connection_unusable"),
            kIntentNotRecordedCode, kIntentNotRecordedMessage, std::nullopt},
        {"IsolationFailed", intentRefused(S::NotCommitted, C::IsolationFailed, "persistence.connection_unusable"),
            kIntentNotRecordedCode, kIntentNotRecordedMessage, std::nullopt},
        {"duplicate intent id", intentRefused(S::NotCommitted, C::Healthy, kIntentDuplicateIdCode, native(1555, "UNIQUE")),
            kIntentDuplicateIdCode, kIntentDuplicateIdMessage, native(1555, "UNIQUE")},
        {"operation id already used by a final record", occupied, kIntentDuplicateIdCode, kIntentDuplicateIdMessage,
            native(1811, "AIO_COMPACTION_INTENT_ID_OCCUPIED")},
        {"project not found", intentRefused(S::NotCommitted, C::Healthy, "project_memory.project_not_found", native(787, "FK")),
            kIntentNotRecordedCode, kIntentNotRecordedMessage, native(787, "FK")},
        {"Committed but not a new row of this call", identicalButNotNew, kIntentNotRecordedCode, kIntentNotRecordedMessage, std::nullopt},
    };
    for (const RefusalCase& item : cases) {
        const std::string what = item.label;
        Fixture f;
        f.intents.writeScript.push_back(item.script);
        const auto outcome = f.run();
        requireNoRequestAndNoRecord(f, what);
        require(f.factory.resolutions == 1, what + ": the client WAS resolved (admission comes after it)");
        require(!outcome.result.success() && outcome.result.error()->code == item.expectedCode
                    && outcome.result.error()->message == item.expectedMessage,
            what + ": the typed refusal");
        require(f.intents.recordCalls == 1, what + ": the admission is written ONCE - nothing is retried");
        require(f.intents.otherCalls == 0, what + ": no delete / compensation of a possible row");
        require(!outcome.responseObserved && outcome.accounting == A::NotApplicable && !outcome.observedRecord.has_value(),
            what + ": no response, nothing to account for (NotApplicable, not NotSaved), no record attempted");
        require(!outcome.primaryNative.has_value() && !outcome.fallbackNative.has_value(),
            what + ": the intent's native detail does not stand in for a record write's");
        require(outcome.intent.admission == CompactionIntentAdmission::NotAdmitted && outcome.intent.writeState == item.script.persist.state,
            what + ": NotAdmitted and the fate of the write is kept");
        require(outcome.intent.writeNative.has_value() == item.expectedNative.has_value()
                    && (!item.expectedNative.has_value()
                        || (outcome.intent.writeNative->extendedCode == item.expectedNative->extendedCode
                            && outcome.intent.writeNative->message == item.expectedNative->message)),
            what + ": the native detail of the intent write is kept");
        if (!item.script.persist.result.success()) {
            require(hasOriginalCause(outcome, Stage::IntentWrite, item.script.persist.result.error()->code, item.script.persist.result.error()->message),
                what + ": the storage's own error is a separate IntentWrite cause");
        }
        require(outcome.connection == item.script.persist.connection, what + ": the observed health is kept");
        // The proof after a refusal: exactly one, and a row that is not seen is not a statement about delivery.
        require(f.intents.observeCalls == 1 && outcome.intent.observation == O::IntentPresent && outcome.intent.source == Src::AtomicPairRead,
            what + ": ONE pair proof; the scripted answer is carried as observed");
    }
}

void testACompactCallIsTheSameSingleRefusal()
{
    Fixture f;
    f.intents.writeScript.push_back(intentRefused(S::Unknown, C::Healthy, "persistence.commit_unknown"));
    const auto view = f.service.compact(f.projectId, f.providerId, f.modelId, {});
    require(!view.success() && view.error()->code == kIntentNotRecordedCode, "compact() is the .result of the ONE compactObserved");
    require(f.intents.recordCalls == 1 && f.client.observedCalls == 0, "one admission, zero requests");
}

// ---- 3. paths that end before a client exists leave the intent alone -------------------------------------------------------------------
void testAStopBeforeAnythingWasSentWritesNoIntentAndKeepsTheS2aRecord()
{
    Fixture f;
    f.repository.insertScript.push_back(committed());
    std::stop_source stop;
    stop.request_stop();
    const auto outcome = f.run(stop.get_token());
    require(outcome.result.success() && outcome.result.value().status == ProjectMemoryCompactionStatus::Cancelled
                && outcome.result.value().inputTokens == 0 && outcome.result.value().costMicros == 0 && !outcome.result.value().costPriceKnown.has_value(),
        "S2a: Cancelled with 0/0/NULL");
    require(f.factory.resolutions == 0 && f.client.observedCalls == 0, "no client resolved, no request");
    require(f.intents.recordCalls == 0 && f.intents.observeCalls == 0 && f.intents.otherCalls == 0, "no intent write, no proof");
    require(f.repository.inserts.size() == 1 && f.repository.commits.empty(), "ONE terminal record");
    require(outcome.intent.admission == CompactionIntentAdmission::NotApplicable && !outcome.intent.writeState.has_value()
                && outcome.intent.observation == O::NotObserved && outcome.intent.source == Src::None,
        "NotApplicable / no write state (that is not Unknown) / NotObserved: a commit of this record is not the closing of an intent");
    require(!outcome.responseObserved && outcome.accounting == A::NotApplicable, "nothing to account for");
}

void testAFailedClientResolutionWritesNoIntentAndKeepsThePreviousResult()
{
    Fixture f;
    f.factory.refusal = common::Error {"credential.missing", "No credential is configured for this provider"};
    f.repository.insertScript.push_back(committed());
    const auto outcome = f.run();
    require(outcome.result.success() && outcome.result.value().status == ProjectMemoryCompactionStatus::Failed
                && outcome.result.value().errorCode == "credential.missing",
        "the same Failed terminal record as before the intent existed");
    require(f.client.observedCalls == 0 && f.intents.recordCalls == 0 && f.intents.observeCalls == 0, "no request, no intent write, no proof");
    require(outcome.intent.admission == CompactionIntentAdmission::NotApplicable && outcome.intent.observation == O::NotObserved,
        "NotApplicable");
    require(!outcome.responseObserved && outcome.accounting == A::NotApplicable, "nothing to account for");
}

// ---- 4. a stop after the admission needs no request -------------------------------------------------------------------------------------
void testAStopAfterAConfirmedAdmissionSendsNothingAndEndsAsAnOrdinaryCancelledRecord()
{
    Fixture f;
    std::stop_source stop;
    f.intents.onRecord = [&](const ProjectMemoryCompactionIntent&, const CompactionIntentWriteOutcome&) { stop.request_stop(); };
    f.repository.insertScript.push_back(committed());
    const auto outcome = f.run(stop.get_token());
    require(f.client.observedCalls == 0 && f.client.sendCalls == 0, "no request after the stop");
    require(outcome.result.success() && outcome.result.value().status == ProjectMemoryCompactionStatus::Cancelled
                && outcome.result.value().errorCode == "provider.cancelled" && outcome.result.value().inputTokens == 0
                && outcome.result.value().costMicros == 0 && !outcome.result.value().costPriceKnown.has_value()
                && !outcome.result.value().resultSegmentId.has_value(),
        "an ordinary Cancelled terminal record with no usage");
    require(f.repository.inserts.size() == 1 && f.repository.commits.empty() && f.intents.recordCalls == 1, "ONE terminal insert after ONE admission");
    require(f.intents.recorded[0].id == outcome.result.value().id, "the terminal record has the id of the intent");
    require(!outcome.responseObserved && outcome.accounting == A::NotApplicable, "no response, nothing to account for");
    require(outcome.intent.admission == CompactionIntentAdmission::Admitted && outcome.intent.observation == O::FinalRecordPresent
                && outcome.intent.source == Src::ConfirmedFinalWrite && f.intents.observeCalls == 0 && f.intents.otherCalls == 0,
        "the confirmed terminal insert is the closing; no best-effort close, no proof read");
}

void testAStopAfterAdmissionWithAnUnstoredTerminalRecordKeepsTheStorageFailureAndProvesOnce()
{
    Fixture f;
    std::stop_source stop;
    f.intents.onRecord = [&](const ProjectMemoryCompactionIntent&, const CompactionIntentWriteOutcome&) { stop.request_stop(); };
    f.repository.insertScript.push_back(failed(S::NotCommitted, C::Healthy, "persistence.write_failed", native(13, "full")));
    const auto outcome = f.run(stop.get_token());
    require(f.client.observedCalls == 0 && !outcome.result.success() && outcome.result.error()->code == "persistence.write_failed",
        "the storage failure of the terminal insert is the result");
    require(outcome.accounting == A::NotApplicable && outcome.primaryNative.has_value() && outcome.primaryNative->extendedCode == 13,
        "no response: nothing to account for; the record write's native detail is its own");
    require(f.intents.observeCalls == 1 && outcome.intent.observation == O::IntentPresent && outcome.intent.source == Src::AtomicPairRead,
        "no confirmed final write: ONE pair proof");
}

void testAStopInsideTheClientKeepsTheS2aUsageAndStillConfirmsTheClosing()
{
    Fixture f;
    std::stop_source stop;
    f.client.stopSource = &stop;
    f.repository.insertScript.push_back(committed());
    const auto outcome = f.run(stop.get_token());
    require(outcome.result.success() && outcome.result.value().status == ProjectMemoryCompactionStatus::Cancelled
                && outcome.result.value().inputTokens == 40 && outcome.result.value().outputTokens == 15 && outcome.responseObserved
                && outcome.accounting == A::Saved,
        "S2a: the received usage is kept from ONE source, the status stays Cancelled");
    require(f.client.observedCalls == 1 && f.intents.recordCalls == 1 && f.intents.observeCalls == 0
                && outcome.intent.source == Src::ConfirmedFinalWrite,
        "one admission, one request, no proof");
}

// ---- 5. every confirmed final write is a confirmation; Saved alone is not -----------------------------------------------------------
void testEveryConfirmedFinalWriteNeedsNoProof()
{
    {   // terminal Failed
        Fixture f;
        f.client.failure = common::Error {"provider.failed", "boom"};
        f.repository.insertScript.push_back(committed());
        const auto outcome = f.run();
        require(outcome.result.success() && outcome.result.value().status == ProjectMemoryCompactionStatus::Failed, "terminal Failed");
        require(f.intents.observeCalls == 0 && outcome.intent.source == Src::ConfirmedFinalWrite && !outcome.responseObserved
                    && outcome.accounting == A::NotApplicable,
            "terminal Failed: confirmed by its own insert");
    }
    {   // accounting fallback
        Fixture f;
        f.repository.commitScript.push_back(failed(S::NotCommitted, C::Healthy, "persistence.write_failed"));
        f.repository.insertScript.push_back(committed());
        const auto outcome = f.run();
        require(!outcome.result.success() && outcome.result.error()->code == "persistence.write_failed" && outcome.accounting == A::Saved,
            "the primary failure stays the result; the fallback row is Saved");
        require(f.intents.observeCalls == 0 && outcome.intent.source == Src::ConfirmedFinalWrite && outcome.intent.observation == O::FinalRecordPresent,
            "the fallback is the LAST final insert and it is confirmed Committed: no proof read");
    }
}

void testASavedFoundByAnIdentityLookupIsNotAConfirmedFinalWrite()
{
    Fixture f;
    f.repository.commitScript.push_back(failed(S::NotCommitted, C::Healthy, "persistence.write_failed"));
    f.repository.insertScript.push_back(failed(S::NotCommitted, C::Healthy, "project_memory.duplicate_id"));
    f.repository.lookupFrom = [](const ProjectMemoryCompactionRecord& inserted) {
        return common::Result<std::optional<ProjectMemoryCompactionRecord>>::ok(inserted);   // an identical row already exists
    };
    const auto outcome = f.run();
    require(outcome.accounting == A::Saved, "setup: the verdict is Saved (an identical row was found), as in S2b");
    require(f.intents.observeCalls == 1 && outcome.intent.source != Src::ConfirmedFinalWrite,
        "...but nothing of THIS call was confirmed Committed: the intent is observed by ONE proof, never derived from accounting == Saved");
    require(f.repository.lookups.size() == 1, "and the service still uses exactly one strict lookup of the record");
}

// ---- 6. the observation is independent of the primary outcome ----------------------------------------------------------------------------
struct ProofCase {
    const char* label;
    CompactionIntentObservationOutcome proof;
    O expected;
    Src expectedSource;
};

CompactionIntentObservationOutcome proofOf(O observation, Src source, std::optional<common::Error> error = std::nullopt)
{
    CompactionIntentObservationOutcome proof;
    proof.observation = observation;
    proof.source = source;
    proof.error = std::move(error);
    return proof;
}

void testAFailedFinalWriteIsObservedByExactlyOneProofAndTheFirstCauseSurvives()
{
    CompactionIntentObservationOutcome unreadable = proofOf(O::Unreadable, Src::None, common::Error {"persistence.connection_unusable", "nothing was read"});
    unreadable.connection = C::Quarantined;
    unreadable.primaryNative = native(5, "locked");
    unreadable.diagnostics.push_back(cause(Stage::ProofRead, "persistence.read_failed"));
    unreadable.diagnosticsIncomplete = true;
    const std::vector<ProofCase> cases = {
        {"IntentPresent (scripted)", proofOf(O::IntentPresent, Src::AtomicPairRead), O::IntentPresent, Src::AtomicPairRead},
        {"FinalRecordPresent (scripted)", proofOf(O::FinalRecordPresent, Src::AtomicPairRead), O::FinalRecordPresent, Src::AtomicPairRead},
        {"NeitherPresent (scripted)", proofOf(O::NeitherPresent, Src::AtomicPairRead), O::NeitherPresent, Src::AtomicPairRead},
        {"Inconsistent (scripted)",
            proofOf(O::Inconsistent, Src::AtomicPairRead, common::Error {kIntentObservationInconsistentCode, "both rows exist"}),
            O::Inconsistent, Src::AtomicPairRead},
        {"Unreadable (scripted)", unreadable, O::Unreadable, Src::None},
    };
    for (const ProofCase& item : cases) {
        const std::string what = item.label;
        Fixture f;
        // The final write's own verdict stays Unknown: no proof of the INTENT changes what is known about the record.
        CompactionPersistOutcome primary = failed(S::Unknown, C::Healthy, "persistence.commit_unknown", native(522, "primary io"));
        primary.diagnostics.push_back(cause(Stage::ProofRead, "persistence.read_failed"));   // the record write's OWN proof failure
        f.repository.commitScript.push_back(primary);
        f.intents.proofScript.push_back(item.proof);
        const auto outcome = f.run();

        require(f.client.observedCalls == 1 && f.repository.commits.size() == 1 && f.repository.inserts.empty(), what + ": one request, one commit, no fallback");
        require(f.intents.observeCalls == 1 && f.intents.proofExpected.size() == 1, what + ": EXACTLY one pair proof");
        require(!compactionIntentIdentityDifference(f.intents.proofExpected[0], identityOf(f.repository.commits[0])).has_value(),
            what + ": the proof asks about the identity of THIS operation");
        require(!outcome.result.success() && outcome.result.error()->code == "persistence.commit_unknown"
                    && outcome.result.error()->message == "primary text of persistence.commit_unknown",
            what + ": the primary result is untouched by the proof");
        require(outcome.accounting == A::Unknown && outcome.responseObserved && outcome.observedRecord.has_value()
                    && outcome.observedRecord->status == ProjectMemoryCompactionStatus::Completed && outcome.observedRecord->inputTokens == 40,
            what + ": accounting, the received usage and the observed record are not rewritten by the observation");
        require(outcome.primaryNative.has_value() && outcome.primaryNative->extendedCode == 522, what + ": the record's native detail is its own");
        require(outcome.intent.admission == CompactionIntentAdmission::Admitted && outcome.intent.writeState == S::Committed,
            what + ": the observation does not rewrite the admission");
        require(outcome.intent.observation == item.expected && outcome.intent.source == item.expectedSource, what + ": the observation and its source");
        require(hasStage(outcome, Stage::ProofRead, "persistence.read_failed"), what + ": the RECORD write's proof cause keeps its own stage");
        if (item.expected == O::Unreadable) {
            require(hasOriginalCause(outcome, Stage::IntentProofRead, "persistence.connection_unusable", "nothing was read"),
                what + ": the original reason of an unreadable proof is a cause");
            require(hasStage(outcome, Stage::IntentProofRead, "persistence.read_failed"), what + ": the proof layer's generic ProofRead is renamed IntentProofRead");
            require(outcome.intent.proofNative.has_value() && outcome.intent.proofNative->extendedCode == 5, what + ": the proof's native detail is kept apart");
            require(outcome.connection == C::Quarantined, what + ": the last health actually observed is kept");
            require(outcome.diagnosticsIncomplete, what + ": the incomplete flag of the proof is kept");
        }
        if (item.expected == O::Inconsistent) {
            require(hasOriginalCause(outcome, Stage::IntentProofRead, kIntentObservationInconsistentCode, "both rows exist"),
                what + ": the contradiction is a cause");
        }
        require(f.intents.otherCalls == 0, what + ": no repair, no close");
    }
}

void testBothFinalWritesRefusedLeaveTheIntentAndNeverCallTheProviderAgain()
{
    Fixture f;
    f.repository.commitScript.push_back(failed(S::NotCommitted, C::Healthy, "persistence.write_failed", native(19, "primary constraint")));
    f.repository.insertScript.push_back(failed(S::NotCommitted, C::Healthy, "persistence.write_failed", native(19, "fallback constraint")));
    const auto outcome = f.run();
    require(f.client.observedCalls == 1, "ONE provider request, never a second");
    require(f.repository.commits.size() == 1 && f.repository.inserts.size() == 1, "one commit, one fallback");
    require(!outcome.result.success() && outcome.result.error()->code == "persistence.write_failed" && outcome.accounting == A::NotSaved,
        "the primary failure; the usage is PROVEN not stored (as in S2b)");
    require(outcome.primaryNative->message == "primary constraint" && outcome.fallbackNative->message == "fallback constraint",
        "both native details, each its own");
    require(f.intents.observeCalls == 1 && outcome.intent.observation == O::IntentPresent && outcome.intent.source == Src::AtomicPairRead,
        "the intent is still observed: ONE proof after BOTH attempts");
    require(f.intents.otherCalls == 0, "the intent is not closed by anything but the storage's own finalization");
}

void testAProofOfAnUnconfirmedUnknownFallbackIsAlsoOne()
{
    Fixture f;
    f.repository.commitScript.push_back(failed(S::NotCommitted, C::Healthy, "persistence.write_failed"));
    f.repository.insertScript.push_back(failed(S::Unknown, C::Healthy, "persistence.commit_unknown"));
    f.intents.proofScript.push_back(proofOf(O::FinalRecordPresent, Src::AtomicPairRead));
    const auto outcome = f.run();
    require(outcome.accounting == A::Unknown && outcome.intent.observation == O::FinalRecordPresent && f.intents.observeCalls == 1,
        "an Unknown fallback is not a confirmed write: ONE proof, and the accounting stays Unknown whatever the proof says");
}

// ---- 7. one tracker is one admission -----------------------------------------------------------------------------------------------------
void testATrackerNeverAdmitsTwice()
{
    ScriptedCompactionIntentRepository intents;
    ProjectMemoryCompactionRecord record;
    record.id = "op";
    record.projectId = "p";
    record.providerId = "pr";
    record.modelId = "m";
    record.sourceSegmentIds = {"a", "b"};
    record.sourceContentHash = "h";
    record.createdAt = std::chrono::system_clock::now();
    CompactionIntentTracker tracker(intents, record);
    auto first = tracker.admit();
    require(first.dispatch && !first.refusal.has_value() && intents.recordCalls == 1, "the first request is admitted");
    auto second = tracker.admit();
    require(!second.dispatch && second.refusal.has_value() && second.refusal->result.error()->code == kIntentNotRecordedCode
                && intents.recordCalls == 1,
        "a second admission writes nothing and permits nothing");
}

// ---- 8. the causes of the intent are bounded and in order -------------------------------------------------------------------------------
void testTheCausesOfARefusedAdmissionAreBoundedAndInOrder()
{
    Fixture f;
    CompactionIntentWriteOutcome script = intentRefused(S::Unknown, C::Healthy, "persistence.commit_unknown");
    for (int i = 0; i < 9; ++i) script.persist.diagnostics.push_back(cause(i == 0 ? Stage::ProofRead : Stage::Rollback, "persistence.rollback_failed"));
    f.intents.writeScript.push_back(script);
    const auto outcome = f.run();
    require(outcome.diagnostics.size() == kMaxCompactionDiagnostics && outcome.diagnosticsIncomplete, "bounded to 8 and the loss is flagged");
    require(outcome.diagnostics[0].stage == Stage::IntentWrite && outcome.diagnostics[0].error.code == "persistence.commit_unknown",
        "the storage's own error comes first, as an IntentWrite cause");
    require(outcome.diagnostics[1].stage == Stage::IntentProofRead && outcome.diagnostics[2].stage == Stage::IntentWrite,
        "the write's secondary causes follow in order, named for the intent (ProofRead -> IntentProofRead, Rollback -> IntentWrite)");
}

} // namespace

int runCompactionIntentDispatchServiceTests()
{
    try {
        testTheIntentIsAdmittedBeforeTheRequestAndSharesTheIdentityOfTheFinalRecord();
        testAnUnconfirmedAdmissionSendsNothingAndWritesNothingElse();
        testACompactCallIsTheSameSingleRefusal();
        testAStopBeforeAnythingWasSentWritesNoIntentAndKeepsTheS2aRecord();
        testAFailedClientResolutionWritesNoIntentAndKeepsThePreviousResult();
        testAStopAfterAConfirmedAdmissionSendsNothingAndEndsAsAnOrdinaryCancelledRecord();
        testAStopAfterAdmissionWithAnUnstoredTerminalRecordKeepsTheStorageFailureAndProvesOnce();
        testAStopInsideTheClientKeepsTheS2aUsageAndStillConfirmsTheClosing();
        testEveryConfirmedFinalWriteNeedsNoProof();
        testASavedFoundByAnIdentityLookupIsNotAConfirmedFinalWrite();
        testAFailedFinalWriteIsObservedByExactlyOneProofAndTheFirstCauseSurvives();
        testBothFinalWritesRefusedLeaveTheIntentAndNeverCallTheProviderAgain();
        testAProofOfAnUnconfirmedUnknownFallbackIsAlsoOne();
        testATrackerNeverAdmitsTwice();
        testTheCausesOfARefusedAdmissionAreBoundedAndInOrder();
    } catch (const std::exception& exception) {
        std::cerr << "CompactionIntentDispatchServiceTests failed: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "CompactionIntentDispatchServiceTests: OK\n";
    return 0;
}
