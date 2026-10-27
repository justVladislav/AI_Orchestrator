#include "ai/application/FollowUpComposerProcessor.hpp"

#include "ai/application/CacheDurationAccounting.hpp"
#include "ai/application/CostCalculation.hpp"
#include "ai/application/planning/FollowUpPlanAdvisories.hpp"
#include "ai/application/context/ProjectContextSnapshot.hpp"
#include "ai/application/Sha256.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace ai::application {
namespace {

constexpr std::size_t kExecutionPreviewBytes = 1000;
constexpr std::size_t kRecoveryPageSize = 500;
constexpr std::uint64_t kMaxStoredFigure = std::numeric_limits<std::int64_t>::max();

template <typename T>
common::Result<T> fail(std::string code, std::string message)
{
    return common::Result<T>::failure({std::move(code), std::move(message)});
}

bool storableText(std::string_view text) noexcept
{
    return projectContextIsValidUtf8(text) && text.find('\0') == std::string_view::npos;
}

// Cuts at a character boundary: never leaves half of a UTF-8 sequence behind.
std::string truncateUtf8(std::string_view text, std::size_t maxBytes)
{
    if (text.size() <= maxBytes) return std::string(text);
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) --cut;
    return std::string(text.substr(0, cut));
}

// Text that is stored in a failure must be readable: valid UTF-8, no NUL, bounded.
std::string readableText(std::string_view text, std::size_t maxBytes)
{
    if (!storableText(text)) return "(the provider's message was not readable text)";
    return truncateUtf8(text, maxBytes);
}

bool usableFailureCode(const std::string& code)
{
    if (code.empty() || code.size() > kFollowUpMaxFailureCodeBytes || !storableText(code)) return false;
    return std::none_of(code.begin(), code.end(), [](char raw) {
        const auto ch = static_cast<unsigned char>(raw);
        return ch < 0x20 || ch == 0x7F;
    });
}

FollowUpPlanFailure makeFailure(const std::string& code, const std::string& message, const std::string* reply)
{
    FollowUpPlanFailure failure;
    failure.code = usableFailureCode(code) ? code : "follow_up.composition_failed";
    failure.message = readableText(message, kFollowUpMaxFailureMessageBytes);
    // The invalid answer's text stays readable when it can be stored as text; otherwise the typed error alone is kept.
    if (reply != nullptr && !reply->empty() && reply->size() <= kFollowUpMaxReplyBytes && storableText(*reply)) {
        failure.rawReply = *reply;
    }
    return failure;
}

// What the provider reported and what it costs, in the form the request stores and the execution log accepts. A usage whose cache
// breakdown cannot be trusted (the subsets exceed the input, or the duration split does not add up) is stored WITHOUT that
// breakdown and with an unknown price, because the log would refuse it and a price built on it would be a guess.
FollowUpAccounting accountingOf(const models::TokenUsage& usage, const CostBreakdown& cost, std::uint64_t latencyMs)
{
    FollowUpAccounting accounting;
    accounting.inputTokens = std::min(usage.inputTokens, kMaxStoredFigure);
    accounting.outputTokens = std::min(usage.outputTokens, kMaxStoredFigure);
    const bool coherent = usage.inputTokens <= kMaxStoredFigure && usage.cacheBreakdownIsConsistent()
        && checkCacheCreationDurationUsage(usage.cacheCreationInputTokens, usage.cacheCreationDuration)
            == CacheCreationDurationUsageProblem::None;
    if (coherent) {
        accounting.cacheCreationInputTokens = usage.cacheCreationInputTokens;
        accounting.cacheReadInputTokens = usage.cacheReadInputTokens;
        accounting.cacheCreationDurationKnown = usage.cacheCreationDuration.known;
        accounting.cacheCreationFiveMinuteTokens = usage.cacheCreationDuration.fiveMinuteTokens;
        accounting.cacheCreationOneHourTokens = usage.cacheCreationDuration.oneHourTokens;
    }
    accounting.costMicros = std::min(cost.costMicros, kMaxStoredFigure);
    accounting.priceKnown = coherent && cost.priceKnown;
    accounting.latencyMs = std::min(latencyMs, kMaxStoredFigure);
    return accounting;
}

