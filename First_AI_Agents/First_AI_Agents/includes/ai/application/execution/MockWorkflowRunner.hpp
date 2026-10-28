#pragma once

#include "ai/application/execution/IWorkflowRunner.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <stop_token>

namespace ai::application {

class ArtifactService;

/**
 * TEST-ONLY seam of the Mock workflow: a barrier a test can hold a Run at, instead of racing the Run's own clock.
 *
 * MockWorkflowRunner calls beforeStep() once per step, AFTER the stage's StageStarted event has been published and BEFORE the
 * step's delay. A test gate may block there - the Run is then certainly Running and cannot finish - but it MUST return promptly when
 * `stopToken` requests a stop. A runner that finds a stop requested as the gate returns cancels at once: it does not wait out one
 * more step. Without a gate (the default, and the only thing any production composition root passes) the runner behaves exactly as it always did.
 * The interface has no dependency on Qt and no production implementation.
 */
class IMockWorkflowGate {
public:
    virtual ~IMockWorkflowGate() = default;
    virtual void beforeStep(const RunId& runId, std::size_t stageIndex, std::size_t step, std::stop_token stopToken) = 0;
};

/**
 * Deterministic four-stage workflow used to prove the complete application
 * lifecycle without API keys, network variability, or provider cost.
 *
 * When constructed with an ArtifactService, a successful run writes at least
 * one reviewable artifact (a context_snapshot after the "Prepare context"
 * stage, and a report after the run completes), so a Qt ArtifactListModel
 * has something real to display without needing a production provider.
 * artifactService is optional and defaults to nullptr so existing test
 * fixtures that construct MockWorkflowRunner without one keep compiling and
 * simply skip artifact creation.
 */
class MockWorkflowRunner final : public IWorkflowRunner {
public:
    explicit MockWorkflowRunner(
        std::chrono::milliseconds stepDelay = std::chrono::milliseconds {20},
        std::size_t progressSteps = 4,
        ArtifactService* artifactService = nullptr,
        std::shared_ptr<IMockWorkflowGate> gate = nullptr);

    // plan is ignored: the Mock workflow's four stages are always the same
    // regardless of any execution profile (it never routes through one).
    [[nodiscard]] std::vector<RunStage> createStages(
        const RunId& runId, const ResolvedExecutionPlan& plan) const override;
    WorkflowResult run(
        const WorkflowRunContext& context,
        std::stop_token stopToken,
        IRunEventSink& eventSink) override;

private:
    std::chrono::milliseconds stepDelay_;
    std::size_t progressSteps_;
    ArtifactService* artifactService_;
    std::shared_ptr<IMockWorkflowGate> gate_;
};

} // namespace ai::application
