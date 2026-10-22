#include "ai/application/MockWorkflowRunner.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace ai::application;
using namespace std::chrono_literals;

void check(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

// A hung test must not hang the build: the watchdogs below end the process, they never decide whether a test passed (that is the state
// of the gate, the events and the result).
[[noreturn]] void failFast(const std::string& message)
{
    std::fprintf(stderr, "%s\n", message.c_str());
    std::fflush(stderr);
    std::_Exit(1);
}

const char* typeName(RunEventType type)
{
    switch (type) {
    case RunEventType::StageStarted: return "StageStarted";
    case RunEventType::StageProgress: return "StageProgress";
    case RunEventType::StageCompleted: return "StageCompleted";
    case RunEventType::StageCancelled: return "StageCancelled";
    default: return "Other";
    }
}

// One line per event, exactly the fields the Mock workflow fills in. The doubles it produces here are all dyadic fractions
// (halves and quarters), so three decimals are exact.
std::string describe(const RunEvent& event)
{
    std::ostringstream line;
    const std::string stage = event.stageId.has_value() ? event.stageId->substr(event.stageId->find(':') + 1) : std::string("-");
    line << typeName(event.type) << ' ' << stage << std::fixed << std::setprecision(3) << " run=" << event.runProgress
         << " stage=" << event.stageProgress << " usage=" << event.usage.inputTokens << '/' << event.usage.outputTokens << " '"
         << event.message << '\'';
    return line.str();
}

// Thread-safe: a gate test publishes from the runner's thread and reads from the test's.
class RecordingSink final : public IRunEventSink {
public:
    void publish(const RunEvent& event) override
    {
        std::function<void(const RunEvent&, std::size_t)> callback;
        std::size_t count = 0;
        {
            std::lock_guard lock(mutex_);
            lines_.push_back(describe(event));
            count = lines_.size();
            callback = onEvent;
        }
        if (callback) callback(event, count);
    }
    [[nodiscard]] std::vector<std::string> lines() const
    {
        std::lock_guard lock(mutex_);
        return lines_;
    }
    std::function<void(const RunEvent&, std::size_t)> onEvent;   // set before the run starts; called after the event has been recorded

private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

WorkflowRunContext contextOf(const MockWorkflowRunner& runner)
{
    WorkflowRunContext context;
    context.run.id = "run-pin";
    context.project.id = "project-1";
    context.stages = runner.createStages(context.run.id, ResolvedExecutionPlan {});
    return context;
}

// The sequence of events of an uninterrupted Mock run with two steps per stage. Pinned before the gate seam existed (the test below
// passed against the runner as it was) and kept as it was.
std::vector<std::string> theUninterruptedSequence()
{
    return {
        "StageStarted prepare-context run=0.000 stage=0.000 usage=0/0 'Prepare context started'",
        "StageProgress prepare-context run=0.125 stage=0.500 usage=0/0 ''",
        "StageProgress prepare-context run=0.250 stage=1.000 usage=0/0 ''",
        "StageCompleted prepare-context run=0.250 stage=1.000 usage=10/5 'Prepare context completed'",
        "StageStarted architect run=0.250 stage=0.000 usage=0/0 'Architecture started'",
        "StageProgress architect run=0.375 stage=0.500 usage=0/0 ''",
        "StageProgress architect run=0.500 stage=1.000 usage=0/0 ''",
        "StageCompleted architect run=0.500 stage=1.000 usage=10/5 'Architecture completed'",
        "StageStarted implementation run=0.500 stage=0.000 usage=0/0 'Implementation started'",
        "StageProgress implementation run=0.625 stage=0.500 usage=0/0 ''",
        "StageProgress implementation run=0.750 stage=1.000 usage=0/0 ''",
        "StageCompleted implementation run=0.750 stage=1.000 usage=10/5 'Implementation completed'",
        "StageStarted documentation run=0.750 stage=0.000 usage=0/0 'Documentation started'",
        "StageProgress documentation run=0.875 stage=0.500 usage=0/0 ''",
        "StageProgress documentation run=1.000 stage=1.000 usage=0/0 ''",
        "StageCompleted documentation run=1.000 stage=1.000 usage=10/5 'Documentation completed'",
    };
}

// ---- the sequence of events of the Mock workflow WITHOUT a gate ---------------------------------------------------------------

void testAnUninterruptedRunPublishesTheSequenceItAlwaysDid()
{
    MockWorkflowRunner runner(1ms, 2);
    RecordingSink sink;
    const auto context = contextOf(runner);
    const WorkflowResult result = runner.run(context, std::stop_token {}, sink);

    check(sink.lines() == theUninterruptedSequence(), "the uninterrupted sequence of events must be the one the Mock workflow always published");
    check(result.success && result.finalStatus == RunStatus::Completed && result.message == "Mock workflow completed"
            && result.usage.inputTokens == 40 && result.usage.outputTokens == 20,
        "...and the result is a completed run with the summed usage");
    check(result.finalOutput.rfind("# Mock Workflow Report", 0) == 0 && result.finalOutput.find("Stages completed: 4") != std::string::npos,
        "...and the report");
}

void testACancellationBeforeTheFirstStepIsPublishedAtTheStageBoundary()
{
    MockWorkflowRunner runner(1ms, 2);
    RecordingSink sink;
    std::stop_source source;
    source.request_stop();
    const WorkflowResult result = runner.run(contextOf(runner), source.get_token(), sink);
    check(sink.lines() == std::vector<std::string> {"StageCancelled prepare-context run=0.000 stage=0.000 usage=0/0 'Mock workflow cancelled'"},
        "a stop requested before the run starts is one StageCancelled at the first stage boundary");
    check(!result.success && result.finalStatus == RunStatus::Cancelled && result.message == "Mock workflow cancelled",
        "...and a Cancelled result");
}

void testACancellationDuringAStepIsPublishedWithTheStepProgress()
{
    {
        MockWorkflowRunner runner(1ms, 2);
        RecordingSink sink;
        std::stop_source source;
        sink.onEvent = [&](const RunEvent& event, std::size_t) {
            if (event.type == RunEventType::StageStarted) source.request_stop();
        };
        const WorkflowResult result = runner.run(contextOf(runner), source.get_token(), sink);
        check(sink.lines()
                == std::vector<std::string> {
                    "StageStarted prepare-context run=0.000 stage=0.000 usage=0/0 'Prepare context started'",
                    "StageCancelled prepare-context run=0.000 stage=0.000 usage=0/0 'Prepare context cancelled'"},
            "a stop requested after the stage started is noticed after the first step's delay: StageCancelled with the step's progress");
        check(!result.success && result.finalStatus == RunStatus::Cancelled && result.message == "Mock workflow cancelled", "...Cancelled");
    }
    {
        MockWorkflowRunner runner(1ms, 2);
        RecordingSink sink;
        std::stop_source source;
        sink.onEvent = [&](const RunEvent& event, std::size_t) {
            if (event.type == RunEventType::StageProgress) source.request_stop();
        };
        const WorkflowResult result = runner.run(contextOf(runner), source.get_token(), sink);
        check(sink.lines()
                == std::vector<std::string> {
                    "StageStarted prepare-context run=0.000 stage=0.000 usage=0/0 'Prepare context started'",
                    "StageProgress prepare-context run=0.125 stage=0.500 usage=0/0 ''",
                    "StageCancelled prepare-context run=0.125 stage=0.500 usage=0/0 'Prepare context cancelled'"},
            "a stop requested after the first progress is noticed in the second step, with half of the stage done");
        check(!result.success && result.finalStatus == RunStatus::Cancelled, "...Cancelled");
    }
}

// ---- the gate ---------------------------------------------------------------------------------------------------------------------

// Records every call into the same log the events go to, so that the ORDER of the two is visible.
class OrderingGate final : public IMockWorkflowGate {
public:
    explicit OrderingGate(RecordingSink& sink) : sink_(sink) {}
    void beforeStep(const RunId& runId, std::size_t stageIndex, std::size_t step, std::stop_token) override
    {
        std::lock_guard lock(mutex_);
        calls_.push_back(runId + " " + std::to_string(stageIndex) + " " + std::to_string(step));
        eventsSeenByTheCall_.push_back(sink_.lines().size());
    }
    [[nodiscard]] std::vector<std::string> calls() const
    {
        std::lock_guard lock(mutex_);
        return calls_;
    }
    [[nodiscard]] std::vector<std::size_t> eventsSeenByTheCall() const
    {
        std::lock_guard lock(mutex_);
        return eventsSeenByTheCall_;
    }

private:
    RecordingSink& sink_;
    mutable std::mutex mutex_;
    std::vector<std::string> calls_;
    std::vector<std::size_t> eventsSeenByTheCall_;
};

void testAGateThatNeverBlocksChangesNothingAndIsCalledOncePerStepAfterTheStageStarted()
{
    RecordingSink sink;
    auto gate = std::make_shared<OrderingGate>(sink);
    MockWorkflowRunner runner(1ms, 2, nullptr, gate);
    const WorkflowResult result = runner.run(contextOf(runner), std::stop_token {}, sink);

    check(sink.lines() == theUninterruptedSequence(), "a gate that never blocks leaves the events exactly as they were");
    check(result.success && result.finalStatus == RunStatus::Completed, "...and the run completes");
    const std::vector<std::string> expectedCalls = {"run-pin 0 1", "run-pin 0 2", "run-pin 1 1", "run-pin 1 2",
        "run-pin 2 1", "run-pin 2 2", "run-pin 3 1", "run-pin 3 2"};
    check(gate->calls() == expectedCalls, "the gate is called once per step, with the run, the stage and the step");
    // How many events there were when each call was made: StageStarted (1) before the first step of a stage, then one progress per
    // step and a StageCompleted per stage - so the calls see 1, 2, 5, 6, 9, 10, 13, 14 events.
    const std::vector<std::size_t> expectedSeen = {1, 2, 5, 6, 9, 10, 13, 14};
    check(gate->eventsSeenByTheCall() == expectedSeen,
        "every call comes AFTER the stage's StageStarted and BEFORE the step's own progress event");
}

// A gate that holds the run at its first step until it is released - or until a stop is requested.
class BlockingGate final : public IMockWorkflowGate {
public:
    void beforeStep(const RunId&, std::size_t, std::size_t, std::stop_token stopToken) override
    {
        std::unique_lock lock(mutex_);
        ++calls_;
        arrived_ = true;
        changed_.notify_all();
        if (released_) return;
        changed_.wait(lock, stopToken, [this] { return released_; });
    }
    void release()
    {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }
    // Watchdog only: returns when the runner has reached the gate; ends the process if it never does.
    void waitUntilArrived()
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 30s, [this] { return arrived_; })) failFast("watchdog: the runner never reached the gate");
    }
    [[nodiscard]] int calls() const
    {
        std::lock_guard lock(mutex_);
        return calls_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    bool arrived_ {false};
    bool released_ {false};
    int calls_ {0};
};

