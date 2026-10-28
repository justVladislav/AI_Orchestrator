#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/common/Result.hpp"

#include <chrono>
#include <functional>
#include <limits>
#include <optional>
#include <string>

namespace ai::application {

// Normalized event vocabulary: workflow implementations publish these events
// without knowing whether the consumer is Qt, CLI, logging, or tests.
enum class RunEventType {
    RunQueued,
    RunStarted,
    StageStarted,
    StageProgress,
    StageCompleted,
    StageFailed,
    StageSkipped,
    StageCancelled,
    RunCancelling,
    RunCancelled,
    RunCompleted,
    RunFailed,
    RunInterrupted,
    RunUsageUpdated,
    // A durable Run warning was just recorded (or could not be: the message says so). Only a notification - the
    // warnings themselves are read from the database (ArtifactService::listWarnings).
    RunWarning
};

struct RunEvent {
    RunEventType type {RunEventType::RunQueued};
    RunId runId;
    std::optional<StageId> stageId;
    double runProgress {0.0};   // Overall workflow progress in [0.0, 1.0].
    double stageProgress {0.0}; // Current stage progress in [0.0, 1.0].
    models::TokenUsage usage;
    std::string message;
    std::chrono::system_clock::time_point timestamp {std::chrono::system_clock::now()};
};

class IRunEventSink {
public:
    virtual ~IRunEventSink() = default;
    virtual void publish(const RunEvent& event) = 0;

    // Production RunManager overrides this to persist the complete cumulative
    // budget snapshot before returning. Test/legacy sinks retain an additive
    // event-only fallback so callers outside the production composition remain
    // source-compatible.
    virtual common::Result<void> persistProviderBudgetUsage(
        const RunId& runId,
        const ProviderExecutionBudgetUsage& usage)
    {
        RunEvent event;
        event.type = RunEventType::RunUsageUpdated;
        event.runId = runId;
        // Parenthesised so a translation unit that includes <windows.h> first (its `max` macro) still compiles this header.
        const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        if (usage.cacheReadInputTokens > maximum - usage.inputTokens) {
            event.usage.inputTokens = maximum;
            event.usage.accountingTrusted = false;
        } else {
            event.usage.inputTokens = usage.inputTokens + usage.cacheReadInputTokens;
        }
        event.usage.outputTokens = usage.outputTokens;
        event.usage.cacheCreationInputTokens = usage.cacheCreationInputTokens;
        event.usage.cacheReadInputTokens = usage.cacheReadInputTokens;
        event.usage.cacheCreationDuration = usage.cacheCreationDuration;
        publish(event);
        return common::Result<void>::ok();
    }
};

// The initial desktop composition needs one observer. A subscription collection
// can replace this alias later without changing workflow contracts.
using RunEventHandler = std::function<void(const RunEvent&)>;

} // namespace ai::application