// The execution row's outcome. `accounting` is absent when no provider response arrived (nothing was used, nothing is priced).
FinishModelExecutionRequest finishOf(ModelExecutionStatus status, const FollowUpAccounting* accounting, const std::string& output,
    const std::string& errorCode, const std::string& errorMessage)
{
    FinishModelExecutionRequest finish;
    finish.status = status;
    finish.outputContent = output;
    finish.outputPreview = truncateUtf8(output, kExecutionPreviewBytes);
    if (accounting != nullptr) {
        finish.inputTokens = accounting->inputTokens;
        finish.outputTokens = accounting->outputTokens;
        finish.cacheCreationInputTokens = accounting->cacheCreationInputTokens;
        finish.cacheReadInputTokens = accounting->cacheReadInputTokens;
        finish.cacheCreationDuration.known = accounting->cacheCreationDurationKnown;
        finish.cacheCreationDuration.fiveMinuteTokens = accounting->cacheCreationFiveMinuteTokens;
        finish.cacheCreationDuration.oneHourTokens = accounting->cacheCreationOneHourTokens;
        finish.costMicros = accounting->costMicros;
        finish.costPriceKnown = accounting->priceKnown;
        finish.latencyMs = accounting->latencyMs;
    }
    finish.errorCode = errorCode;
    finish.errorMessage = errorMessage;
    return finish;
}

// The execution outcome a SETTLED request implies: a stopped call is cancelled, everything else that got a response completed (an
// invalid answer is still a completed call that was paid for).
FinishModelExecutionRequest finishOfRequest(const FollowUpPlanRequest& request)
{
    std::string output;
    if (request.answer.has_value()) output = request.answer->rawReply;
    else if (request.failure.has_value() && request.failure->rawReply.has_value()) output = *request.failure->rawReply;
    if (request.state == FollowUpPlanRequestState::Cancelled) {
        return finishOf(ModelExecutionStatus::Cancelled, &*request.accounting, {}, "provider.cancelled", "The composer call was stopped");
    }
    return finishOf(ModelExecutionStatus::Completed, &*request.accounting, output, {}, {});
}

} // namespace

FollowUpComposerProcessor::FollowUpComposerProcessor(
    IFollowUpPlanRequestRepository& requests,
    const IFollowUpSourceReader& source,
    const IProviderRepository& providers,
    const IModelRepository& models,
    IProviderClientFactory& clients,
    ModelExecutionService& executions,
    Clock clock,
    std::optional<common::Error> refusal)
    : requests_(requests)
    , source_(source)
    , providers_(providers)
    , models_(models)
    , clients_(clients)
    , executions_(executions)
    , clock_(std::move(clock))
    , refusal_(std::move(refusal))
{
}

std::chrono::system_clock::time_point FollowUpComposerProcessor::now() const
{
    return clock_ ? clock_() : std::chrono::system_clock::now();
}

FollowUpAdmissionPorts FollowUpComposerProcessor::admissionPorts() const
{
    return FollowUpAdmissionPorts {source_, providers_, models_};
}

bool FollowUpComposerProcessor::requestStop(const FollowUpPlanRequestId& id)
{
    std::lock_guard lock(activeMutex_);
    const auto found = active_.find(id);
    if (found == active_.end()) return false;
    found->second.request_stop();
    return true;
}

common::Result<FollowUpPlanRequest> FollowUpComposerProcessor::processAuthorized(
    const FollowUpPlanRequestId& id, std::stop_token external)
{
    if (refusal_.has_value()) return common::Result<FollowUpPlanRequest>::failure(*refusal_);

    // Registered BEFORE the claim: a stop that arrives between the claim and the provider call must find this call and prevent it.
    std::stop_source local;
    {
        std::lock_guard lock(activeMutex_);
        if (active_.contains(id)) {
            return fail<FollowUpPlanRequest>("follow_up.already_running", "This request is already being processed");
        }
        active_.emplace(id, local);
    }
    struct Unregister {
        FollowUpComposerProcessor& self;
        const FollowUpPlanRequestId& id;
        ~Unregister()
        {
            std::lock_guard lock(self.activeMutex_);
            self.active_.erase(id);
        }
    } unregister {*this, id};
    std::stop_callback link(external, [&local] { local.request_stop(); });
    return drive(id, local.get_token());
}

