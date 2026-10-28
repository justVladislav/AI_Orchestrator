#pragma once

#include "ai/application/artifacts/IArtifactRepository.hpp"
#include "ai/application/artifacts/IArtifactStore.hpp"
#include "ai/application/execution/IRunWarningRepository.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

/**
 * Public application facade for artifact read models and creation.
 *
 * This is what a future Qt ArtifactListModel/RunController will call through
 * BackendRuntime::artifactService(), and what workflow runners (starting
 * with MockWorkflowRunner) use to persist reviewable output during a run.
 * Content always flows through IArtifactStore; only metadata flows through
 * IArtifactRepository. Callers never see or construct a raw absolute path
 * except through resolveArtifactPath(), which re-validates containment.
 */
class ArtifactService {
public:
    // Told about a warning right after it was recorded (recordError empty), or when it could NOT be recorded
    // (recordError set: the warning is then not durable and this call is the only trace). Called on the thread that
    // lost the artifact, with no lock held.
    using RunWarningObserver =
        std::function<void(const RunWarning& warning, const std::optional<common::Error>& recordError)>;

    // warnings is optional: without it a lost diagnostic artifact can only be announced to the observer.
    ArtifactService(
        IArtifactRepository& repository,
        IArtifactStore& store,
        IRunWarningRepository* warnings = nullptr);

    // Set once while the runtime is composed, before any concurrent use.
    void setWarningObserver(RunWarningObserver observer) { warningObserver_ = std::move(observer); }

    // The durable warnings of a Run, oldest first. Empty when no warning store is configured.
    [[nodiscard]] common::Result<std::vector<RunWarning>> listWarnings(const RunId& runId) const;

    [[nodiscard]] common::Result<std::vector<Artifact>> listArtifacts(const RunId& runId) const;
    [[nodiscard]] common::Result<Artifact> getArtifact(const ArtifactId& id) const;

    // The safe "open target" for a Qt adapter: re-resolves and re-validates
    // canonical containment inside the artifact store root before returning
    // an absolute path, rather than trusting the stored relative path alone.
    [[nodiscard]] common::Result<std::filesystem::path> resolveArtifactPath(const ArtifactId& id) const;

    // In-app, read-only alternative to resolveArtifactPath(): returns the
    // artifact's full text content instead of a path to hand to an external
    // opener. Used by the run-details "View context" viewer, which must
    // never let the caller edit or write back to the artifact.
    [[nodiscard]] common::Result<std::string> getArtifactContent(const ArtifactId& id) const;

    // Writes content through the store, then records its metadata. If the
    // metadata insert fails after a successful write, the orphaned file is
    // acceptable for this MVP: it never becomes visible through
    // listArtifacts/getArtifact because nothing references its id.
    common::Result<Artifact> createArtifact(
        const RunId& runId,
        std::optional<StageId> stageId,
        ArtifactKind kind,
        std::string name,
        std::string mediaType,
        std::string_view content);

    // For artifacts nothing looks up by name (reports, provenance notes): the Run goes on without them, but the loss
    // must not be silent. Behaves exactly like createArtifact(); when it fails, a RunWarning is recorded durably
    // (once per run/stage/artifact name) and the observer is told. A MANDATORY artifact must keep using
    // createArtifact() and fail its stage. If the warning itself cannot be recorded, the returned error says so
    // (code artifact.diagnostic_warning_unrecorded) and the observer receives the record error.
    common::Result<Artifact> createDiagnosticArtifact(
        const RunId& runId,
        std::optional<StageId> stageId,
        ArtifactKind kind,
        std::string name,
        std::string mediaType,
        std::string_view content);

private:
    IArtifactRepository& repository_;
    IArtifactStore& store_;
    IRunWarningRepository* warnings_ {nullptr};
    RunWarningObserver warningObserver_;
};

} // namespace ai::application
