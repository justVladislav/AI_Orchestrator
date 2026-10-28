#pragma once

#include "ai/application/ApplicationTypes.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ai::application {

// Artifacts are identified the same way as every other aggregate: a UUID
// string, so Qt/JSON/SQL adapters never need a second identifier type.
using ArtifactId = std::string;

// Stable, closed vocabulary. A new kind requires updating toString/
// artifactKindFromString together so persistence and Qt roles cannot drift.
enum class ArtifactKind { ContextSnapshot, Report, Patch, Source, Log, Other };

[[nodiscard]] const char* toString(ArtifactKind kind) noexcept;
[[nodiscard]] std::optional<ArtifactKind> artifactKindFromString(std::string_view value) noexcept;

/**
 * Reviewable output produced by a run (or one of its stages).
 *
 * Only metadata lives here and in SQLite. relativePath is relative to the
 * artifact store root and is never an absolute filesystem path: resolving it
 * to something openable is IArtifactStore's job, not this DTO's.
 */
struct Artifact {
    ArtifactId id;
    RunId runId;
    std::optional<StageId> stageId;
    ArtifactKind kind {ArtifactKind::Other};
    std::string name;
    std::string mediaType;
    std::filesystem::path relativePath;
    std::uint64_t byteSize {0};
    std::string sha256;
    std::chrono::system_clock::time_point createdAt;
};

// One selected project file as it will be recorded inside a
// RunContextSnapshot. content is populated when the file is small enough to
// inline directly in the snapshot; when absent, a caller must have a
// separately justified reference (e.g. the file was already stored as its
// own artifact) - this type never silently drops file bytes.
struct ContextSnapshotFile {
    std::string relativePath;
    std::uint64_t byteSize {0};
    std::optional<std::string> content;
};

/**
 * Provider-neutral record of exactly what context a run executed against:
 * the complete project description, selected project-memory section and ids,
 * complete run task, and selected project files. This is what a future
 * IWorkflowRunner must receive, and
 * what should be persisted/stored as a reviewable artifact for
 * reproducibility - see ContextSnapshot.hpp for the builder that assembles
 * one and enforces an explicit size budget instead of truncating silently.
 */
struct RunContextSnapshot {
    RunId runId;
    ProjectId projectId;
    std::string projectDescription;
    std::string projectMemorySection;
    std::vector<ProjectMemorySegmentId> includedProjectMemorySegmentIds;
    std::string runTask;
    std::vector<ContextSnapshotFile> files;
    std::uint64_t totalByteCount {0};
    std::chrono::system_clock::time_point createdAt;
};

} // namespace ai::application