void testABlockedGateHoldsTheRunAtItsFirstStepUntilItIsReleased()
{
    RecordingSink sink;
    auto gate = std::make_shared<BlockingGate>();
    MockWorkflowRunner runner(1ms, 2, nullptr, gate);
    std::promise<WorkflowResult> finished;
    auto outcome = finished.get_future();
    std::thread worker([&] { finished.set_value(runner.run(contextOf(runner), std::stop_token {}, sink)); });

    gate->waitUntilArrived();
    check(sink.lines() == std::vector<std::string> {"StageStarted prepare-context run=0.000 stage=0.000 usage=0/0 'Prepare context started'"},
        "held at the gate: the stage has started and nothing else has happened - no progress, no completion");
    check(outcome.wait_for(0ms) != std::future_status::ready, "...and the run has not finished");
    check(gate->calls() == 1, "...in one call");

    gate->release();
    if (outcome.wait_for(30s) != std::future_status::ready) failFast("watchdog: the released run never finished");
    worker.join();
    const WorkflowResult result = outcome.get();
    check(sink.lines() == theUninterruptedSequence() && result.success && result.finalStatus == RunStatus::Completed,
        "released, the run goes on to complete with the events it always published");
}

// Runs `work` on its own thread and ends the process if it is still running after the watchdog: a runner that waited out a step of an
// hour would otherwise hang the test instead of failing it.
template <typename Work>
auto runWithWatchdog(const char* what, Work work)
{
    std::promise<decltype(work())> promise;
    auto future = promise.get_future();
    std::thread thread([&] { promise.set_value(work()); });
    if (future.wait_for(30s) != std::future_status::ready) failFast(std::string("watchdog: ") + what);
    thread.join();
    return future.get();
}

