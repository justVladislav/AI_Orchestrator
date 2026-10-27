#include "ai/application/artifacts/ArtifactService.hpp"

#include "ai/application/IdGenerator.hpp"
#include "ai/application/Sha256.hpp"

#include <chrono>
#include <utility>

namespace ai::application {

ArtifactService::ArtifactService(
    IArtifactRepository& repository,
    IArtifactStore& store,
    IRunWarningRepository* warnings)
    : repository_(repository)
    , store_(store)
    , warnings_(warnings)
{
}

common::Result<std::vector<RunWarning>> ArtifactService::listWarnings(const RunId& runId) const
{
    if (warnings_ == nullptr) return common::Result<std::vector<RunWarning>>::ok({});
    return warnings_->listWarnings(runId);
}

common::Result<std::vector<Artifact>> ArtifactService::listArtifacts(const RunId& runId) const
{
    return repository_.listArtifacts(runId);
}

common::Result<Artifact> ArtifactService::getArtifact(const ArtifactId& id) const
{
    const auto found = repository_.findArtifact(id);
    if (!found.success()) {
        return common::Result<Artifact>::failure(*found.error());
    }
    if (!found.value().has_value()) {
        return common::Result<Artifact>::failure({"artifact.not_found", "Artifact not found"});
    }
    return common::Result<Artifact>::ok(*found.value());
}

common::Result<std::filesystem::path> ArtifactService::resolveArtifactPath(const ArtifactId& id) const
{
    const auto artifact = getArtifact(id);
    if (!artifact.success()) {
        return common::Result<std::filesystem::path>::failure(*artifact.error());
    }
    // The store, not this service, owns the canonical-containment recheck:
    // it knows the real root and can detect a symlink escape at resolve time
    // even if the row itself looked safe when it was written.
    return store_.resolveAbsolutePath(artifact.value().relativePath);
}

common::Result<std::string> ArtifactService::getArtifactContent(const ArtifactId& id) const
{
    const auto artifact = getArtifact(id);
    if (!artifact.success()) {
        return common::Result<std::string>::failure(*artifact.error());
    }
    auto content = store_.readArtifact(artifact.value().relativePath);
    if (!content.success()) return content;
    if (content.value().size() != artifact.value().byteSize
        || sha256Hex(content.value()) != artifact.value().sha256) {
        return common::Result<std::string>::failure({
            "artifact.integrity_mismatch",
            "Artifact content does not match its durable size and hash"});
    }
    return content;
}

common::Result<Artifact> ArtifactService::createArtifact(
    const RunId& runId,
    std::optional<StageId> stageId,
    ArtifactKind kind,
    std::string name,
    std::string mediaType,
    std::string_view content)
{
    const auto written = store_.writeArtifact(runId, name, content);
    if (!written.success()) {
        return common::Result<Artifact>::failure(*written.error());
    }

    Artifact artifact;
    artifact.id = generateUuid();
    artifact.runId = runId;
    artifact.stageId = std::move(stageId);
    artifact.kind = kind;
    artifact.name = std::move(name);
    artifact.mediaType = std::move(mediaType);
    artifact.relativePath = written.value().relativePath;
    artifact.byteSize = written.value().byteSize;
    artifact.sha256 = written.value().sha256;
    artifact.createdAt = std::chrono::system_clock::now();

    const auto inserted = repository_.insertArtifact(artifact);
    if (!inserted.success()) {
        // Filesystem and SQLite cannot share one transaction. Compensate a
        // metadata failure so an invisible orphan file is not left behind.
        const auto removed = store_.removeArtifact(artifact.relativePath);
        if (!removed.success()) {
            return common::Result<Artifact>::failure({
                "artifact.compensation_failed",
                inserted.error()->message + "; cleanup failed: " + removed.error()->message});
        }
        return common::Result<Artifact>::failure(*inserted.error());
    }
    return inserted;
}

common::Result<Artifact> ArtifactService::createDiagnosticArtifact(
    const RunId& runId,
    std::optional<StageId> stageId,
    ArtifactKind kind,
    std::string name,
    std::string mediaType,
    std::string_view content)
{
    // The name is read after the call: createArtifact() takes it by value.
    const std::string artifactName = name;
    auto created = createArtifact(runId, stageId, kind, std::move(name), std::move(mediaType), content);
    if (created.success()) return created;

    RunWarning warning;
    warning.id = generateUuid();
    warning.runId = runId;
    warning.stageId = std::move(stageId);
    warning.code = kRunWarningDiagnosticArtifactLostCode;
    warning.artifactName = boundedUtf8(artifactName, kRunWarningArtifactNameMaxBytes);
    warning.message = boundedUtf8(
        "Diagnostic artifact '" + warning.artifactName + "' was not saved ("
            + created.error()->code + ": " + created.error()->message + ")",
        kRunWarningMessageMaxBytes);
    warning.createdAt = std::chrono::system_clock::now();

    std::optional<common::Error> recordError;
    bool announce = true;
    if (warnings_ == nullptr) {
        recordError = common::Error {"run_warning.store_unavailable", "No run warning store is configured"};
    } else {
        const auto recorded = warnings_->recordWarning(warning);
        if (!recorded.success()) {
            recordError = *recorded.error();
        } else {
            // false = the very same loss was recorded before (and announced then).
            announce = recorded.value();
        }
    }
    if (announce && warningObserver_) warningObserver_(warning, recordError);

    if (recordError.has_value()) {
        return common::Result<Artifact>::failure({
            "artifact.diagnostic_warning_unrecorded",
            created.error()->message + "; the warning could not be recorded: " + recordError->message});
    }
    return created;
}

} // namespace ai::application
