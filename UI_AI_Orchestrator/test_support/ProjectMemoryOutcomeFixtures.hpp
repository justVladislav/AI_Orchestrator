#pragma once
// C2-B/S2b-3: shared FAKES of the project-memory outcome tests (the QtAdapter controller tests and the QML feature tests): a scripted
// OBSERVED compaction repository, a scripted memory, a controllable fake provider (condition-variable gates, no sleeps) and a Rig that
// wires the REAL ProjectMemoryController over the REAL ProjectMemoryCompactionService. No network, no database, no provider.
// The repository replays PRODUCTION phases (Rollback, ProofRead, PrimaryWrite); it never pre-marks the service's fallback phases.
#include <ai/application/catalog/ProviderCatalogService.hpp>
#include <ai/application/conversations/ProjectMemoryCompactionService.hpp>
#include <ai/application/conversations/ProjectMemoryService.hpp>
#include <ai/application/inmemory/InMemoryRepositories.hpp>
#include <ai/ui/ProjectMemoryController.hpp>
#include "QtTestSupport.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace ai::ui::test_support::memory_outcome {

using namespace ai::application;
namespace common = ai::common;
using C = CompactionConnectionState;
using S = CompactionPersistState;
using Stage = CompactionDiagnosticStage;

inline void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

inline bool waitFor(const char* label, const std::function<bool()>& predicate, std::chrono::milliseconds timeout = std::chrono::seconds(10))
{
    return ai::ui::test_support::waitForCondition(label, predicate, [] { return std::string(); }, timeout);
}


// The text every failing read of the QML scenarios reports: the SAME code and message on purpose, to prove that ownership is the
// operation that was running and never the text.
inline const common::Error& sharedReadFailure()
{
    static const common::Error error {"persistence.read_failed", "storage boom"};
    return error;
}

class ScriptedMemory final : public IProjectMemoryRepository {
public:
    std::vector<ProjectMemorySegment> segments;
    // When set, the segments cannot be read (the compaction itself never reads while this is set: tests set it after the service
    // read its sources).
    std::atomic<bool> listFails {false};

    common::Result<ProjectMemorySegment> insertMemorySegment(ProjectMemorySegment value) override
    {
        return common::Result<ProjectMemorySegment>::ok(std::move(value));
    }
    [[nodiscard]] common::Result<std::vector<ProjectMemorySegment>> listMemorySegments(const ProjectId&) const override
    {
        if (listFails.load()) return common::Result<std::vector<ProjectMemorySegment>>::failure(sharedReadFailure());
        return common::Result<std::vector<ProjectMemorySegment>>::ok(segments);
    }
    [[nodiscard]] common::Result<std::optional<ProjectMemorySegment>> findMemorySegment(const ProjectMemorySegmentId&) const override
    {
        return common::Result<std::optional<ProjectMemorySegment>>::ok(std::nullopt);
    }
    common::Result<void> removeMemorySegments(const ProjectId&) override { return common::Result<void>::ok(); }
};

class ScriptedRepository final : public IProjectMemoryCompactionRepository {
public:
    std::deque<CompactionPersistOutcome> commitScript;
    std::deque<CompactionPersistOutcome> insertScript;
    std::atomic<int> commits {0};
    std::atomic<int> inserts {0};
    std::atomic<int> legacyCalls {0};
    std::atomic<int> lookups {0};
    // A gate the commit can be held at, so that a stop request arrives while the commit is in flight.
    std::atomic<bool> holdCommit {false};
    std::atomic<bool> commitEntered {false};
    std::mutex gateMutex;
    std::condition_variable gate;
    bool commitReleased {false};
    std::vector<ProjectMemoryCompactionRecord> committedRecords;
    std::vector<ProjectMemoryCompactionRecord> insertedRecords;