void testACancellationReleasesTheGateAndTheRunDoesNotWaitOutAnotherStep()
{
    RecordingSink sink;
    auto gate = std::make_shared<BlockingGate>();
    // A step that lasts an hour: if the runner slept after the gate returned, the thread below would still be sleeping.
    MockWorkflowRunner runner(std::chrono::hours(1), 2, nullptr, gate);
    std::stop_source source;
    std::promise<WorkflowResult> finished;
    auto outcome = finished.get_future();
    std::thread worker([&] { finished.set_value(runner.run(contextOf(runner), source.get_token(), sink)); });

    gate->waitUntilArrived();
    check(sink.lines().size() == 1, "setup: held at the gate");
    source.request_stop();
    if (outcome.wait_for(30s) != std::future_status::ready) failFast("watchdog: the runner waited out a step after the gate was released by the stop");
    worker.join();
    const WorkflowResult result = outcome.get();
    check(sink.lines()
            == std::vector<std::string> {
                "StageStarted prepare-context run=0.000 stage=0.000 usage=0/0 'Prepare context started'",
                "StageCancelled prepare-context run=0.000 stage=0.000 usage=0/0 'Prepare context cancelled'"},
        "a stop releases the gate and the run is cancelled at once, with the step's progress");
    check(!result.success && result.finalStatus == RunStatus::Cancelled && result.message == "Mock workflow cancelled", "...Cancelled");
}