common::Result<FollowUpPlanRequest> FollowUpComposerProcessor::drive(const FollowUpPlanRequestId& id, std::stop_token stop)
{
    using Result = common::Result<FollowUpPlanRequest>;
    const auto transitionOf = [&](FollowUpPlanEvent event, FollowUpPlanRequestState expected) {
        FollowUpTransition step;
        step.event = event;
        step.expectedState = expected;
        return step;
    };

    const auto loaded = requests_.findById(id);
    if (!loaded.success()) return Result::failure(*loaded.error());
    if (!loaded.value().has_value()) return fail<FollowUpPlanRequest>("follow_up.not_found", "No such follow-up request");
    const FollowUpPlanRequest request = *loaded.value();
    if (request.state != FollowUpPlanRequestState::Authorized) {
        return fail<FollowUpPlanRequest>("follow_up.state_changed",
            std::string("The request is '") + toString(request.state) + "', not 'authorized'");
    }

    // 1. The ONE deadline. Checked FIRST, before a single source is read: an expired request is expired whatever state the sources,
    // the catalog or the loader are in.
    if (followUpFloorToMilliseconds(now()) >= request.quoteExpiresAt) {
        static_cast<void>(requests_.transition(id, transitionOf(FollowUpPlanEvent::Expire, request.state), now()));
        return fail<FollowUpPlanRequest>("follow_up.quote_expired", "The authorisation has expired");
    }

    // 2. The admission checks again, from the sources. A drift is free: no call is made.
    auto admitted = admitStoredFollowUpRequest(admissionPorts(), request);
    if (!admitted.success()) {
        const auto error = *admitted.error();
        if (isFollowUpStaleCode(error.code)) {
            static_cast<void>(requests_.transition(id, transitionOf(FollowUpPlanEvent::MarkStale, request.state), now()));
        }
        return Result::failure(error);
    }

    // 3. The claim: the one compare-and-set after which the call may be billed. Whoever loses it makes no call.
    auto claim = requests_.transition(id, transitionOf(FollowUpPlanEvent::Claim, request.state), now());
    if (!claim.success()) {
        if (claim.error()->code == "follow_up.quote_expired") {
            static_cast<void>(requests_.transition(id, transitionOf(FollowUpPlanEvent::Expire, request.state), now()));
        }
        return claim;
    }
    const FollowUpPlanRequest claimed = claim.value();
    const FollowUpAdmitted& value = admitted.value();
    const auto settle = [&](FollowUpTransition step) { return requests_.transition(id, std::move(step), now()); };

    // From here on the request is settled whatever happens; the first error of a secondary write is reported at the end.
    std::optional<common::Error> secondary;
    const auto remember = [&](const common::Result<ModelExecution>& written) {
        if (!written.success() && !secondary.has_value()) secondary = *written.error();
    };
    const auto settleFailed = [&](const std::string& code, const std::string& message, const std::string* reply,
                                  const FollowUpAccounting* accounting) {
        FollowUpTransition step = transitionOf(FollowUpPlanEvent::CompositionFailed, FollowUpPlanRequestState::Composing);
        step.failure = makeFailure(code, message, reply);
        if (accounting != nullptr) step.accounting = *accounting;
        return settle(std::move(step));
    };
    // The outcome of the pass. A failed write of the execution row is retried ONCE from the request's own accounting (the durable
    // copy, written in the same atomic step as the settlement); if that also fails the error is returned and the row is written by
    // reconcileAccounting at the next startup - the paid usage is never lost and the provider is never called again.
    const auto result = [&](const common::Result<FollowUpPlanRequest>& settled) {
        if (!settled.success()) return settled;
        if (secondary.has_value() && settled.value().accounting.has_value()) {
            const auto repaired = settleExecutionsOf(settled.value());
            if (repaired.success()) secondary.reset();
        }
        if (secondary.has_value()) return Result::failure(*secondary);
        return settled;
    };

    // 4. The execution row, written before the call: the money has a record even if everything after it goes wrong.
    ProviderChatRequest chat;
    chat.providerId = value.provider.id;
    chat.modelId = value.model.id;
    chat.apiModelId = value.model.apiModelId;
    chat.systemPrompt = value.context.systemPrompt;
    chat.userContent = value.context.userContent;
    chat.timeout = std::chrono::milliseconds {30000};
    // Exactly the stored effective limit and the stored cache policy, nothing recomputed (a later need for other values is a stale).
    chat.maxOutputTokens = static_cast<std::uint32_t>(claimed.quote.effectiveMaxOutputTokens());
    chat.temperature = 0.2;
    chat.disableTotalTimeout = true;
    chat.promptCache = claimed.quote.promptCache();

    CreateModelExecutionRequest create;
    create.source = ModelExecutionSource::FollowUp;
    create.sourceId = claimed.id;
    create.projectId = claimed.projectId;
    create.providerId = value.provider.id;
    create.modelId = value.model.id;
    create.role = "follow_up_composer";
    create.position = 0;
    create.attempt = 1;
    create.requestHash = sha256Hex(value.provider.id + "\n" + value.model.id + "\n" + value.model.apiModelId + "\n"
        + chat.systemPrompt + "\n" + chat.userContent);
    create.contextHash = value.context.contextFingerprint;
    const auto created = executions_.createExecution(create);
    if (!created.success()) {
        // No call was made, but the claim was: the request says so (it stays possibly charged by design).
        return settleFailed("follow_up.execution_log_failed", created.error()->message, nullptr, nullptr);
    }
    const ModelExecutionId executionId = created.value().id;
    const auto started = executions_.startExecution(executionId);
    if (!started.success()) {
        remember(executions_.finishExecution(executionId,
            finishOf(ModelExecutionStatus::Failed, nullptr, {}, started.error()->code, started.error()->message)));
        return settleFailed("follow_up.execution_log_failed", started.error()->message, nullptr, nullptr);
    }

    const auto settleCancelled = [&](const common::Result<ProviderChatResponse>* response, std::uint64_t latencyMs) {
        std::optional<FollowUpAccounting> accounting;
        if (response != nullptr && response->success()) {
            accounting = accountingOf(response->value().usage, computeSingleResponseCostMicros(value.model, response->value().usage), latencyMs);
        }
        remember(executions_.finishExecution(executionId,
            finishOf(ModelExecutionStatus::Cancelled, accounting.has_value() ? &*accounting : nullptr, {}, "provider.cancelled",
                "The composer call was stopped")));
        FollowUpTransition step = transitionOf(FollowUpPlanEvent::Cancel, FollowUpPlanRequestState::Composing);
        step.accounting = accounting;
        return result(settle(std::move(step)));
    };

    // A stop that arrived before the call prevents it.
    if (stop.stop_requested()) return settleCancelled(nullptr, 0);

    // 5. ONE turn through the provider.
    const auto client = clients_.clientFor(value.provider.id, value.provider.kind);
    const auto begun = std::chrono::steady_clock::now();
    common::Result<ProviderChatResponse> response = client.success()
        ? client.value()->send(chat, stop)
        : common::Result<ProviderChatResponse>::failure(*client.error());
    const auto latencyMs = static_cast<std::uint64_t>(std::max<std::int64_t>(0,
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begun).count()));

    if (stop.stop_requested()) return settleCancelled(&response, latencyMs);

    if (!response.success()) {
        const auto& error = *response.error();
        remember(executions_.finishExecution(executionId,
            finishOf(ModelExecutionStatus::Failed, nullptr, {}, error.code, error.message)));
        return result(settleFailed(error.code, error.message, nullptr, nullptr));
    }

    // 6. The cost of what the provider reported, before anything else about the answer.
    const ProviderChatResponse& reply = response.value();
    const FollowUpAccounting accounting = accountingOf(reply.usage, computeSingleResponseCostMicros(value.model, reply.usage), latencyMs);
    remember(executions_.finishExecution(executionId,
        finishOf(ModelExecutionStatus::Completed, &accounting, reply.content, {}, {})));

    // 7. The answer. ONE envelope, a strict parse, a plan inside the authorised window - or a stored, typed failure. Either way the
    // accounting of the paid response goes into the SAME atomic step.
    if (reply.content.empty() || reply.content.size() > kFollowUpMaxReplyBytes || !storableText(reply.content)) {
        return result(settleFailed("follow_up.reply_not_storable",
            "The provider's reply is empty, over 256 KiB or not valid text, so it cannot be stored as an answer", &reply.content,
            &accounting));
    }
    const auto json = extractNextRunPlanJsonFromMessage(reply.content);
    if (!json.success()) return result(settleFailed(json.error()->code, json.error()->message, &reply.content, &accounting));
    const auto plan = parseNextRunPlan(json.value());
    if (!plan.success()) return result(settleFailed(plan.error()->code, plan.error()->message, &reply.content, &accounting));
    const auto window = checkFollowUpPlanAgainstAuthorisation(plan.value(), claimed.splitHint, claimed.maxDrafts);
    if (!window.success()) return result(settleFailed(window.error()->code, window.error()->message, &reply.content, &accounting));

    FollowUpPlanAnswer answer;
    answer.rawReply = reply.content;
    answer.planFingerprint = plan.value().fingerprint;
    answer.advisoriesJson = serializeFollowUpPlanAdvisories(
        computeFollowUpPlanAdvisories(plan.value(), value.material.snapshot, claimed.goal, value.context.userContent),
        kFollowUpMaxAdvisoriesBytes);
    FollowUpTransition ready = transitionOf(FollowUpPlanEvent::PlanSucceeded, FollowUpPlanRequestState::Composing);
    ready.answer = std::move(answer);
    ready.accounting = accounting;
    auto stored = settle(std::move(ready));
    if (!stored.success()) {
        // The answer was refused by the request's own rules (it should not have been): keep it as a typed failure rather than
        // leaving a paid call without an outcome. If the write itself is what failed, this fails the same way and is reported.
        const auto cause = *stored.error();
        auto failed = settleFailed(cause.code, cause.message, &reply.content, &accounting);
        if (!failed.success()) return failed;
        return result(failed);
    }
    return result(stored);
}

