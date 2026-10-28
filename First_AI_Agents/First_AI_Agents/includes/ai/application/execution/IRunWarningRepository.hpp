#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/common/Result.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

// A durable note that something a Run was supposed to leave behind is missing although the Run itself went on.
// Today there is one cause: a DIAGNOSTIC artifact (a report or a provenance note nobody looks up by name) could not be
// stored. A MANDATORY artifact never becomes a warning - its loss already fails the stage that needs it.
inline constexpr const char* kRunWarningDiagnosticArtifactLostCode = "run_warning.diagnostic_artifact_lost";
inline constexpr std::size_t kRunWarningMessageMaxBytes = 1024;
inline constexpr std::size_t kRunWarningArtifactNameMaxBytes = 256;

struct RunWarning {
    std::string id;
    RunId runId;
    std::optional<StageId> stageId;
    std::string code;
    // Bounded (kRunWarningMessageMaxBytes) and human-readable: the artifact name and the store's own error text.
    std::string message;
    // The name the lost artifact was to be stored under; part of the identity of the warning.
    std::string artifactName;
    std::chrono::system_clock::time_point createdAt;
};

// Cuts text to at most maxBytes without splitting a UTF-8 sequence.
[[nodiscard]] std::string boundedUtf8(std::string_view text, std::size_t maxBytes);

/**
 * Persistence port for Run warnings. A warning is identified by (run, stage, code, artifact name): recording the same
 * failure again (a retried write, a re-processed stage) leaves exactly one row.
 */
class IRunWarningRepository {
public:
    virtual ~IRunWarningRepository() = default;

    // Idempotent. true = a new row was written, false = the same warning was already there.
    virtual common::Result<bool> recordWarning(RunWarning warning) = 0;
    // Deterministic order: (createdAt, id).
    [[nodiscard]] virtual common::Result<std::vector<RunWarning>> listWarnings(const RunId& runId) const = 0;
};

} // namespace ai::application