// A gate that asks for the stop itself and returns without blocking.
class StoppingGate final : public IMockWorkflowGate {
public:
    explicit StoppingGate(std::stop_source& source) : source_(source) {}
    void beforeStep(const RunId&, std::size_t, std::size_t, std::stop_token) override
    {
        ++calls;
        source_.request_stop();
    }
    std::atomic<int> calls {0};

private:
    std::stop_source& source_;
};

void testAStopRequestedByTheGateItselfIsHonouredBeforeTheStepsDelay()
{
    RecordingSink sink;
    std::stop_source source;
    auto gate = std::make_shared<StoppingGate>(source);
    MockWorkflowRunner runner(std::chrono::hours(1), 2, nullptr, gate);
    const WorkflowResult result = runWithWatchdog("the runner slept through the step after the gate asked for the stop",
        [&] { return runner.run(contextOf(runner), source.get_token(), sink); });
    check(gate->calls == 1, "the gate was called once");
    check(sink.lines()
            == std::vector<std::string> {
                "StageStarted prepare-context run=0.000 stage=0.000 usage=0/0 'Prepare context started'",
                "StageCancelled prepare-context run=0.000 stage=0.000 usage=0/0 'Prepare context cancelled'"},
        "a stop found as the gate returns is published as the cancellation of that step, with no delay");
    check(!result.success && result.finalStatus == RunStatus::Cancelled, "...Cancelled");
}

void testAStopRequestedBeforeTheRunNeverReachesTheGate()
{
    RecordingSink sink;
    auto gate = std::make_shared<OrderingGate>(sink);
    MockWorkflowRunner runner(1ms, 2, nullptr, gate);
    std::stop_source source;
    source.request_stop();
    const WorkflowResult result = runner.run(contextOf(runner), source.get_token(), sink);
    check(gate->calls().empty(), "the stage boundary cancels before the stage starts, so the gate is never called");
    check(sink.lines() == std::vector<std::string> {"StageCancelled prepare-context run=0.000 stage=0.000 usage=0/0 'Mock workflow cancelled'"}
            && !result.success && result.finalStatus == RunStatus::Cancelled,
        "...and the events are those of a run without a gate");
}

} // namespace

int main()
{
    try {
        testAnUninterruptedRunPublishesTheSequenceItAlwaysDid();
        testACancellationBeforeTheFirstStepIsPublishedAtTheStageBoundary();
        testACancellationDuringAStepIsPublishedWithTheStepProgress();
        testAGateThatNeverBlocksChangesNothingAndIsCalledOncePerStepAfterTheStageStarted();
        testABlockedGateHoldsTheRunAtItsFirstStepUntilItIsReleased();
        testACancellationReleasesTheGateAndTheRunDoesNotWaitOutAnotherStep();
        testAStopRequestedByTheGateItselfIsHonouredBeforeTheStepsDelay();
        testAStopRequestedBeforeTheRunNeverReachesTheGate();
        std::cout << "MockWorkflowRunner_tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