common::Result<std::size_t> FollowUpComposerProcessor::settleExecutionsOf(const FollowUpPlanRequest& request)
{
    using Result = common::Result<std::size_t>;
    if (!request.accounting.has_value()) return Result::ok(0);
    const auto rows = executions_.listExecutions(ModelExecutionSource::FollowUp, request.id);
    if (!rows.success()) return Result::failure(*rows.error());
    std::size_t written = 0;
    std::optional<common::Error> first;
    for (const auto& row : rows.value()) {
        if (row.status != ModelExecutionStatus::Pending && row.status != ModelExecutionStatus::Running) continue;
        // A row that never started cannot be completed: start it first (the call it records did happen).
        if (row.status == ModelExecutionStatus::Pending) {
            const auto started = executions_.startExecution(row.id);
            if (!started.success()) {
                if (!first.has_value()) first = *started.error();
                continue;
            }
        }
        const auto done = executions_.finishExecution(row.id, finishOfRequest(request));
        if (done.success()) ++written;
        else if (!first.has_value()) first = *done.error();
    }
    if (first.has_value()) return Result::failure(*first);
    return Result::ok(written);
}

common::Result<std::size_t> FollowUpComposerProcessor::reconcileAccounting()
{
    using Result = common::Result<std::size_t>;
    if (refusal_.has_value()) return Result::failure(*refusal_);
    const auto unfinished = executions_.listUnfinishedExecutions();
    if (!unfinished.success()) return Result::failure(*unfinished.error());
    std::set<FollowUpPlanRequestId> ids;
    for (const auto& row : unfinished.value()) {
        if (row.source == ModelExecutionSource::FollowUp) ids.insert(row.sourceId);
    }
    std::size_t written = 0;
    std::optional<common::Error> first;
    for (const auto& id : ids) {
        const auto found = requests_.findById(id);
        if (!found.success()) {
            if (!first.has_value()) first = *found.error();
            continue;
        }
        // No request, or one that holds no accounting (its call was never answered, or is still in flight): not ours to settle -
        // the generic recovery of orphaned executions covers it.
        if (!found.value().has_value() || !found.value()->accounting.has_value()) continue;
        const auto done = settleExecutionsOf(*found.value());
        if (done.success()) written += done.value();
        else if (!first.has_value()) first = *done.error();
    }
    if (first.has_value()) return Result::failure(*first);
    return Result::ok(written);
}

