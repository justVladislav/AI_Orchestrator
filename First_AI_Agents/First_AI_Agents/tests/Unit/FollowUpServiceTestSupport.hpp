#pragma once

// PC-5C test harness: the service and the processor over REAL in-memory repositories (requests, drafts, executions, catalog), a
// scripted source reader, a scripted provider and an injectable clock. No file, network or Qt.

#include "FollowUpRequestTestSupport.hpp"

#include "ai/application/planning/FollowUpComposerProcessor.hpp"
#include "ai/application/planning/FollowUpPlanService.hpp"
#include "ai/application/providers/IProviderClient.hpp"
#include "ai/application/inmemory/InMemoryFollowUpPlanRepository.hpp"
#include "ai/application/inmemory/InMemoryRepositories.hpp"
#include "ai/application/agents/ModelExecutionService.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace followup_service_test {

namespace common = ai::common;

using namespace followup_test;

inline constexpr const char* kProject = "project-1";
inline constexpr const char* kRun = "run-1";

inline FollowUpFileCandidate candidate(const std::string& path, const std::string& content,
    FollowUpFileSafety safety = FollowUpFileSafety::Eligible)
{
    FollowUpFileCandidate file;
    file.relativePath = path;
    file.safety = safety;
    if (safety == FollowUpFileSafety::Eligible) {
        file.content = content;
        file.contentSha256 = ai::application::sha256Hex(content);
    }
    return file;
}

// The source Run's material: the REAL snapshot and brief builders of the pure fixtures, plus candidate files.
inline FollowUpSourceMaterial materialOf(const std::vector<FollowUpFileCandidate>& files)
{
    FollowUpSourceMaterial material;
    material.project.id = kProject;
    material.project.name = "Demo";
    material.project.status = ProjectStatus::Active;
    material.runProjectId = kProject;
    material.brief = briefOf(kRun, kProject);
    material.snapshot = snapshotOf(kRun, kProject);
    material.candidates = files;
    return material;
}

inline std::vector<FollowUpFileCandidate> defaultFiles()
{
    return {candidate("src/a.cpp", "int a() { return 1; }\n"), candidate("src/b.cpp", "int b() { return 2; }\n")};
}

class ScriptedReader final : public IFollowUpSourceReader {
public:
    explicit ScriptedReader(FollowUpSourceMaterial initial) : material_(std::move(initial)) {}

    [[nodiscard]] common::Result<FollowUpSourceMaterial> read(const ProjectId& projectId, const RunId& runId) const override
    {
        std::lock_guard lock(mutex_);
        ++reads_;
        if (failure_.has_value()) return common::Result<FollowUpSourceMaterial>::failure(*failure_);
        if (projectId != material_.project.id) {
            return common::Result<FollowUpSourceMaterial>::failure({"follow_up.project_not_found", "No such project"});
        }
        if (runId != kRun) return common::Result<FollowUpSourceMaterial>::failure({"follow_up.run_not_found", "No such Run"});
        if (missingOutcome_) return common::Result<FollowUpSourceMaterial>::failure({"follow_up.outcome_missing", "No brief yet"});
        return common::Result<FollowUpSourceMaterial>::ok(material_);
    }

    template <typename Fn>
    void edit(Fn fn)
    {
        std::lock_guard lock(mutex_);
        fn(material_);
    }
    void fail(std::optional<common::Error> error)
    {
        std::lock_guard lock(mutex_);
        failure_ = std::move(error);
    }
    void setMissingOutcome(bool value)
    {
        std::lock_guard lock(mutex_);
        missingOutcome_ = value;
    }
    [[nodiscard]] int reads() const
    {
        std::lock_guard lock(mutex_);
        return reads_;
    }

private:
    mutable std::mutex mutex_;
    FollowUpSourceMaterial material_;
    std::optional<common::Error> failure_;
    bool missingOutcome_ {false};
    mutable int reads_ {0};
};

struct ScriptedReply {
    std::optional<ProviderChatResponse> ok;
    std::optional<common::Error> error;
    // The call parks until its stop token fires and then fails like a real client ("provider.cancelled").
    bool blockUntilStop {false};
    // ... unless it carries a complete answer, which a late stop does not recall: the call returns it AFTER the stop.
    bool answerAfterStop {false};

    [[nodiscard]] common::Result<ProviderChatResponse> toResult() const
    {
        if (ok.has_value()) return common::Result<ProviderChatResponse>::ok(*ok);
        return common::Result<ProviderChatResponse>::failure(error.value_or(common::Error {"provider.request_failed", "script exhausted"}));
    }
};

