// C2-B/S2b-2: the compaction SERVICE against a scripted observed repository and a fake provider (no network, no database).
// The repository replays exactly the outcomes a test gives it and records every call, so each test states what the storage
// said and checks what the service did with it: how many provider calls, how many writes, which record, which accounting verdict,
// and that nothing the storage reported (health, native codes, causes) got lost. The checks use throwing require().
#include "CompactionIntentTestSupport.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionAccounting.hpp"
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
namespace policy = ai::application::compaction_accounting;
namespace common = ai::common;
using C = CompactionConnectionState;
using S = CompactionPersistState;
using A = CompactionAccounting;

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
    common::Result<std::optional<ProjectMemoryCompactionRecord>> lookup {
        common::Result<std::optional<ProjectMemoryCompactionRecord>>::ok(std::nullopt)};
    // When set, the answer of the lookup is computed from the LAST record the service inserted (the fallback it built itself).
    std::function<common::Result<std::optional<ProjectMemoryCompactionRecord>>(const ProjectMemoryCompactionRecord&)> lookupFrom;

    std::vector<ProjectMemoryCompactionRecord> commits;
    std::vector<ProjectMemorySegment> committedSegments;
    std::vector<ProjectMemoryCompactionRecord> inserts;
    std::vector<ProjectMemoryCompactionId> lookups;
    int listCalls {0};
    int legacyCalls {0};     // the service must use the OBSERVED port only
    int unscripted {0};

    common::Result<void> insertCompactionRecord(const ProjectMemoryCompactionRecord&) override
    {
        ++legacyCalls;
        return common::Result<void>::ok();
    }
    common::Result<void> commitSuccessfulCompaction(const ProjectMemoryCompactionRecord&, ProjectMemorySegment) override
    {
        ++legacyCalls;
        return common::Result<void>::ok();
    }
    [[nodiscard]] common::Result<std::vector<ProjectMemoryCompactionRecord>> listCompactionRecords(const ProjectId&) const override
    {
        ++const_cast<ScriptedRepository*>(this)->listCalls;
        return common::Result<std::vector<ProjectMemoryCompactionRecord>>::ok({});
    }
    CompactionPersistOutcome commitSuccessfulCompactionObserved(const ProjectMemoryCompactionRecord& record, ProjectMemorySegment segment) override
    {
        commits.push_back(record);
        committedSegments.push_back(std::move(segment));
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
        return lookup;
    }

private:
    CompactionPersistOutcome next(std::deque<CompactionPersistOutcome>& script)
    {
        if (script.empty()) {
            ++unscripted;   // a write nobody scripted: the test fails on it
            CompactionPersistOutcome outcome;
            outcome.result = common::Result<void>::failure({"test.unscripted_write", "no outcome was scripted for this write"});
            return outcome;
        }
        CompactionPersistOutcome outcome = std::move(script.front());
        script.pop_front();
        return outcome;
    }
};

// send() is a trap: the service must use sendObserved() exactly once.
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
    std::optional<ProviderChatResponse> discarded;
    std::stop_source* stopSource {nullptr};

    common::Result<ProviderChatResponse> send(const ProviderChatRequest&, std::stop_token) override
    {
        ++sendCalls;
        return common::Result<ProviderChatResponse>::failure({"test.send_used", "the service must call sendObserved"});
    }
    ProviderChatOutcome sendObserved(const ProviderChatRequest&, std::stop_token) override
    {
        ++observedCalls;
        if (stopSource != nullptr) stopSource->request_stop();
        if (failure.has_value()) return {common::Result<ProviderChatResponse>::failure(*failure), discarded};
        ProviderChatResponse response;
        response.content = content;
        response.usage = usage;
        return {common::Result<ProviderChatResponse>::ok(std::move(response)), discarded};
    }
};