common::Result<FollowUpComposerProcessor::RecoveryReport> FollowUpComposerProcessor::recoverAtStartup()
{
    using Result = common::Result<RecoveryReport>;
    if (refusal_.has_value()) return Result::failure(*refusal_);
    RecoveryReport report;

    // EVERY request of a state, one page at a time. The cursor is the key of the last row of the previous page, so each page starts
    // strictly after it: the walk makes progress whether or not the rows leave the state, and ends at the first short page.
    const auto walk = [&](FollowUpPlanRequestState state,
                          const std::function<common::Result<void>(const FollowUpPlanRequest&)>& visit) -> common::Result<void> {
        std::optional<FollowUpStateCursor> cursor;
        for (;;) {
            const auto page = requests_.listByStateAfter(state, kRecoveryPageSize, cursor);
            if (!page.success()) return common::Result<void>::failure(*page.error());
            for (const auto& request : page.value()) {
                const auto visited = visit(request);
                if (!visited.success()) return visited;
            }
            if (page.value().size() < kRecoveryPageSize) return common::Result<void>::ok();
            cursor = FollowUpStateCursor {page.value().back().createdAt, page.value().back().id};
        }
    };

    // An orphaned `composing` request: the process that held the call is gone. It is NEVER called again; it is settled as interrupted
    // (possibly charged - whatever the execution log holds is its cost).
    const auto interrupted = walk(FollowUpPlanRequestState::Composing, [&](const FollowUpPlanRequest& request) {
        FollowUpTransition step;
        step.event = FollowUpPlanEvent::Interrupt;
        step.expectedState = FollowUpPlanRequestState::Composing;
        const auto done = requests_.transition(request.id, std::move(step), now());
        if (done.success()) {
            ++report.interrupted;
        } else if (done.error()->code != "follow_up.state_changed") {
            return common::Result<void>::failure(*done.error());
        }
        return common::Result<void>::ok();
    });
    if (!interrupted.success()) return Result::failure(*interrupted.error());

    // This source is deliberately excluded from ModelExecutionService's generic startup
    // sweep. A paid response already stored on its request must restore the exact terminal
    // outcome and accounting; without such a response, the abandoned row is an ordinary
    // interruption with zero usage. Try every row and keep the first error so a later startup
    // can retry any row that could not be written.
    const auto unfinished = executions_.listUnfinishedExecutions();
    if (!unfinished.success()) {
        report.executionError = *unfinished.error();
    } else {
        std::set<FollowUpPlanRequestId> repairedRequests;
        for (const auto& row : unfinished.value()) {
            if (row.source != ModelExecutionSource::FollowUp) continue;
            const auto found = requests_.findById(row.sourceId);
            if (!found.success()) {
                if (!report.executionError.has_value()) report.executionError = *found.error();
                continue;
            }
            if (!found.value().has_value()) {
                if (!report.executionError.has_value()) {
                    report.executionError = common::Error {
                        "persistence.invalid_data",
                        "An unfinished follow-up execution has no source request"};
                }
                continue;
            }
            if (found.value()->accounting.has_value()) {
                if (!repairedRequests.insert(row.sourceId).second) continue;
                const auto repaired = settleExecutionsOf(*found.value());
                if (repaired.success()) report.accountingRepaired += repaired.value();
                else if (!report.accountingError.has_value()) report.accountingError = *repaired.error();
                continue;
            }
            const auto closed = executions_.finishExecution(row.id,
                finishOf(ModelExecutionStatus::Failed, nullptr, {}, kModelExecutionInterruptedErrorCode,
                    "Recovered after an interrupted application session"));
            if (closed.success()) ++report.executionsInterrupted;
            else if (!report.executionError.has_value()) report.executionError = *closed.error();
        }
    }

    const auto authorized = walk(FollowUpPlanRequestState::Authorized, [&](const FollowUpPlanRequest& request) {
        if (followUpFloorToMilliseconds(now()) >= request.quoteExpiresAt) {
            FollowUpTransition step;
            step.event = FollowUpPlanEvent::Expire;
            step.expectedState = FollowUpPlanRequestState::Authorized;
            const auto done = requests_.transition(request.id, std::move(step), now());
            if (done.success()) {
                ++report.expired;
            } else if (done.error()->code != "follow_up.state_changed") {
                return common::Result<void>::failure(*done.error());
            }
        } else {
            report.toProcess.push_back(request.id);
        }
        return common::Result<void>::ok();
    });
    if (!authorized.success()) return Result::failure(*authorized.error());

    return Result::ok(std::move(report));
}

