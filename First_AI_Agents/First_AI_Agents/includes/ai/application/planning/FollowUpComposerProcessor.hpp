#pragma once

#include "ai/application/planning/FollowUpAdmission.hpp"
#include "ai/application/planning/FollowUpPlanRequest.hpp"
#include "ai/application/planning/IFollowUpPlanRequestRepository.hpp"
#include "ai/application/IProviderClient.hpp"
#include "ai/application/agents/ModelExecutionService.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace ai::application {

// PC-5C: the PAID half of the opt-in composer. It handles ONLY a request that is already `authorized` - it never creates, quotes or
// authorises one - and it makes at most ONE provider call per request, ever: a request is claimed once (a compare-and-set), and a
// retry is a NEW, explicitly authorised request. Nothing is re-paid silently.
//
// For one authorized request, processAuthorized does, in this order:
//   1. the 15-minute deadline (a request past it becomes `expired`, free);
//   2. the admission checks AGAIN, from the sources, assembling the provider bytes itself (a drift is a free `stale`);
//   3. the CLAIM (authorized -> composing). From here on the call may be billed;
//   4. an execution row under source `follow_up` (source id = the request id, role `follow_up_composer`, no link to the source Run),
//      so the cost is visible in the project's analytics and on the request and never changes the cost of the source Run;
//   5. ONE turn through IProviderClient::send - no tools, no streaming - with exactly the stored effective output limit and the
//      stored cache policy;
//   6. the execution row is settled with the provider's usage and the price of THAT usage (the cost is written before anything else
//      about the answer). If that write fails the paid usage is NOT lost: it is also stored in the request (step 7), and
//      reconcileAccounting writes the row again from there, now and at every startup, with no second provider call;
//   7. the answer: ONE valid `next-run-plan.v1` of the authorised size is stored immutably with its advisories - together with the
//      accounting of the call, in the same atomic step - and the request is
//      `plan_ready`; anything else (a provider failure, no / several envelopes, a parse error, a plan outside the draft window, a reply
//      that cannot be stored) is stored as a `failed` request with the typed error, and the reply when it is storable text;
//      a stop requested during the call makes the request `cancelled` (possibly charged).
//
// A GUEST runtime (another process owns the database) is READ-NOTHING and WRITE-NOTHING: with a `refusal` every entry fails closed
// with it (follow_up.not_the_owner) before it reads, writes or calls anything.
class FollowUpComposerProcessor final {
public:
    using Clock = std::function<std::chrono::system_clock::time_point()>;

    struct RecoveryReport {
        std::size_t interrupted {0};   // orphaned `composing` requests (possibly charged, never re-paid)
        std::size_t expired {0};       // old `authorized` requests (free)
        // Executions of settled requests whose log write had failed and was written again from the request's own accounting.
        std::size_t accountingRepaired {0};
        // Follow-up executions for which no provider response was durably recorded. They are
        // closed as interrupted by this owner, never by generic execution recovery.
        std::size_t executionsInterrupted {0};
        // A row of that repair that could not be written (the first one); the pass itself still completed.
        std::optional<common::Error> accountingError;
        // A no-response row that could not be closed (the first one); the pass still visits
        // the others, and a later startup retries it.
        std::optional<common::Error> executionError;
        // Unexpired `authorized` requests, oldest first: the owner claims each ONCE.
        std::vector<FollowUpPlanRequestId> toProcess;
    };

    FollowUpComposerProcessor(
        IFollowUpPlanRequestRepository& requests,
        const IFollowUpSourceReader& source,
        const IProviderRepository& providers,
        const IModelRepository& models,
        IProviderClientFactory& clients,
        ModelExecutionService& executions,
        Clock clock = {},
        std::optional<common::Error> refusal = std::nullopt);

    // Drives ONE authorized request as described above. Success = the request was settled by this call (`plan_ready`, `failed` or
    // `cancelled`); the returned request is the stored one. Failure leaves the request as follows:
    //   follow_up.stale_* / follow_up.quote_expired  the request was marked stale / expired (free), no call was made
    //   follow_up.state_changed                       the request is no longer authorized (cancelled, claimed elsewhere): nothing done
    //   a fault (persistence, a broken loader)        the request is left alone, no call was made
    //   a failed write AFTER the call                 the error of that write; the call was made and paid
    [[nodiscard]] common::Result<FollowUpPlanRequest> processAuthorized(const FollowUpPlanRequestId& id, std::stop_token stop = {});