class ScriptedClient final : public IProviderClient {
public:
    common::Result<ProviderChatResponse> send(const ProviderChatRequest& request, std::stop_token stop) override
    {
        ScriptedReply reply;
        {
            std::lock_guard lock(mutex_);
            requests_.push_back(request);
            ++entered_;
            if (!script_.empty()) {
                reply = std::move(script_.front());
                script_.pop_front();
            }
        }
        enteredChanged_.notify_all();
        if (reply.blockUntilStop) {
            std::mutex parked;
            std::condition_variable_any wake;
            std::unique_lock lock(parked);
            wake.wait(lock, stop, [] { return false; });
            if (reply.answerAfterStop) return reply.toResult();
            return common::Result<ProviderChatResponse>::failure({"provider.cancelled", "stopped"});
        }
        return reply.toResult();
    }

    void push(ScriptedReply reply)
    {
        std::lock_guard lock(mutex_);
        script_.push_back(std::move(reply));
    }
    [[nodiscard]] std::size_t calls() const
    {
        std::lock_guard lock(mutex_);
        return requests_.size();
    }
    [[nodiscard]] ProviderChatRequest lastRequest() const
    {
        std::lock_guard lock(mutex_);
        check(!requests_.empty(), "the provider was called");
        return requests_.back();
    }
    // Blocks until the provider has been entered `count` times.
    void waitEntered(std::size_t count)
    {
        std::unique_lock lock(mutex_);
        enteredChanged_.wait(lock, [&] { return entered_ >= count; });
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable enteredChanged_;
    std::deque<ScriptedReply> script_;
    std::vector<ProviderChatRequest> requests_;
    std::size_t entered_ {0};
};

class ScriptedFactory final : public IProviderClientFactory {
public:
    explicit ScriptedFactory(std::shared_ptr<ScriptedClient> client) : client_(std::move(client)) {}

    common::Result<std::shared_ptr<IProviderClient>> clientFor(const ProviderId&, const std::string&) override
    {
        ++requests;
        if (failure.has_value()) return common::Result<std::shared_ptr<IProviderClient>>::failure(*failure);
        return common::Result<std::shared_ptr<IProviderClient>>::ok(client_);
    }

    std::atomic<int> requests {0};
    std::optional<common::Error> failure;

private:
    std::shared_ptr<ScriptedClient> client_;
};

// An execution repository whose inserts can be made to fail (the log is the money's record: a failure must stop the call).
class FlakyExecutions final : public IModelExecutionRepository {
public:
    InMemoryModelExecutionRepository inner;
    std::atomic<bool> failInserts {false};
    // The write that SETTLES an execution (completed / failed / cancelled) fails; the start of it still works. `failSettling` for
    // good, `failNextSettlings` for that many attempts only (a transient fault).
    std::atomic<bool> failSettling {false};
    std::atomic<int> failNextSettlings {0};

    common::Result<ModelExecution> insertExecution(ModelExecution value) override
    {
        if (failInserts) return common::Result<ModelExecution>::failure({"persistence.insert_failed", "injected"});
        return inner.insertExecution(std::move(value));
    }
    common::Result<ModelExecution> updateExecution(ModelExecution value) override
    {
        const bool settling = value.status != ModelExecutionStatus::Pending && value.status != ModelExecutionStatus::Running;
        if (failSettling && settling) return common::Result<ModelExecution>::failure({"persistence.update_failed", "injected"});
        if (settling && failNextSettlings > 0) {
            --failNextSettlings;
            return common::Result<ModelExecution>::failure({"persistence.update_failed", "injected once"});
        }
        return inner.updateExecution(std::move(value));
    }
    [[nodiscard]] common::Result<std::optional<ModelExecution>> findExecution(const ModelExecutionId& id) const override { return inner.findExecution(id); }
    [[nodiscard]] common::Result<std::vector<ModelExecution>> listExecutions(ModelExecutionSource source, const std::string& sourceId) const override
    {
        return inner.listExecutions(source, sourceId);
    }
    [[nodiscard]] common::Result<std::vector<ModelExecution>> listProjectExecutions(const ProjectId& projectId) const override
    {
        return inner.listProjectExecutions(projectId);
    }
    [[nodiscard]] common::Result<std::vector<ModelExecution>> listUnfinishedExecutions() const override { return inner.listUnfinishedExecutions(); }
    common::Result<QualityEvaluation> insertEvaluation(QualityEvaluation value) override { return inner.insertEvaluation(std::move(value)); }
    [[nodiscard]] common::Result<std::optional<QualityEvaluation>> findEvaluation(const QualityEvaluationId& id) const override { return inner.findEvaluation(id); }
    [[nodiscard]] common::Result<std::vector<QualityEvaluation>> listEvaluations(const ModelExecutionId& executionId) const override
    {
        return inner.listEvaluations(executionId);
    }
};

inline ScriptedReply okReply(const std::string& content, std::uint64_t inputTokens = 30'000, std::uint64_t outputTokens = 2'000)
{
    ProviderChatResponse response;
    response.content = content;
    response.usage.inputTokens = inputTokens;
    response.usage.outputTokens = outputTokens;
    ScriptedReply reply;
    reply.ok = std::move(response);
    return reply;
}

inline ScriptedReply errorReply(const std::string& code, const std::string& message)
{
    ScriptedReply reply;
    reply.error = common::Error {code, message};
    return reply;
}

inline ScriptedReply blockingReply()
{
    ScriptedReply reply;
    reply.blockUntilStop = true;
    return reply;
}

// A provider catalog whose reads can be made to fail (an expired request must stay expired whatever state the catalog is in).
class FlakyProviders final : public IProviderRepository {
public:
    InMemoryProviderRepository inner;
    std::atomic<bool> failReads {false};
    mutable std::atomic<int> reads {0};

