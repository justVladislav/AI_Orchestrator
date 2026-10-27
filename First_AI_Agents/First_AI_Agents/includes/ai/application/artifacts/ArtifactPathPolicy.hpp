#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ai::application {

// Shared sanitization rules used by every IArtifactStore implementation so
// in-memory tests and the real filesystem store cannot silently diverge in
// what they consider a safe path segment.

// Keeps only [A-Za-z0-9._-], collapses every other character to '_', strips
// leading dots/dashes (so the result can never be interpreted as ".." or a
// hidden/relative marker), and falls back to "artifact" if nothing usable
// remains (e.g. the input was empty or entirely unsafe characters).
[[nodiscard]] std::string sanitizeArtifactFileName(std::string_view suggestedName);

// Same character allowlist as sanitizeArtifactFileName, used for path
// segments derived from identifiers (runId) rather than user-supplied names.
// Falls back to "unknown" if nothing usable remains.
[[nodiscard]] std::string sanitizePathSegment(std::string_view id);

// Builds a unique, safe artifact-root-relative path of the form
// "<sanitized runId>/<uniquePrefix>-<sanitized name>" so concurrent writes
// for the same run never collide and every path segment is already free of
// traversal characters before any filesystem containment check runs.
[[nodiscard]] std::string buildArtifactRelativePath(
    std::string_view runId,
    std::string_view suggestedName,
    std::uint64_t uniqueCounter);

} // namespace ai::application