class ClientFactory final : public IProviderClientFactory {
public:
    explicit ClientFactory(IProviderClient& client) : client_(client) {}
    int resolutions {0};
    common::Result<std::shared_ptr<IProviderClient>> clientFor(const ProviderId&, const std::string&) override
    {
        ++resolutions;
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
    ScriptedClient client;
    ClientFactory factory {client};
    test_support::ScriptedCompactionIntentRepository intents;
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

    ProjectMemoryCompactionOutcome run(std::stop_token stop = {})
    {
        return service.compactObserved(projectId, providerId, modelId, stop);
    }
};

// ---- scripted storage verdicts --------------------------------------------------------------------------------------------------------
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

CompactionDiagnostic cause(CompactionDiagnosticStage stage, const char* code)
{
    return CompactionDiagnostic {stage, common::Error {code, std::string("text of ") + code}};
}

bool hasDiagnostic(const ProjectMemoryCompactionOutcome& outcome, CompactionDiagnosticStage stage, const std::string& code)
{
    for (const auto& entry : outcome.diagnostics) {
        if (entry.stage == stage && entry.error.code == code) return true;
    }
    return false;
}

// A cause that carries exactly this ORIGINAL code and message (not a summary that mentions them).
bool hasOriginalCause(const ProjectMemoryCompactionOutcome& outcome, CompactionDiagnosticStage stage, const std::string& code, const std::string& message)
{
    for (const auto& entry : outcome.diagnostics) {
        if (entry.stage == stage && entry.error.code == code && entry.error.message == message) return true;
    }
    return false;
}

std::size_t indexOfDiagnostic(const ProjectMemoryCompactionOutcome& outcome, const std::string& code)
{
    for (std::size_t i = 0; i < outcome.diagnostics.size(); ++i) {
        if (outcome.diagnostics[i].error.code == code) return i;
    }
    return outcome.diagnostics.size();
}

// ---- 1. the normal paths, and compact() as a view ------------------------------------------------------------------------------------
void testACommittedCompactionIsSavedAndCarriesWhatTheStorageSaid()
{
    Fixture f;
    CompactionPersistOutcome stored = committed(C::Healthy);
    stored.diagnostics.push_back(cause(CompactionDiagnosticStage::Rollback, "persistence.rollback_failed"));   // an entry recovery note
    stored.diagnosticsIncomplete = true;
    f.repository.commitScript.push_back(stored);

    const auto outcome = f.run();
    require(outcome.result.success() && outcome.result.value().status == ProjectMemoryCompactionStatus::Completed
                && outcome.result.value().resultSegmentId.has_value(),
        "Completed: the record is returned");
    require(outcome.accounting == A::Saved && outcome.responseObserved, "Committed -> Saved, response observed");
    require(f.client.observedCalls == 1 && f.client.sendCalls == 0 && f.factory.resolutions == 1, "ONE provider call, through sendObserved");
    require(f.repository.commits.size() == 1 && f.repository.inserts.empty() && f.repository.lookups.empty() && f.repository.listCalls == 0
                && f.repository.legacyCalls == 0,
        "ONE commit, no fallback, no lookup, no history listing, only the observed port");
    require(outcome.observedRecord.has_value() && policy::identityDifference(*outcome.observedRecord, outcome.result.value()) == nullptr
                && policy::identityDifference(f.repository.commits[0], outcome.result.value()) == nullptr,
        "the observed record IS the committed one");
    require(outcome.connection == C::Healthy && outcome.diagnosticsIncomplete
                && hasDiagnostic(outcome, CompactionDiagnosticStage::Rollback, "persistence.rollback_failed"),
        "the notes of a SUCCESSFUL write, its health and its 'incomplete' flag are not lost");
    require(!outcome.primaryNative.has_value() && !outcome.fallbackNative.has_value(), "no native failure on a success");
    require(f.memory.writes == 0, "the service itself never writes memory segments");
}

void testCompactIsAThinViewOfTheSameSingleCall()
{
    {
        Fixture f;
        f.repository.commitScript.push_back(committed());
        const auto view = f.service.compact(f.projectId, f.providerId, f.modelId, {});
        require(view.success() && view.value().status == ProjectMemoryCompactionStatus::Completed, "compact(): the same success");
        require(f.client.observedCalls == 1 && f.repository.commits.size() == 1 && f.repository.inserts.empty(),
            "compact(): no double execution (one provider call, one commit)");
    }
    {
        Fixture f;
        f.repository.commitScript.push_back(failed(S::NotCommitted, C::Healthy, "persistence.transaction_failed", native(787, "FOREIGN KEY")));
        f.repository.insertScript.push_back(committed());
        const auto view = f.service.compact(f.projectId, f.providerId, f.modelId, {});
        require(!view.success() && view.error()->code == "persistence.transaction_failed",
            "compact(): the failure is the PRIMARY one although the fallback row was stored");
        require(f.client.observedCalls == 1 && f.repository.commits.size() == 1 && f.repository.inserts.size() == 1,
            "compact(): still one provider call, one commit, one fallback");
    }
}

// ---- 2. a failed commit with a proof: exactly one fallback ------------------------------------------------------------------------------
void testAProvenNotCommittedCommitOnAHealthyConnectionGetsExactlyOneFailedAccountingRow()
{
    Fixture f;
    f.repository.commitScript.push_back(failed(S::NotCommitted, C::Healthy, "persistence.transaction_failed", native(787, "FOREIGN KEY constraint failed")));
    f.repository.insertScript.push_back(committed(C::Healthy));

    const auto outcome = f.run();
    require(!outcome.result.success() && outcome.result.error()->code == "persistence.transaction_failed"
                && outcome.result.error()->message == "primary text of persistence.transaction_failed",
        "the PRIMARY failure stays the result, with its own text (not replaced by the fixed fallback text)");
    require(outcome.accounting == A::Saved && outcome.responseObserved, "the usage is Saved by the fallback row");
    require(f.repository.commits.size() == 1 && f.repository.inserts.size() == 1 && f.repository.lookups.empty() && f.repository.listCalls == 0,
        "ONE commit, ONE fallback insert, no lookup");
    const ProjectMemoryCompactionRecord& attempted = f.repository.commits[0];
    const ProjectMemoryCompactionRecord& fallback = f.repository.inserts[0];
    require(fallback.id == attempted.id && fallback.projectId == attempted.projectId && fallback.providerId == attempted.providerId
                && fallback.modelId == attempted.modelId && fallback.sourceSegmentIds == attempted.sourceSegmentIds
                && fallback.sourceContentHash == attempted.sourceContentHash && fallback.createdAt == attempted.createdAt,
        "the fallback has the SAME id, project, provider, model, ordered sources, hash and creation time");
    require(fallback.inputTokens == 40 && fallback.outputTokens == 15 && fallback.costMicros == attempted.costMicros && attempted.costMicros > 0
                && fallback.costPriceKnown == attempted.costPriceKnown && fallback.costPriceKnown == std::optional<bool> {true},
        "...the same usage, amount and flag (nothing recalculated)");
    require(fallback.status == ProjectMemoryCompactionStatus::Failed && !fallback.resultSegmentId.has_value()
                && fallback.errorCode == "persistence.transaction_failed" && fallback.errorMessage == policy::kFallbackErrorMessage
                && fallback.errorMessage.find("FOREIGN") == std::string::npos,
        "Failed, no result segment, the primary code, the fixed text (no SQLite text)");
    require(outcome.observedRecord.has_value() && policy::identityDifference(fallback, *outcome.observedRecord) == nullptr,
        "observedRecord is the fallback that was tried");
    require(f.client.observedCalls == 1, "NO second provider call");
    require(f.repository.committedSegments.size() == 1 && f.memory.writes == 0, "no second summary / segment and the memory is not replaced");
    require(outcome.primaryNative.has_value() && outcome.primaryNative->extendedCode == 787 && !outcome.fallbackNative.has_value()
                && outcome.connection == C::Healthy,
        "the primary native codes are kept apart; the fallback had none; the health is the last observed");
}

void testNothingIsWrittenWhenThePrimaryOutcomeIsNotProven()
{
    // Unknown, whatever the health says
    for (const std::optional<C> connection : {std::optional<C> {}, std::optional<C> {C::Healthy}, std::optional<C> {C::Quarantined},
             std::optional<C> {C::IsolationFailed}}) {
        Fixture f;
        auto unknown = failed(S::Unknown, connection, "persistence.transaction_failed", native(787, "FOREIGN KEY"));
        unknown.diagnostics.push_back(cause(CompactionDiagnosticStage::Rollback, "persistence.rollback_failed"));
        unknown.diagnostics.push_back(cause(CompactionDiagnosticStage::ProofRead, "persistence.proof_unavailable"));
        f.repository.commitScript.push_back(unknown);
        const auto outcome = f.run();
        require(f.repository.inserts.empty() && f.repository.lookups.empty() && f.client.observedCalls == 1, "Unknown: ZERO fallback inserts");
        require(!outcome.result.success() && outcome.accounting == A::Unknown && outcome.responseObserved, "Unknown: the accounting is Unknown");
        require(outcome.connection == connection, "...the health is passed on exactly as observed (nullopt stays nullopt)");
        require(outcome.observedRecord.has_value() && outcome.observedRecord->status == ProjectMemoryCompactionStatus::Completed
                    && outcome.observedRecord->resultSegmentId.has_value(),
            "...and the Completed record is NOT rewritten into a Failed one that was never written");
        require(outcome.primaryNative.has_value() && outcome.primaryNative->extendedCode == 787, "...the primary native codes are kept");
        require(indexOfDiagnostic(outcome, "persistence.rollback_failed") < indexOfDiagnostic(outcome, "persistence.proof_unavailable")
                    && indexOfDiagnostic(outcome, "persistence.proof_unavailable") < indexOfDiagnostic(outcome, "project_memory.fallback_not_attempted"),
            "...the causes keep their order and the reason for no fallback comes after them");
    }
    // NotCommitted but the connection is not explicitly healthy
    for (const std::optional<C> connection : {std::optional<C> {}, std::optional<C> {C::Quarantined}, std::optional<C> {C::IsolationFailed}}) {
        Fixture f;
        f.repository.commitScript.push_back(failed(S::NotCommitted, connection));
        const auto outcome = f.run();
        require(f.repository.inserts.empty() && f.repository.lookups.empty(), "NotCommitted + unusable/unobserved health: ZERO fallback inserts");
        require(outcome.accounting == A::NotSaved && !outcome.result.success(), "...NotSaved (this write is proven not stored), still a failure");
        require(outcome.connection == connection, "...the health as observed");
        require(hasDiagnostic(outcome, CompactionDiagnosticStage::FallbackWrite, "project_memory.fallback_not_attempted"),
            "...the reason nothing else was tried is a separate cause");
    }
}

void testAFailedFallbackKeepsBothCausesAndNeverTriesAgain()
{
    for (const S fallbackState : {S::NotCommitted, S::Unknown}) {
        Fixture f;
        auto primary = failed(S::NotCommitted, C::Healthy, "persistence.transaction_failed", native(787, "FOREIGN KEY"));
        primary.diagnostics.push_back(cause(CompactionDiagnosticStage::Rollback, "entry.note"));
        f.repository.commitScript.push_back(primary);
        auto fallback = failed(fallbackState, C::Quarantined, "persistence.write_failed", native(5, "database is locked"));
        // production-shaped: the repository names its causes Rollback / ProofRead, whichever write it was
        fallback.diagnostics.push_back(cause(CompactionDiagnosticStage::Rollback, "fallback.cleanup"));
        fallback.diagnostics.push_back(cause(CompactionDiagnosticStage::ProofRead, "fallback.proof"));
        f.repository.insertScript.push_back(fallback);

        const auto outcome = f.run();
        require(f.repository.inserts.size() == 1 && f.repository.lookups.empty(), "ONE fallback insert, never a second one");
        require(!outcome.result.success() && outcome.result.error()->code == "persistence.transaction_failed",
            "the result is the PRIMARY failure, not the fallback's");
        require(outcome.accounting == (fallbackState == S::NotCommitted ? A::NotSaved : A::Unknown), "the accounting follows the fallback's proof");
        require(outcome.primaryNative.has_value() && outcome.primaryNative->extendedCode == 787 && outcome.fallbackNative.has_value()
                    && outcome.fallbackNative->extendedCode == 5,
            "the primary and the fallback native codes are two separate values");
        require(outcome.connection == C::Quarantined, "the health is the LAST one observed (the fallback's)");
        require(hasDiagnostic(outcome, CompactionDiagnosticStage::Rollback, "entry.note"),
            "the PRIMARY write's cause keeps its own phase (Rollback)");
        require(hasDiagnostic(outcome, CompactionDiagnosticStage::FallbackRollback, "fallback.cleanup")
                    && hasDiagnostic(outcome, CompactionDiagnosticStage::FallbackProofRead, "fallback.proof")
                    && !hasDiagnostic(outcome, CompactionDiagnosticStage::Rollback, "fallback.cleanup")
                    && !hasDiagnostic(outcome, CompactionDiagnosticStage::ProofRead, "fallback.proof"),
            "the fallback write's causes are named by the APPLICATION: Rollback -> FallbackRollback, ProofRead -> FallbackProofRead");
        require(hasDiagnostic(outcome, CompactionDiagnosticStage::FallbackWrite, "persistence.write_failed"),
            "the fallback insert's OWN error is a separate cause (FallbackWrite)");
        require(indexOfDiagnostic(outcome, "entry.note") < indexOfDiagnostic(outcome, "persistence.write_failed")
                    && indexOfDiagnostic(outcome, "persistence.write_failed") < indexOfDiagnostic(outcome, "fallback.cleanup")
                    && indexOfDiagnostic(outcome, "fallback.cleanup") < indexOfDiagnostic(outcome, "fallback.proof")
                    && indexOfDiagnostic(outcome, "fallback.proof") < outcome.diagnostics.size(),
            "primary causes, the fallback write's error, then its secondary causes, in the order they happened");
        require(outcome.observedRecord.has_value() && outcome.observedRecord->status == ProjectMemoryCompactionStatus::Failed,
            "observedRecord is the fallback that was attempted");
        require(f.client.observedCalls == 1 && outcome.responseObserved, "no second provider call");
    }
}

// ---- 3. an id that is already taken -------------------------------------------------------------------------------------------------
CompactionPersistOutcome duplicateOutcome(std::optional<C> connection, S state = S::NotCommitted)
{
    return failed(state, connection, "project_memory.duplicate_id", native(1555, "UNIQUE constraint failed"));
}

void testADuplicateFallbackIdIsReconciledByTheStrictLookupOnly()
{
    using Row = ProjectMemoryCompactionRecord;
    using Lookup = common::Result<std::optional<Row>>;
    struct Done {
        ProjectMemoryCompactionOutcome outcome;
        std::size_t inserts;
        std::size_t lookups;
        int lists;
    };
    // `stored` turns the fallback the service sent into the answer of the ONE lookup of its id.
    const auto resolve = [](std::function<Lookup(const Row&)> stored, std::optional<C> connection = C::Healthy, S fallbackState = S::NotCommitted) {
        Fixture f;
        f.repository.commitScript.push_back(failed(S::NotCommitted, C::Healthy, "persistence.transaction_failed", native(787, "FK")));
        f.repository.insertScript.push_back(duplicateOutcome(connection, fallbackState));
        f.repository.lookupFrom = std::move(stored);
        auto outcome = f.run();
        return Done {std::move(outcome), f.repository.inserts.size(), f.repository.lookups.size(), f.repository.listCalls};
    };
    const auto millisecondForm = [](Row row) {
        row.createdAt = std::chrono::system_clock::time_point(
            std::chrono::duration_cast<std::chrono::milliseconds>(row.createdAt.time_since_epoch()));
        return row;
    };

    // the exact stored form of the fallback (millisecond precision): Saved, idempotent
    {
        const auto r = resolve([&](const Row& sent) { return Lookup::ok(millisecondForm(sent)); });
        require(r.outcome.accounting == A::Saved && !r.outcome.result.success() && r.outcome.result.error()->code == "persistence.transaction_failed",
            "an identical stored row -> Saved, and the result is STILL the primary failure");
        require(r.inserts == 1 && r.lookups == 1 && r.lists == 0, "exactly ONE insert, ONE strict lookup of that id, ZERO history listings");
        require(hasDiagnostic(r.outcome, CompactionDiagnosticStage::IdentityCheck, "project_memory.record_already_stored"), "...the reconciliation is visible");
        require(r.outcome.fallbackNative.has_value() && r.outcome.fallbackNative->extendedCode == 1555 && r.outcome.primaryNative.has_value()
                    && r.outcome.primaryNative->extendedCode == 787,
            "...with both native codes kept apart");
    }
    // every immutable field, one at a time: a conflict, and never an overwrite
    struct Mutation {
        const char* field;
        void (*apply)(Row&);
    };
    const Mutation mutations[] = {
        {"id", [](Row& s) { s.id += "x"; }},
        {"project_id", [](Row& s) { s.projectId = "other-project"; }},
        {"provider_id", [](Row& s) { s.providerId = "other-provider"; }},
        {"model_id", [](Row& s) { s.modelId = "other-model"; }},
        {"source order", [](Row& s) { std::swap(s.sourceSegmentIds.front(), s.sourceSegmentIds.back()); }},
        {"source missing", [](Row& s) { s.sourceSegmentIds.pop_back(); }},
        {"source_content_hash", [](Row& s) { s.sourceContentHash = std::string(64, 'f'); }},
        {"result_segment_id", [](Row& s) { s.resultSegmentId = "some-result"; }},
        {"input_tokens", [](Row& s) { s.inputTokens += 1; }},
        {"output_tokens", [](Row& s) { s.outputTokens += 1; }},
        {"cost_micros", [](Row& s) { s.costMicros += 1; }},
        {"flag true->false", [](Row& s) { s.costPriceKnown = false; }},
        {"flag true->nullopt", [](Row& s) { s.costPriceKnown = std::nullopt; }},
        {"status", [](Row& s) { s.status = ProjectMemoryCompactionStatus::Cancelled; }},
        {"error_code", [](Row& s) { s.errorCode = "other.code"; }},
        {"error_message", [](Row& s) { s.errorMessage = "other text"; }},
        {"next millisecond", [](Row& s) { s.createdAt += std::chrono::milliseconds(1); }},
    };
    for (const Mutation& mutation : mutations) {
        const auto r = resolve([&](const Row& sent) {
            Row stored = millisecondForm(sent);
            mutation.apply(stored);
            return Lookup::ok(stored);
        });
        require(r.outcome.accounting == A::NotSaved && hasDiagnostic(r.outcome, CompactionDiagnosticStage::IdentityCheck, "project_memory.record_id_conflict"),
            std::string("a stored row that differs in ") + mutation.field + " -> NotSaved + record_id_conflict");
        require(r.inserts == 1 && r.lookups == 1 && r.lists == 0, std::string("...one insert, one lookup, no overwrite (") + mutation.field + ")");
    }
    // a lookup that cannot say what is stored, or contradicts the duplicate: Unknown
    {
        const auto damaged = resolve([](const Row&) { return Lookup::failure({"persistence.read_failed", "damaged provenance list"}); });
        require(damaged.outcome.accounting == A::Unknown && hasDiagnostic(damaged.outcome, CompactionDiagnosticStage::IdentityCheck, "project_memory.identity_check_failed"),
            "a damaged / unreadable row -> Unknown");
        require(hasOriginalCause(damaged.outcome, CompactionDiagnosticStage::IdentityCheck, "persistence.read_failed", "damaged provenance list"),
            "...and the lookup's ORIGINAL error (code and message) is a separate typed cause, not a code inside a text");
        const auto unsupported = resolve([](const Row&) { return Lookup::failure({"project_memory.lookup_unsupported", "this repository cannot look up"}); });
        require(unsupported.outcome.accounting == A::Unknown
                    && hasOriginalCause(unsupported.outcome, CompactionDiagnosticStage::IdentityCheck, "project_memory.lookup_unsupported", "this repository cannot look up"),
            "an unsupported lookup -> Unknown, with its original error");
        const auto missing = resolve([](const Row&) { return Lookup::ok(std::nullopt); });
        require(missing.outcome.accounting == A::Unknown && hasDiagnostic(missing.outcome, CompactionDiagnosticStage::IdentityCheck, "project_memory.identity_contradiction"),
            "a duplicate although no row exists -> Unknown, never Saved");
    }
    // no lookup on a connection that is not explicitly healthy
    for (const std::optional<C> connection : {std::optional<C> {}, std::optional<C> {C::Quarantined}, std::optional<C> {C::IsolationFailed}}) {
        const auto r = resolve([&](const Row& sent) { return Lookup::ok(millisecondForm(sent)); }, connection);
        require(r.lookups == 0 && r.inserts == 1 && r.lists == 0, "no lookup on an unusable / unobserved connection");
        // the health is the LAST one actually observed: the fallback's when it observed one, otherwise the primary write's (Healthy here)
        const std::optional<C> lastObserved = connection.has_value() ? connection : std::optional<C> {C::Healthy};
        require(r.outcome.accounting == A::NotSaved && r.outcome.connection == lastObserved, "...the verdict is the fallback's own, the health as last observed");
    }
}

// R1 of the S2b-2 review: the error of the fallback insert is returned by the repository ONLY in its result. With no native detail and
// no secondary cause (both are allowed by the port) it must still be found in the outcome, apart from the commit error.
void testTheFallbackInsertsOwnErrorIsKeptEvenWithoutNativeOrSecondaryCauses()
{
    Fixture f;
    f.repository.commitScript.push_back(failed(S::NotCommitted, C::Healthy, "persistence.transaction_failed"));   // no native, no causes
    f.repository.insertScript.push_back(failed(S::NotCommitted, C::Healthy, "persistence.write_failed"));        // no native, no causes
    const auto outcome = f.run();
    require(!outcome.result.success() && outcome.result.error()->code == "persistence.transaction_failed"
                && outcome.result.error()->message == "primary text of persistence.transaction_failed",
        "the primary error is untouched");
    require(!outcome.primaryNative.has_value() && !outcome.fallbackNative.has_value(), "(there are no native details to fall back on)");
    require(outcome.diagnostics.size() == 1
                && hasOriginalCause(outcome, CompactionDiagnosticStage::FallbackWrite, "persistence.write_failed", "primary text of persistence.write_failed"),
        "the fallback insert's error is the one separate cause, with its original code and message, in the FallbackWrite phase");
    require(outcome.accounting == A::NotSaved, "and the verdict is unchanged");

    // the duplicate path: the fallback's own error is kept too, then the lookup's
    Fixture g;
    g.repository.commitScript.push_back(failed(S::NotCommitted, C::Healthy));
    g.repository.insertScript.push_back(failed(S::NotCommitted, C::Healthy, "project_memory.duplicate_id"));
    g.repository.lookup = common::Result<std::optional<ProjectMemoryCompactionRecord>>::failure({"persistence.read_failed", "row damaged"});
    const auto duplicate = g.run();
    require(hasOriginalCause(duplicate, CompactionDiagnosticStage::FallbackWrite, "project_memory.duplicate_id", "primary text of project_memory.duplicate_id"),
        "duplicate: the fallback insert's error is kept");
    require(hasOriginalCause(duplicate, CompactionDiagnosticStage::IdentityCheck, "persistence.read_failed", "row damaged"),
        "duplicate: the lookup's original error is kept apart");
    require(indexOfDiagnostic(duplicate, "project_memory.duplicate_id") < indexOfDiagnostic(duplicate, "persistence.read_failed")
                && indexOfDiagnostic(duplicate, "persistence.read_failed") < indexOfDiagnostic(duplicate, "project_memory.identity_check_failed"),
        "in the order they happened: the fallback write, the lookup, then the verdict in words");
    require(duplicate.accounting == A::Unknown && !duplicate.result.success() && duplicate.result.error()->code == "persistence.transaction_failed",
        "Unknown; the primary error is still the result");
}

// ---- 4. terminal records that carry paid usage, and the attempts without a response ------------------------------------------------------
void testAPaidFailedRecordThatCannotBeStoredKeepsEverythingAndIsWrittenOnce()
{
    for (const bool oversize : {false, true}) {
        for (const S state : {S::NotCommitted, S::Unknown}) {
            Fixture f;
            f.client.content = oversize ? std::string(ProjectMemoryCompactionService::kMaxSummaryBytes + 1, 'x') : std::string("   \n ");
            auto insert = failed(state, C::Healthy, "persistence.write_failed", native(13, "disk full"));
            insert.diagnostics.push_back(cause(CompactionDiagnosticStage::ProofRead, "persistence.read_failed"));
            f.repository.insertScript.push_back(insert);
            const auto outcome = f.run();
            require(!outcome.result.success() && outcome.result.error()->code == "persistence.write_failed",
                "the result is the failure of THIS insert (the terminal cause lives in the record)");
            require(outcome.accounting == (state == S::NotCommitted ? A::NotSaved : A::Unknown) && outcome.responseObserved,
                "NotSaved / Unknown by the insert's own proof");
            require(f.repository.inserts.size() == 1 && f.repository.commits.empty() && f.repository.lookups.empty() && f.client.observedCalls == 1,
                "ONE provider call, ONE insert, no commit, no second insert, no lookup");
            require(outcome.observedRecord.has_value() && outcome.observedRecord->status == ProjectMemoryCompactionStatus::Failed
                        && outcome.observedRecord->errorCode == (oversize ? "project_memory.summary_too_large" : "project_memory.empty_summary")
                        && outcome.observedRecord->inputTokens == 40 && outcome.observedRecord->outputTokens == 15
                        && outcome.observedRecord->costMicros > 0 && outcome.observedRecord->costPriceKnown == std::optional<bool> {true},
                "the terminal cause, the usage, the amount and the flag are kept in the record");
            require(outcome.primaryNative.has_value() && outcome.primaryNative->extendedCode == 13 && !outcome.fallbackNative.has_value()
                        && outcome.connection == C::Healthy && hasDiagnostic(outcome, CompactionDiagnosticStage::ProofRead, "persistence.read_failed"),
                "native codes, health and causes of the insert are kept");
        }
    }
    // the same record stored: the old success
    Fixture f;
    f.client.content = "";
    f.repository.insertScript.push_back(committed());
    const auto stored = f.run();
    require(stored.result.success() && stored.result.value().status == ProjectMemoryCompactionStatus::Failed && stored.accounting == A::Saved,
        "a stored terminal record is the old ok(record) and Saved");
}

void testAPaidCancelledRecordThatCannotBeStoredKeepsTheCancelledCause()
{
    for (const bool discardedPath : {false, true}) {
        Fixture f;
        std::stop_source stop;
        f.client.stopSource = &stop;
        ProviderChatResponse received;
        received.usage.inputTokens = 100;
        received.usage.outputTokens = 7;
        if (discardedPath) {
            f.client.failure = common::Error {"provider.cancelled", "cancelled in flight"};
            f.client.discarded = received;
        } else {
            f.client.usage = received.usage;
        }
        f.repository.insertScript.push_back(failed(S::NotCommitted, C::Healthy, "persistence.write_failed", native(13, "disk full")));
        const auto outcome = f.run(stop.get_token());
        require(!outcome.result.success() && outcome.result.error()->code == "persistence.write_failed",
            "the result is the failure of the cancelled record's insert");
        require(outcome.accounting == A::NotSaved && outcome.responseObserved, "a received response, not stored: NotSaved");
        require(outcome.observedRecord.has_value() && outcome.observedRecord->status == ProjectMemoryCompactionStatus::Cancelled
                    && outcome.observedRecord->errorCode == "provider.cancelled" && outcome.observedRecord->inputTokens == 100
                    && outcome.observedRecord->outputTokens == 7 && outcome.observedRecord->costPriceKnown.has_value(),
            "the cancelled record keeps provider.cancelled and the received usage; the storage code does not replace it");
        require(f.repository.inserts.size() == 1 && f.repository.commits.empty() && f.client.observedCalls == 1, "one provider call, one insert, no commit");
    }
}

void testAttemptsWithoutAResponseAreNotAccountedAndNeverHideAStorageFailure()
{
    // stop before anything was sent
    {
        Fixture f;
        std::stop_source stop;
        stop.request_stop();
        f.repository.insertScript.push_back(failed(S::Unknown, C::Quarantined, "persistence.connection_unusable"));
        const auto outcome = f.run(stop.get_token());
        require(f.client.observedCalls == 0 && f.client.sendCalls == 0 && f.factory.resolutions == 0, "pre-stop: ZERO provider calls, no client resolved");
        require(!outcome.result.success() && outcome.result.error()->code == "persistence.connection_unusable",
            "pre-stop: the storage failure is the result");
        require(outcome.accounting == A::NotApplicable && !outcome.responseObserved && outcome.connection == C::Quarantined,
            "...NotApplicable (no response) and the health is NOT hidden");
        require(outcome.observedRecord.has_value() && outcome.observedRecord->status == ProjectMemoryCompactionStatus::Cancelled
                    && outcome.observedRecord->inputTokens == 0 && outcome.observedRecord->outputTokens == 0
                    && outcome.observedRecord->costMicros == 0 && !outcome.observedRecord->costPriceKnown.has_value(),
            "...ONE Cancelled attempt record with 0/0/NULL (no estimate was invented)");
        require(f.repository.inserts.size() == 1 && f.repository.commits.empty(), "...one insert, no fallback");
    }
    // the same pre-stop, stored
    {
        Fixture f;
        std::stop_source stop;
        stop.request_stop();
        f.repository.insertScript.push_back(committed());
        const auto outcome = f.run(stop.get_token());
        require(outcome.result.success() && outcome.result.value().status == ProjectMemoryCompactionStatus::Cancelled
                    && outcome.accounting == A::NotApplicable,
            "pre-stop, stored: the old ok(record), NotApplicable");
    }
    // a provider failure, no response
    {
        Fixture f;
        f.client.failure = common::Error {"provider.timeout", "raw provider text"};
        f.repository.insertScript.push_back(failed(S::NotCommitted, C::Healthy, "persistence.write_failed", native(13, "disk full")));
        const auto outcome = f.run();
        require(!outcome.result.success() && outcome.result.error()->code == "persistence.write_failed" && outcome.accounting == A::NotApplicable
                    && !outcome.responseObserved && outcome.primaryNative.has_value() && outcome.connection == C::Healthy,
            "provider failure + storage failure: the storage error, NotApplicable, health and codes kept");
        require(outcome.observedRecord.has_value() && outcome.observedRecord->errorCode == "provider.timeout"
                    && !outcome.observedRecord->costPriceKnown.has_value() && outcome.observedRecord->costMicros == 0,
            "...and no cost / 'known 0' was invented for a call without a response");
        require(f.repository.inserts.size() == 1, "...one insert");
    }
    {
        Fixture f;
        f.client.failure = common::Error {"provider.timeout", "raw provider text"};
        f.repository.insertScript.push_back(committed());
        const auto outcome = f.run();
        require(outcome.result.success() && outcome.result.value().status == ProjectMemoryCompactionStatus::Failed && outcome.accounting == A::NotApplicable,
            "provider failure, stored: the old ok(record), NotApplicable");
    }
    // a stop that came with no response at all
    {
        Fixture f;
        std::stop_source stop;
        f.client.stopSource = &stop;
        f.client.failure = common::Error {"provider.cancelled", "cancelled"};
        f.repository.insertScript.push_back(committed());
        const auto outcome = f.run(stop.get_token());
        require(outcome.result.success() && outcome.accounting == A::NotApplicable && !outcome.responseObserved
                    && outcome.observedRecord->inputTokens == 0 && !outcome.observedRecord->costPriceKnown.has_value(),
            "a cancel without a response: 0/0/NULL, NotApplicable");
    }
}

void testFailuresBeforeAnAttemptTouchNothing()
{
    Fixture f;
    const auto unknownProject = f.service.compactObserved("no-such-project", f.providerId, f.modelId, {});
    require(!unknownProject.result.success() && unknownProject.result.error()->code == "project_memory.project_not_found"
                && unknownProject.accounting == A::NotApplicable && !unknownProject.observedRecord.has_value() && !unknownProject.responseObserved,
        "unknown project: the old failure, NotApplicable, no record");
    f.memory.segments.resize(1);
    const auto fewSegments = f.run();
    require(!fewSegments.result.success() && fewSegments.result.error()->code == "project_memory.insufficient_segments_to_compact"
                && fewSegments.accounting == A::NotApplicable && !fewSegments.observedRecord.has_value(),
        "fewer than two segments: the old failure, no record");
    require(f.client.observedCalls == 0 && f.factory.resolutions == 0 && f.repository.inserts.empty() && f.repository.commits.empty(),
        "no provider call and no write, no fallback attempt for a validation failure");
}

// ---- 5. limits, and the legacy port ---------------------------------------------------------------------------------------------------
void testTheMergedDiagnosticsAreBoundedAndTheLossIsFlagged()
{
    Fixture f;
    auto primary = failed(S::NotCommitted, C::Healthy);
    for (int i = 0; i < 6; ++i) primary.diagnostics.push_back(cause(CompactionDiagnosticStage::Rollback, ("primary." + std::to_string(i)).c_str()));
    f.repository.commitScript.push_back(primary);
    auto fallback = failed(S::NotCommitted, C::Healthy, "persistence.write_failed");
    for (int i = 0; i < 6; ++i) fallback.diagnostics.push_back(cause(CompactionDiagnosticStage::Rollback, ("fallback." + std::to_string(i)).c_str()));
    f.repository.insertScript.push_back(fallback);
    const auto outcome = f.run();
    require(outcome.diagnostics.size() == kMaxCompactionDiagnostics && outcome.diagnosticsIncomplete, "at most 8 causes, and the loss is flagged");
    require(outcome.diagnostics[5].error.code == "primary.5" && outcome.diagnostics[6].error.code == "persistence.write_failed"
                && outcome.diagnostics[7].error.code == "fallback.0",
        "the order is kept: primary first, then the fallback write's error, then its causes");
    require(outcome.diagnostics[6].stage == CompactionDiagnosticStage::FallbackWrite && outcome.diagnostics[7].stage == CompactionDiagnosticStage::FallbackRollback,
        "...in the fallback phase");

    Fixture g;
    auto lost = committed();
    lost.diagnosticsIncomplete = true;
    g.repository.commitScript.push_back(lost);
    require(g.run().diagnosticsIncomplete, "a layer below that lost a cause makes the outcome incomplete");
    Fixture h;
    auto lostInFallback = failed(S::NotCommitted, C::Healthy, "persistence.write_failed");
    lostInFallback.diagnosticsIncomplete = true;
    h.repository.commitScript.push_back(failed(S::NotCommitted, C::Healthy));
    h.repository.insertScript.push_back(lostInFallback);
    require(h.run().diagnosticsIncomplete, "...also when it was the fallback write that lost it");
}

class LegacyRepository final : public IProjectMemoryCompactionRepository {
public:
    int commits {0};
    int inserts {0};
    common::Result<void> insertCompactionRecord(const ProjectMemoryCompactionRecord&) override
    {
        ++inserts;
        return common::Result<void>::ok();
    }
    common::Result<void> commitSuccessfulCompaction(const ProjectMemoryCompactionRecord&, ProjectMemorySegment) override
    {
        ++commits;
        return common::Result<void>::failure({"legacy.commit_failed", "no"});
    }
    [[nodiscard]] common::Result<std::vector<ProjectMemoryCompactionRecord>> listCompactionRecords(const ProjectId&) const override
    {
        return common::Result<std::vector<ProjectMemoryCompactionRecord>>::ok({});
    }
};

void testALegacyRepositoryFailureIsUnknownAndNeverClaimsARollback()
{
    Fixture base;
    LegacyRepository legacy;
    ProjectMemoryCompactionService service(base.memory, legacy, base.intents, base.projects, base.providers, base.models, base.factory);
    const auto outcome = service.compactObserved(base.projectId, base.providerId, base.modelId, {});
    require(legacy.commits == 1 && legacy.inserts == 0, "one commit through the default observed method and NO fallback insert");
    require(!outcome.result.success() && outcome.result.error()->code == "legacy.commit_failed" && outcome.accounting == A::Unknown
                && !outcome.connection.has_value() && outcome.responseObserved,
        "the legacy default is Unknown with an unobserved connection - no rollback is attributed to an old client");
    require(base.client.observedCalls == 1, "one provider call");
}

} // namespace

