#include "ai/application/execution/MockWorkflowRunner.hpp"

#include "ai/application/artifacts/ArtifactService.hpp"
#include "ai/application/context/ContextSnapshot.hpp"

#include <algorithm>
#include <sstream>
#include <thread>
#include <utility>

namespace ai::application {

MockWorkflowRunner::MockWorkflowRunner(
    std::chrono::milliseconds stepDelay,
    std::size_t progressSteps,
    ArtifactService* artifactService,
    std::shared_ptr<IMockWorkflowGate> gate)
    : stepDelay_(std::max(stepDelay, std::chrono::milliseconds {1}))
    , progressSteps_(std::max<std::size_t>(progressSteps, 1))
    , artifactService_(artifactService)
    , gate_(std::move(gate))
{
}

std::vector<RunStage> MockWorkflowRunner::createStages(
    const RunId& runId, const ResolvedExecutionPlan& /*plan*/) const
{
    // Prefix stage IDs with the run UUID so future SQLite primary keys are
    // globally unique while suffixes remain human-readable in diagnostics.
    const auto makeStage = [&runId](std::string suffix, std::string type, std::string title) {
        RunStage stage;
        stage.id = runId + ":" + suffix;
        stage.runId = runId;
        stage.type = std::move(type);
        stage.title = std::move(title);
        stage.description = "Deterministic Mock workflow stage";
        stage.providerId = "mock";
        stage.modelId = "mock-v1";
        return stage;
    };

    return {
        makeStage("prepare-context", "context", "Prepare context"),
        makeStage("architect", "architect", "Architecture"),
        makeStage("implementation", "implementation", "Implementation"),
        makeStage("documentation", "documentation", "Documentation")
    };
}

WorkflowResult MockWorkflowRunner::run(
    const WorkflowRunContext& context,
    std::stop_token stopToken,
    IRunEventSink& eventSink)
{
    // This intentionally mirrors the lifecycle expected from real providers:
    // started -> progress* -> completed, with stop checks before and during work.
    models::TokenUsage totalUsage;
    const double stageCount = static_cast<double>(context.stages.size());

    for (std::size_t stageIndex = 0; stageIndex < context.stages.size(); ++stageIndex) {
        const RunStage& stage = context.stages[stageIndex];
        if (stopToken.stop_requested()) {
            RunEvent cancelled;
            cancelled.type = RunEventType::StageCancelled;
            cancelled.runId = context.run.id;
            cancelled.stageId = stage.id;
            cancelled.runProgress = static_cast<double>(stageIndex) / stageCount;
            cancelled.message = "Mock workflow cancelled";
            eventSink.publish(cancelled);
            return {false, RunStatus::Cancelled, "Mock workflow cancelled", totalUsage};
        }

        RunEvent started;
        started.type = RunEventType::StageStarted;
        started.runId = context.run.id;
        started.stageId = stage.id;
        started.runProgress = static_cast<double>(stageIndex) / stageCount;
        started.message = stage.title + " started";
        eventSink.publish(started);

        for (std::size_t step = 1; step <= progressSteps_; ++step) {
            const auto cancelledInStep = [&]() -> WorkflowResult {
                RunEvent cancelled;
                cancelled.type = RunEventType::StageCancelled;
                cancelled.runId = context.run.id;
                cancelled.stageId = stage.id;
                cancelled.stageProgress = static_cast<double>(step - 1) / static_cast<double>(progressSteps_);
                cancelled.runProgress = (static_cast<double>(stageIndex) + cancelled.stageProgress) / stageCount;
                cancelled.message = stage.title + " cancelled";
                eventSink.publish(cancelled);
                return {false, RunStatus::Cancelled, "Mock workflow cancelled", totalUsage};
            };

            // TEST-ONLY barrier (see IMockWorkflowGate); absent in every production composition. A stop that releases the gate is honoured
            // at once, before the step's delay - cancellation never waits out one more step.
            if (gate_ != nullptr) {
                gate_->beforeStep(context.run.id, stageIndex, step, stopToken);
                if (stopToken.stop_requested()) return cancelledInStep();
            }

            // Small deterministic delays make progress/cancellation observable
            // without introducing network calls into integration tests.
            std::this_thread::sleep_for(stepDelay_);
            if (stopToken.stop_requested()) return cancelledInStep();

            RunEvent progress;
            progress.type = RunEventType::StageProgress;
            progress.runId = context.run.id;
            progress.stageId = stage.id;
            progress.stageProgress = static_cast<double>(step) / static_cast<double>(progressSteps_);
            progress.runProgress = (static_cast<double>(stageIndex) + progress.stageProgress) / stageCount;
            eventSink.publish(progress);
        }

        // Fixed usage validates aggregation without pretending to be real billing.
        const models::TokenUsage stageUsage {10, 5};
        totalUsage.inputTokens += stageUsage.inputTokens;
        totalUsage.outputTokens += stageUsage.outputTokens;

        RunEvent completed;
        completed.type = RunEventType::StageCompleted;
        completed.runId = context.run.id;
        completed.stageId = stage.id;
        completed.stageProgress = 1.0;
        completed.runProgress = static_cast<double>(stageIndex + 1) / stageCount;
        completed.usage = stageUsage;
        completed.message = stage.title + " completed";
        eventSink.publish(completed);

        // The first declared stage is always "Prepare context" (see
        // createStages above); this is the natural point to assemble and
        // store the RunContextSnapshot the run actually executed against, so
        // it becomes a real reviewable artifact even though this deterministic
        // runner ignores the snapshot's contents itself. No file selection UI
        // exists yet, so the snapshot currently has no selected files.
        if (stageIndex == 0 && artifactService_ != nullptr) {
            const auto snapshot = buildRunContextSnapshot(
                context.run.id,
                context.project.id,
                context.project.description,
                context.run.task,
                {},
                std::nullopt,
                {},
                {});
            if (snapshot.success()) {
                static_cast<void>(artifactService_->createDiagnosticArtifact(
                    context.run.id,
                    stage.id,
                    ArtifactKind::ContextSnapshot,
                    "context-snapshot.md",
                    "text/markdown",
                    renderRunContextSnapshot(snapshot.value())));
            }
            // A failed snapshot/artifact write does not fail the Mock run:
            // artifact creation is best-effort output, not a correctness
            // requirement for the deterministic pipeline itself.
        }
    }

    std::ostringstream report;
    report << "# Mock Workflow Report\n\n";
    report << "Run: " << context.run.id << "\n";
    report << "Project: " << context.project.id << "\n";
    report << "Stages completed: " << context.stages.size() << "\n";
    report << "Total input tokens: " << totalUsage.inputTokens << "\n";
    report << "Total output tokens: " << totalUsage.outputTokens << "\n\n";
    report << "## Stage summary\n\n";
    for (const auto& stage : context.stages) {
        report << "- " << stage.title << " (" << stage.type << ")\n";
    }
    const std::string finalReport = report.str();

    if (artifactService_ != nullptr) {
        static_cast<void>(artifactService_->createDiagnosticArtifact(
            context.run.id,
            std::nullopt,
            ArtifactKind::Report,
            "mock-run-report.md",
            "text/markdown",
            finalReport));
    }

    return {true, RunStatus::Completed, "Mock workflow completed", totalUsage, finalReport};
}

} // namespace ai::application
