#include "ai/application/ManifestProposalContentHash.hpp"

#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/Sha256.hpp"

#include <sstream>

namespace ai::application {

std::string computeManifestProposalContentHash(
    const ChangeSetId& proposalId,
    const std::vector<ManifestProposalContentHashChangeInput>& changes)
{
    // Exact serialization this module's own doc comment promises to keep
    // stable: proposalId, then per change - operation, relativePath,
    // sourcePath (Rename only), and (Create/Modify only) the SHA-256 of its
    // proposed content - each field NUL-delimited, each change newline-
    // delimited, the whole material then hashed once more with SHA-256.
    std::ostringstream material;
    material << proposalId << '\n';
    for (const auto& change : changes) {
        material << toString(change.operation) << '\0'
                  << detail::genericUtf8(change.relativePath) << '\0';
        if (change.sourcePath.has_value()) {
            material << detail::genericUtf8(*change.sourcePath);
        }
        material << '\0';
        if (change.contentSha256.has_value()) {
            material << *change.contentSha256;
        }
        material << '\n';
    }
    return sha256Hex(material.str());
}

} // namespace ai::application