int runProjectMemoryCompactionPersistenceServiceTests()
{
    try {
        testACommittedCompactionIsSavedAndCarriesWhatTheStorageSaid();
        testCompactIsAThinViewOfTheSameSingleCall();
        testAProvenNotCommittedCommitOnAHealthyConnectionGetsExactlyOneFailedAccountingRow();
        testNothingIsWrittenWhenThePrimaryOutcomeIsNotProven();
        testAFailedFallbackKeepsBothCausesAndNeverTriesAgain();
        testADuplicateFallbackIdIsReconciledByTheStrictLookupOnly();
        testTheFallbackInsertsOwnErrorIsKeptEvenWithoutNativeOrSecondaryCauses();
        testAPaidFailedRecordThatCannotBeStoredKeepsEverythingAndIsWrittenOnce();
        testAPaidCancelledRecordThatCannotBeStoredKeepsTheCancelledCause();
        testAttemptsWithoutAResponseAreNotAccountedAndNeverHideAStorageFailure();
        testFailuresBeforeAnAttemptTouchNothing();
        testTheMergedDiagnosticsAreBoundedAndTheLossIsFlagged();
        testALegacyRepositoryFailureIsUnknownAndNeverClaimsARollback();
    } catch (const std::exception& exception) {
        std::cerr << "ProjectMemoryCompactionPersistenceServiceTests failed: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "ProjectMemoryCompactionPersistenceServiceTests: OK\n";
    return 0;
}