    common::Result<Provider> insert(Provider provider) override { return inner.insert(std::move(provider)); }
    common::Result<Provider> update(Provider provider) override { return inner.update(std::move(provider)); }
    [[nodiscard]] common::Result<std::optional<Provider>> findById(const ProviderId& id) const override
    {
        ++reads;
        if (failReads) return common::Result<std::optional<Provider>>::failure({"persistence.read_failed", "injected"});
        return inner.findById(id);
    }
    [[nodiscard]] common::Result<std::vector<Provider>> listAll() const override { return inner.listAll(); }
};

struct Harness {
    Catalog catalog;
    FlakyProviders providers;
    InMemoryModelRepository models;
    InMemoryRunDraftRepository drafts;
    InMemoryFollowUpPlanRepository requests;
    FlakyExecutions executionStore;
    ModelExecutionService executions;
    ScriptedReader reader;
    std::shared_ptr<ScriptedClient> client;
    ScriptedFactory factory;
    std::atomic<std::int64_t> nowMs {0};
    std::vector<std::string> nudged;
    FollowUpPlanService service;
    FollowUpComposerProcessor processor;

    explicit Harness(std::vector<FollowUpFileCandidate> files = defaultFiles(), std::optional<common::Error> refusal = std::nullopt)
        : requests([](const std::string& runId) -> std::optional<ProjectId> {
              return runId == kRun || runId.rfind("run-", 0) == 0 ? std::optional<ProjectId>(kProject) : std::nullopt;
          }, &drafts)
        , executions(executionStore)
        , reader(materialOf(files))
        , client(std::make_shared<ScriptedClient>())
        , factory(client)
        , service(requests, requests, reader, providers, models, drafts, [this] { return at(nowMs.load()); }, refusal)
        , processor(requests, reader, providers, models, factory, executions, [this] { return at(nowMs.load()); }, refusal)
    {
        check(providers.insert(providerOf(catalog)).success(), "setup: provider");
        check(models.insert(modelOf(catalog)).success(), "setup: model");
        service.setAuthorizedNudge([this](const std::string& id) { nudged.push_back(id); });
        service.setComposingCancel([this](const std::string& id) { return processor.requestStop(id); });
    }

    [[nodiscard]] FollowUpIntent intent() const
    {
        FollowUpIntent value;
        value.projectId = kProject;
        value.sourceRunId = kRun;
        value.kind = FollowUpPlanKind::NextRun;
        value.goal = "Plan the next focused implementation step.";
        value.maxDrafts = 2;
        value.splitHint = FollowUpSplitHint::Auto;
        value.providerId = catalog.providerId;
        value.modelId = catalog.modelId;
        return value;
    }

    FollowUpPlanRequest quoted()
    {
        const auto outcome = service.quote(intent());
        check(outcome.success(), "setup: quote: " + codeOf(outcome.error()));
        return outcome.value().request;
    }
    FollowUpPlanRequest authorized()
    {
        const auto request = quoted();
        const auto result = service.authorize(request.id);
        check(result.success(), "setup: authorize: " + codeOf(result.error()));
        return result.value();
    }
    FollowUpPlanRequest planReady(const std::string& reply = replyOfPlanJson(planJsonOf('c', 2, true)))
    {
        const auto request = authorized();
        client->push(okReply(reply));
        const auto result = processor.processAuthorized(request.id);
        check(result.success() && result.value().state == FollowUpPlanRequestState::PlanReady,
            "setup: plan ready: " + codeOf(result.error()));
        return result.value();
    }
    FollowUpPlanRequest stored(const std::string& id) const
    {
        const auto found = requests.findById(id);
        check(found.success() && found.value().has_value(), "the request is stored");
        return *found.value();
    }
    [[nodiscard]] std::vector<ModelExecution> executionsOf(const std::string& id) const
    {
        const auto listed = executionStore.inner.listExecutions(ModelExecutionSource::FollowUp, id);
        check(listed.success(), "listExecutions");
        return listed.value();
    }
    void editModel(const std::function<void(Model&)>& edit)
    {
        auto model = models.findById(catalog.modelId).value().value();
        edit(model);
        check(models.update(model).success(), "setup: update model");
    }
    void editProvider(const std::function<void(Provider&)>& edit)
    {
        auto provider = providers.findById(catalog.providerId).value().value();
        edit(provider);
        check(providers.update(provider).success(), "setup: update provider");
    }
};

} // namespace followup_service_test