    void releaseCommit()
    {
        std::lock_guard lock(gateMutex);
        commitReleased = true;
        gate.notify_all();
    }

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
    std::atomic<bool> listFails {false};   // the history cannot be read
    [[nodiscard]] common::Result<std::vector<ProjectMemoryCompactionRecord>> listCompactionRecords(const ProjectId&) const override
    {
        if (listFails.load()) return common::Result<std::vector<ProjectMemoryCompactionRecord>>::failure(sharedReadFailure());
        return common::Result<std::vector<ProjectMemoryCompactionRecord>>::ok({});
    }
    CompactionPersistOutcome commitSuccessfulCompactionObserved(const ProjectMemoryCompactionRecord& record, ProjectMemorySegment) override
    {
        committedRecords.push_back(record);
        ++commits;
        if (holdCommit.load()) {
            commitEntered = true;
            std::unique_lock lock(gateMutex);
            gate.wait(lock, [this] { return commitReleased; });
        }
        return next(commitScript);
    }
    CompactionPersistOutcome insertCompactionRecordObserved(const ProjectMemoryCompactionRecord& record) override
    {
        insertedRecords.push_back(record);
        ++inserts;
        return next(insertScript);
    }
    [[nodiscard]] common::Result<std::optional<ProjectMemoryCompactionRecord>> findCompactionRecord(const ProjectMemoryCompactionId&) const override
    {
        ++const_cast<ScriptedRepository*>(this)->lookups;
        return common::Result<std::optional<ProjectMemoryCompactionRecord>>::ok(std::nullopt);
    }

private:
    static CompactionPersistOutcome next(std::deque<CompactionPersistOutcome>& script)
    {
        if (script.empty()) {
            CompactionPersistOutcome outcome;
            outcome.result = common::Result<void>::failure({"test.unscripted_write", "no outcome was scripted for this write"});
            return outcome;
        }
        CompactionPersistOutcome outcome = std::move(script.front());
        script.pop_front();
        return outcome;
    }
};

// send() is a trap: the controller's service must use sendObserved() exactly once per accepted call.
class ControlledClient final : public IProviderClient {
public:
    std::atomic<int> sendCalls {0};
    std::atomic<int> observedCalls {0};
    std::atomic<bool> entered {false};
    bool hold {false};   // set before the call starts
    std::optional<common::Error> failure;
    std::optional<ProviderChatResponse> discarded;
    std::function<void()> whileInFlight;

    void release()
    {
        std::lock_guard lock(mutex_);
        released_ = true;
        gate_.notify_all();
    }

    common::Result<ProviderChatResponse> send(const ProviderChatRequest&, std::stop_token) override
    {
        ++sendCalls;
        return common::Result<ProviderChatResponse>::failure({"test.send_used", "the service must call sendObserved"});
    }
    ProviderChatOutcome sendObserved(const ProviderChatRequest&, std::stop_token token) override
    {
        ++observedCalls;
        entered = true;
        if (hold) {
            std::unique_lock lock(mutex_);
            gate_.wait(lock, token, [this] { return released_; });
            if (!released_ && token.stop_requested()) {
                // a cancelled call: the response that was in flight is reported as discarded (when there is one)
                return {common::Result<ProviderChatResponse>::failure({"provider.cancelled", "cancelled"}), discarded};
            }
        }
        if (whileInFlight) whileInFlight();
        if (failure.has_value()) return {common::Result<ProviderChatResponse>::failure(*failure), std::nullopt};
        ProviderChatResponse response;
        response.content = "CONSOLIDATED";
        response.usage.inputTokens = 40;
        response.usage.outputTokens = 15;
        return {common::Result<ProviderChatResponse>::ok(std::move(response)), std::nullopt};
    }

private:
    std::mutex mutex_;
    std::condition_variable_any gate_;
    bool released_ {false};
};

class ClientFactory final : public IProviderClientFactory {
public:
    explicit ClientFactory(IProviderClient& client) : client_(client) {}
    common::Result<std::shared_ptr<IProviderClient>> clientFor(const ProviderId&, const std::string&) override
    {
        return common::Result<std::shared_ptr<IProviderClient>>::ok(std::shared_ptr<IProviderClient>(&client_, [](IProviderClient*) {}));
    }

private:
    IProviderClient& client_;
};

