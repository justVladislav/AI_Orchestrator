#pragma once

#include <string>

namespace ai::application {

// Generates a random RFC 4122 version-4 UUID without a platform dependency.
[[nodiscard]] std::string generateUuid();

// Generates a short (16 lowercase hex characters, 64 bits of entropy),
// unpredictable token without a platform dependency - used where
// generateUuid()'s 36-character length itself contributes to a real
// Windows MAX_PATH budget problem (see ManifestProposalOverlay's candidate
// directory naming). Never used where a caller actually needs the
// identity/uniqueness guarantees a full RFC 4122 UUID documents.
[[nodiscard]] std::string generateShortToken();

} // namespace ai::application