// ---- The worker -------------------------------------------------------------------------------------------------------------

FollowUpComposerRunner::FollowUpComposerRunner(FollowUpComposerProcessor& processor)
    : processor_(processor)
{
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

FollowUpComposerRunner::~FollowUpComposerRunner()
{
    // jthread requests the stop token of the running call and joins; the queue is dropped (an authorized request not yet claimed is
    // picked up by the next startup pass, which claims it once).
    worker_.request_stop();
}

void FollowUpComposerRunner::setErrorSink(std::function<void(const common::Error&)> sink)
{
    std::lock_guard lock(sinkMutex_);
    errorSink_ = std::move(sink);
}

void FollowUpComposerRunner::report(const common::Error& error)
{
    // A request that moved on (cancelled, claimed elsewhere, already in the call) is the expected outcome of a race, not an error.
    if (error.code == "follow_up.state_changed" || error.code == "follow_up.already_running") return;
    std::function<void(const common::Error&)> sink;
    {
        std::lock_guard lock(sinkMutex_);
        sink = errorSink_;
    }
    if (sink) sink(error);
}

void FollowUpComposerRunner::nudge(const FollowUpPlanRequestId& id)
{
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(Job {false, id});
    }
    wake_.notify_one();
}

void FollowUpComposerRunner::recoverAtStartup()
{
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(Job {true, {}});
    }
    wake_.notify_one();
}

bool FollowUpComposerRunner::requestStop(const FollowUpPlanRequestId& id)
{
    return processor_.requestStop(id);
}

void FollowUpComposerRunner::waitUntilIdle()
{
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return queue_.empty() && !busy_; });
}

void FollowUpComposerRunner::run(std::stop_token stop)
{
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            if (!wake_.wait(lock, stop, [this] { return !queue_.empty(); })) return;
            job = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
        }
        if (job.recovery) {
            const auto recovered = processor_.recoverAtStartup();
            if (!recovered.success()) {
                report(*recovered.error());
            } else {
                if (recovered.value().accountingError.has_value()) report(*recovered.value().accountingError);
                if (recovered.value().executionError.has_value()) report(*recovered.value().executionError);
                for (const auto& id : recovered.value().toProcess) {
                    if (stop.stop_requested()) break;
                    const auto processed = processor_.processAuthorized(id, stop);
                    if (!processed.success()) report(*processed.error());
                }
            }
        } else {
            const auto processed = processor_.processAuthorized(job.id, stop);
            if (!processed.success()) report(*processed.error());
        }
        {
            std::lock_guard lock(mutex_);
            busy_ = false;
        }
        idle_.notify_all();
    }
}

} // namespace ai::application