    // Startup pass over EVERY request of the two states, page by page with a cursor (there is no row-count ceiling and every page
    // makes progress): an orphaned `composing` request becomes `interrupted`, an `authorized` request past its deadline becomes
    // `expired`; every unfinished follow-up execution is then owned here: a settled request with accounting restores its exact
    // paid outcome, while a request without accounting is failed as `model_execution.interrupted`. Generic model-execution recovery
    // deliberately skips this source. The live `authorized` ones are reported for the owner to process once. It calls no provider
    // itself.
    [[nodiscard]] common::Result<RecoveryReport> recoverAtStartup();

    // The durable way to write the cost of a paid answer into the execution log WITHOUT a second provider call. A request settled
    // after a provider response carries that response's usage and price (its `accounting`, written atomically with the state
    // change); when the log write of the execution had failed, the row is still Running. For every such row this finishes it from
    // the request's own accounting. Idempotent and safe to call at any time: it only touches rows that are still unfinished AND whose
    // request already holds an accounting (so never one whose call is still in flight). Returns how many rows it wrote; a row that
    // cannot be written is reported (the first error) after the others were tried.
    [[nodiscard]] common::Result<std::size_t> reconcileAccounting();

    // Asks the call that is running for this id to stop; true when there was one (it had registered before its claim, so a stop
    // that arrives between the claim and the provider call also prevents the call). The request is settled by processAuthorized.
    bool requestStop(const FollowUpPlanRequestId& id);

private:
    [[nodiscard]] std::chrono::system_clock::time_point now() const;
    [[nodiscard]] FollowUpAdmissionPorts admissionPorts() const;
    [[nodiscard]] common::Result<FollowUpPlanRequest> drive(const FollowUpPlanRequestId& id, std::stop_token stop);
    // Writes the unfinished execution rows of ONE settled request from its accounting; the number of rows written.
    [[nodiscard]] common::Result<std::size_t> settleExecutionsOf(const FollowUpPlanRequest& request);

    IFollowUpPlanRequestRepository& requests_;
    const IFollowUpSourceReader& source_;
    const IProviderRepository& providers_;
    const IModelRepository& models_;
    IProviderClientFactory& clients_;
    ModelExecutionService& executions_;
    Clock clock_;
    std::optional<common::Error> refusal_;

    std::mutex activeMutex_;
    std::map<FollowUpPlanRequestId, std::stop_source> active_;
};

// The owner's worker: one thread that processes authorized requests one at a time, in the order they were nudged. It is a thin shell
// around the processor (which is where every rule lives and is tested synchronously).
class FollowUpComposerRunner final {
public:
    explicit FollowUpComposerRunner(FollowUpComposerProcessor& processor);
    // Stops the worker; a call that is running is asked to stop.
    ~FollowUpComposerRunner();

    FollowUpComposerRunner(const FollowUpComposerRunner&) = delete;
    FollowUpComposerRunner& operator=(const FollowUpComposerRunner&) = delete;

    // The service calls this after an authorisation.
    void nudge(const FollowUpPlanRequestId& id);
    // Queues the startup pass: it interrupts orphans, expires old requests, repairs unwritten cost records and then processes each
    // live one once.
    void recoverAtStartup();
    // Every error a pass returns (a failed write after a paid call, a refused claim, a failed recovery) reaches this sink, called on
    // the worker thread; without a sink it is dropped. The expected refusals of a request that moved on (`state_changed`) are not
    // errors and are not reported.
    void setErrorSink(std::function<void(const common::Error&)> sink);
    bool requestStop(const FollowUpPlanRequestId& id);
    // Blocks until the queue is empty and nothing is running (for tests and orderly shutdown).
    void waitUntilIdle();

private:
    struct Job {
        bool recovery {false};
        FollowUpPlanRequestId id;
    };

    void run(std::stop_token stop);
    void report(const common::Error& error);

    FollowUpComposerProcessor& processor_;
    std::mutex sinkMutex_;
    std::function<void(const common::Error&)> errorSink_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::condition_variable idle_;
    std::deque<Job> queue_;
    bool busy_ {false};
    std::jthread worker_;   // last: it starts in the constructor body and must stop before the members above go
};

} // namespace ai::application