inline void seedCatalog(InMemoryProviderRepository& providers, InMemoryModelRepository& models)
{
    const auto now = std::chrono::system_clock::now();
    Provider provider;
    provider.id = "prov1";
    provider.name = "Fake";
    provider.kind = "openai_compatible";
    provider.enabled = true;
    provider.createdAt = now;
    provider.updatedAt = now;
    require(providers.insert(provider).success(), "setup: provider");
    Model model;
    model.id = "model1";
    model.providerId = "prov1";
    model.apiModelId = "fake-model-1";
    model.displayName = "Fake Model";
    model.contextWindowTokens = 128000;
    model.maxOutputTokens = 4096;
    model.inputPriceMicrosPerMillionTokens = 5'000'000;
    model.outputPriceMicrosPerMillionTokens = 15'000'000;
    model.enabled = true;
    model.createdAt = now;
    model.updatedAt = now;
    require(models.insert(model).success(), "setup: model");
}

inline Project projectNamed(const std::string& id)
{
    Project project;
    project.id = id;
    project.name = "Compaction " + id;
    project.description = "d";
    project.rootPath = "C:/fixture/" + id;
    project.createdAt = std::chrono::system_clock::now();
    project.updatedAt = project.createdAt;
    return project;
}

inline ProjectMemorySegment segmentFor(const std::string& projectId, const std::string& id)
{
    ProjectMemorySegment segment;
    segment.id = id;
    segment.projectId = projectId;
    segment.runId = "run-" + id;
    segment.summary = "Summary of " + id;
    segment.sourceTokenCount = 11;
    segment.summaryTokenCount = 7;
    segment.createdAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(1000 + static_cast<int>(id.size())));
    return segment;
}

// ---- scripted storage verdicts --------------------------------------------------------------------------------------------------------

inline CompactionPersistOutcome committed(std::optional<C> connection = C::Healthy)
{
    CompactionPersistOutcome outcome;
    outcome.state = S::Committed;
    outcome.connection = connection;
    return outcome;
}

inline CompactionPersistOutcome failedWrite(S state, std::optional<C> connection, const char* code = "persistence.transaction_failed",
    const char* message = "the write failed", std::optional<CompactionNativeDetail> nativeDetail = std::nullopt,
    std::vector<CompactionDiagnostic> diagnostics = {})
{
    CompactionPersistOutcome outcome;
    outcome.result = common::Result<void>::failure({code, message});
    outcome.state = state;
    outcome.connection = connection;
    outcome.primaryNative = std::move(nativeDetail);
    outcome.diagnostics = std::move(diagnostics);
    return outcome;
}

inline CompactionNativeDetail native(int extended, const char* text)
{
    CompactionNativeDetail detail;
    detail.extendedCode = extended;
    detail.code = extended & 0xFF;
    detail.message = text;
    return detail;
}


// The real controller over the real service with the fakes above. Nothing is recorded here: a test connects what it needs.
struct Rig {
    InMemoryProjectRepository projects;
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    ScriptedMemory memory;
    ScriptedRepository repository;
    ControlledClient client;
    ClientFactory factory {client};
    ProjectMemoryService memoryService {memory};
    ProviderCatalogService catalog {providers, models};
    ProjectMemoryCompactionService service {memory, repository, projects, providers, models, factory};
    std::unique_ptr<ProjectMemoryController> controller;

    explicit Rig(const std::string& projectId = "cp1")
    {
        seedCatalog(providers, models);
        require(projects.insert(projectNamed(projectId)).success(), "setup: project");
        for (int index = 0; index < 3; ++index) memory.segments.push_back(segmentFor(projectId, "seg" + std::to_string(index)));
        controller = std::make_unique<ProjectMemoryController>(memoryService, service, catalog);
    }
};

} // namespace ai::ui::test_support::memory_outcome
